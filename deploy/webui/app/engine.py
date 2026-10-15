# -*- coding: utf-8 -*-
"""引擎进程管理：启动参数、起停、探活、显存/内存、日志解析。

从 Windows 托盘版迁移到 Linux：
  - subprocess.Popen 替代 CREATE_NO_WINDOW
  - ss -ltnp 替代 netstat -ano
  - proc.terminate() 替代 taskkill
  - /proc/meminfo 替代 ctypes.windll.kernel32
  - 日志解析适配 ninfer 格式
"""
import json
import os
import re
import socket
import subprocess
import threading
import time
import urllib.request

from .config import *   # noqa: F401,F403
from . import config as _cfg_mod
from .logutil import log


def _tail_bytes(path, n):
    """Read the last n bytes of a file. f.read()[-n:] loads the whole file
    first (engine.log rotates at 20 MB)."""
    try:
        with open(path, 'rb') as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - n))
            return f.read()
    except Exception:
        return b''


def engine_stage():
    """('loading', 42) while the engine is still reading weights, else ('', 0)."""
    data = _tail_bytes(log_path(), 120000).decode('utf-8', 'replace')
    ready = data.rfind('engine ready')
    load = data.rfind('loading weights')
    if ready < 0 and load < 0:
        return '', 0
    if ready < 0 or (load >= 0 and load > ready):
        ls = data.rfind('\n', 0, load) + 1
        m = re.match(r'.*?(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})', data[ls:ls + 40])
        if m:
            try:
                ts = time.mktime(time.strptime(m.group(1), '%Y-%m-%d %H:%M:%S'))
                if time.time() - ts > 180:
                    return '', 0
            except Exception:
                pass
        # ninfer 日志没有百分比，只有 "loading weights | 18.5 GiB"
        return 'loading', 0
    return '', 0


def pid_on_port(port):
    """Find PID listening on port using ss (Linux)."""
    try:
        raw = subprocess.run(['ss', '-ltnp'], capture_output=True, timeout=10).stdout
        out = raw.decode('utf-8', 'replace') if isinstance(raw, bytes) else raw
    except Exception as e:
        log('pid_on_port 失败: %s' % e)
        return None
    for line in out.splitlines():
        if ':%d ' % port in line or ':%d\t' % port in line:
            m = re.search(r'pid=(\d+)', line)
            if m:
                return int(m.group(1))
    return None


# ---------------- 引擎参数 ----------------

def args_for(c=None):
    c = c or cfg()
    a = [MODEL_NINFER, '--host', HOST, '--port', str(cur_port()),
         '--model-id', 'qwen3.8-27b',
         '--max-context', str(CTX),
         '--kv-capacity', str(POOL),
         '--kv-dtype', 'k8v4',
         '--host-kv-mib', '65536',
         '--prefill-chunk', '1024',
         '--max-concurrency', str(c.get('parallel', 1)),
         '--max-shared-prefixes', '16',
         # 2026-10-13: 1552 回退 256。StateImage 是固定 183 MB/条的全价快照（与对话长度无关，
         # 大头是 dflash2 sliding-window 局部态），HostStatePool 一次性全价预分配、无按需模式：
         # 1552 x 183 MB = 283 GiB pinned，且每次重启多花 2m18s pin。
         # 命中率收益来自 --auto-long-anchors（anchors 2->4），不来自 slot 数；256 条已远超实际并发。
         '--host-state-slots', '256',
         '--max-private-continuations', '256',
         '--derive-session-keys',
         '--auto-prefix-grid',
         '--recency-eviction',
         '--value-aware-demote',
         # 2026-10-12: 自动 anchor。长对话在消息边界自动落 anchor，endpoint 被挤出后可从最近
         # anchor 续，不必整段重算 prefill。anchors 数随之从 2 变 4（model_instance.cpp:333）。
         '--auto-long-anchors',
         '--long-anchor-spacing', '2048']
    if c['mtp']:
        a += ['--spec', 'dflash2', '--draft-tokens', str(c['n_max'])]
    else:
        a += ['--spec', 'none']
    if not c.get('think', False):
        a += ['--default-reasoning-effort', 'none']
    else:
        a += ['--default-reasoning-effort', 'high']
    a += ['--default-max-tokens', str(c.get('out_len', OUT_LEN))]
    # 采样参数写进 argv，所有客户端都受保护
    a += ['--temperature', '0.7', '--top-p', '0.9', '--top-k', '20',
          '--presence-penalty', '0']
    return a


def exe_cwd(c=None):
    c = c or cfg()
    return EXE_NINFER, CWD_NINFER


def log_path(c=None):
    return os.path.join(BASE, 'engine.log')


PORT_CACHE = {}


def port_open(port, timeout=0.6):
    now = time.time()
    hit = PORT_CACHE.get(port)
    if hit and (now - hit[0]) < 4.0:
        return hit[1]
    try:
        with socket.create_connection(('127.0.0.1', port), timeout=timeout):
            ok = True
    except OSError:
        ok = False
    PORT_CACHE[port] = (now, ok)
    return ok


def port_cache_clear():
    PORT_CACHE.clear()
    HTTP_CACHE.clear()


HTTP_CACHE = {}

_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def http_ok(port, path, timeout=0.6):
    key = (port, path)
    hit = HTTP_CACHE.get(key)
    if hit and (time.time() - hit[0]) < 1.5:
        return hit[1]
    try:
        req = urllib.request.Request('http://127.0.0.1:%d%s' % (port, path))
        with _OPENER.open(req, timeout=timeout) as r:
            ok = r.status
    except Exception:
        ok = 0
    HTTP_CACHE[key] = (time.time(), ok)
    return ok


def engine_stats():
    """直连引擎 /stats 取原始 JSON（webui 不缓存，面板每次拉新）。"""
    try:
        req = urllib.request.Request(
            'http://127.0.0.1:%d/stats' % cur_port())
        with _OPENER.open(req, timeout=3) as r:
            return json.loads(r.read().decode('utf-8', 'replace'))
    except Exception:
        return None


# 在线清缓存：直调引擎 admin 端点（POST→202→轮询 GET 至终态）。
# 仅 try_idle 对外默认；drain/abort 由调用方显式传 mode。
# 返回 dict（done 明细 / busy / failed / error），绝不抛异常。
def engine_cache_clear(mode='try_idle', scope=None, timeout=60):
    port = cur_port()
    body = json.dumps({
        'mode': mode,
        'scope': scope or ['all'],
    }).encode('utf-8')
    try:
        req = urllib.request.Request(
            'http://127.0.0.1:%d/v1/admin/cache/clear' % port,
            data=body, headers={'Content-Type': 'application/json'})
        with _OPENER.open(req, timeout=10) as r:
            enq = json.loads(r.read().decode('utf-8', 'replace'))
    except Exception as e:
        return {'ok': False, 'error': 'enqueue-failed: %s' % str(e)[:120]}
    cid = enq.get('clear_id')
    if cid is None:
        return {'ok': False, 'error': 'no-clear-id'}
    t0 = time.time()
    last = None
    while time.time() - t0 < timeout:
        time.sleep(0.5)
        try:
            req = urllib.request.Request(
                'http://127.0.0.1:%d/v1/admin/cache/clear/%s' % (port, cid))
            with _OPENER.open(req, timeout=10) as r:
                last = json.loads(r.read().decode('utf-8', 'replace'))
        except urllib.error.HTTPError as e:
            if e.code == 409:
                try:
                    detail = json.loads(e.read().decode('utf-8', 'replace'))
                except Exception:
                    detail = {}
                msg = detail.get('message') or detail.get('error') or 'engine busy'
                return {'ok': False, 'busy': True, 'clear_id': cid,
                        'error': 'busy: %s' % str(msg)[:140]}
            return {'ok': False, 'error': 'poll-http-%s' % e.code}
        except Exception as e:
            return {'ok': False, 'error': 'poll-failed: %s' % str(e)[:120]}
        if (last or {}).get('state') in ('done', 'failed'):
            if last.get('state') == 'done':
                return {'ok': True, 'clear_id': cid,
                        'kv_before': last.get('kv_tokens_before'),
                        'kv_after': last.get('kv_tokens_after'),
                        'host_before': last.get('host_bytes_before'),
                        'host_after': last.get('host_bytes_after'),
                        'duration_ms': last.get('duration_ms'),
                        'failed_requests': last.get('failed_requests'),
                        'detail': last.get('detail')}
            return {'ok': False, 'clear_id': cid,
                    'error': 'clear-failed: %s' % str(last.get('detail'))[:140]}
    return {'ok': False, 'clear_id': cid, 'error': 'poll-timeout'}


def _num(value):
    try:
        return int(value or 0)
    except (TypeError, ValueError):
        return 0


def _pages(node):
    if not isinstance(node, dict):
        return 0
    return _num(node.get("pages"))


def _bytes(node):
    if not isinstance(node, dict):
        return 0
    return _num(node.get("bytes"))


def cache_meter():
    """把引擎 /stats 折算成运行状况页的缓存观测，给长缓存优化用。

    MainKV D2H/H2D 展示主 KV 下沉与召回；BackendKV 是独立资源，不能代替主 KV。
    spill_pages 是 pressure 下沉发布计数；demote/evict/drop/degrade 按 owner 归因，
    不能互相替代，也不能单独证明某条会话可复用。
    """
    st = engine_stats()
    if not st:
        return None
    ctx = st.get("context_cache") or {}
    cap = st.get("capacity") or {}
    occ = ctx.get("occupancy") or st.get("occupancy") or {}
    pre = ctx.get("pressure") or {}
    main = ctx.get("main_kv_transfers") or {}
    state_tr = ctx.get("state_transfers") or {}
    sel = ctx.get("selections") or {}

    d2h = main.get("d2h") or {}
    h2d = main.get("h2d") or {}
    return {
        # 条数：已用 / 上限
        "slots_used": _num(occ.get("device_state_slots")) + _num(occ.get("host_state_slots")),
        "slots_max": _num(cap.get("device_state_slots")) + _num(cap.get("host_state_slots")),
        # 容量：已用 / 上限（token；occupancy 按页计，x64 换算）
        "tokens_used": _num(occ.get("device_main_kv_pages")) * _num(cap.get("kv_page_tokens") or 64),
        "tokens_max": _num(cap.get("kv_capacity_tokens")),
        # 逐出、丢弃与下沉发布
        "evicted": _num(pre.get("private_owners_evicted")) + _num(pre.get("shared_owners_evicted")),
        "dropped": _num(pre.get("checkpoints_dropped")),
        "spill_pages": _num(pre.get("spill_pages")),
        "demoted": _num(pre.get("private_owners_demoted")),
        "degraded": _num(pre.get("private_owners_degraded")) + _num(pre.get("shared_owners_degraded")),
        # 主 KV 下沉 / 召回
        "d2h_pages": _pages(d2h),
        "d2h_bytes": _bytes(d2h),
        "h2d_pages": _pages(h2d),
        "h2d_bytes": _bytes(h2d),
        # Host 实际占用（occupancy，不是预算上限）
        "host_kv_bytes": _num(occ.get("host_kv_bytes")),
        "host_state_slots": _num(occ.get("host_state_slots")),
        # StateImage 下沉 / 召回次数（与 KV 页计数口径不同）
        "state_d2h": _num((state_tr.get("d2h") or {}).get("count")),
        "state_h2d": _num((state_tr.get("h2d") or {}).get("count")),
        # 复用路径汇总：只看趋势，不代替逐会话命中
        "reused_tokens": _num(sel.get("reused_prompt_tokens")),
        "sel_root": _num(sel.get("root")),
        "sel_shared": _num(sel.get("shared_stable_prefix")),
        "sel_endpoint": _num(sel.get("private_endpoint")) + _num(sel.get("private_turn_closure"))
        + _num(sel.get("private_response_replay")) + _num(sel.get("private_long_anchor")),
    }
def vram():
    if time.time() - GPU_CACHE['tv'] < 3.0:
        return GPU_CACHE['vram']
    try:
        raw = subprocess.run(['nvidia-smi', '--query-gpu=index,memory.used,memory.total',
                              '--format=csv,noheader,nounits'],
                             capture_output=True, timeout=10).stdout
        if isinstance(raw, bytes):
            raw = raw.decode('utf-8', 'replace')
        # 找到 GPU 2（我们用的 GPU）
        for line in raw.strip().splitlines():
            parts = [x.strip() for x in line.split(',')]
            if len(parts) >= 3 and parts[0] == GPU_ID:
                u, t = int(parts[1]), int(parts[2])
                GPU_CACHE['tv'] = time.time(); GPU_CACHE['vram'] = (u, t)
                return u, t
        #  fallback：取第一行
        parts = [x.strip() for x in raw.strip().splitlines()[0].split(',')]
        if len(parts) >= 3:
            u, t = int(parts[1]), int(parts[2])
        else:
            u, t = int(parts[0]), int(parts[1])
        GPU_CACHE['tv'] = time.time(); GPU_CACHE['vram'] = (u, t)
        return u, t
    except Exception:
        GPU_CACHE['tv'] = time.time() - 7.0
        return -1, -1


def ram():
    """Read memory info from /proc/meminfo (Linux)."""
    if time.time() - GPU_CACHE['tr'] < 3.0:
        return GPU_CACHE['ram']
    try:
        with open('/proc/meminfo', 'r') as f:
            lines = f.readlines()
        info = {}
        for line in lines:
            parts = line.split()
            if len(parts) >= 2:
                info[parts[0].rstrip(':')] = int(parts[1])  # kB
        total = info.get('MemTotal', 0)
        avail = info.get('MemAvailable', 0)
        if total and avail:
            load = round((total - avail) * 100.0 / total)
            v = (load, round(avail / 1024.0 / 1024.0, 1), round(total / 1024.0 / 1024.0, 1))
            GPU_CACHE['tr'] = time.time(); GPU_CACHE['ram'] = v
            return v
    except Exception:
        pass
    return (0, 0.0, 0.0)


def is_running():
    if state['proc'] is not None and state['proc'].poll() is None:
        return True
    return bool(state.get('adopted')) and port_open(cur_port())


def uptime_s():
    return int(time.time() - state['started']) if state['started'] and is_running() else 0


def uptime():
    s = uptime_s()
    return '-' if not s else '%02d:%02d:%02d' % (s // 3600, (s % 3600) // 60, s % 60)


def probe():
    if is_running():
        state['status'] = 'running' if port_open(cur_port()) else 'starting'
    else:
        state['status'] = 'stopped'
        if state.get('run_cfg') is not None:
            state['run_cfg'] = None
    return state['status']


def start():
    sync_cfg_from_disk()
    if is_running():
        if state.get('need_restart'):
            stop()
        else:
            log('已在运行，忽略启动')
            return None
    port_cache_clear()
    if port_open(cur_port()):
        _rc = dict(cfg())
        _rc['engine'] = 'ninfer'
        _rc['approx'] = True
        state.update(proc=None, logfh=None, started=time.time(), last_boot=time.time(),
                     status='running', trimmed=True, adopted=True, need_restart=False,
                     run_cfg=_rc)
        log('端口 %d 已有引擎 → 接管' % cur_port())
        port_cache_clear()
        return None
    lp = log_path()
    os.makedirs(os.path.dirname(lp), exist_ok=True)
    try:
        if os.path.getsize(lp) > 20 * 1024 * 1024:
            os.replace(lp, lp + '.old')
    except Exception:
        pass
    fh = open(lp, 'ab', buffering=0)
    exe, cwd = exe_cwd()
    env = dict(os.environ)
    env['CUDA_VISIBLE_DEVICES'] = GPU_ID
    for key in (
        "NINFER_KV_RING", "NINFER_KV_WINDOW", "NINFER_KV_RETRIEVE",
        "NINFER_TERNARY_KVMEM", "NINFER_HOST_PAGEABLE",
        "NINFER_KV_REUSE_HOSTBACKED",
    ):
        env.pop(key, None)
    # KVMem 五开关（一个配置，缺一不可）
    # 注意：KVMem 环要求 --max-concurrency 1，并发 >1 时必须禁用

    # 2026-10-13: NINFER_KV_REUSE_HOSTBACKED 必须无条件打开，不能只在 parallel==1 时设。
    # inspect_admission() 有一道静态容量门：kv_pages_for_frontier(checkpoint->frontier) >
    # physical_pool().usable_pages() 时，若此开关未设就直接 return nullopt -> 降级 root -> cache 0。
    # 104k prompt ≈ 1631 页，超过 GPU 池可用页数；而 5.4k prompt ≈ 86 页畅通无阻 —— 这正是大会话
    # 每轮都 0% 命中、小会话 99.9% 命中的根因。开关打开后，越过门的 checkpoint 只要每页 Host-resident
    # 且 replica current，就走已有 H2D 恢复路径（restore_kv_pages_from_host），不再是死路。
    # 与 KVMem 环无关：环有自己的 NINFER_KV_WINDOW/RING，且要求 --max-concurrency 1。
    env['NINFER_HOST_PAGEABLE'] = '1'
    env['NINFER_KV_REUSE_HOSTBACKED'] = '1'
    # 2026-10-13: Host StateImage 跳过 dflash_local（滑动窗口草稿 K/V，占比约 21%）。
    # 实测（GPU4, 32 槽）：pin 5.84 -> 4.59 GiB；256 槽折算 46.7 -> 36.6 GiB，省约 10 GiB。
    # 该分量可由一个窗口的 token 重算，快照不必携带；device 侧池不动，恢复时清零重建。
    # 对照实验：skip on/off 两臂在全新引擎上的召回率逐项一致（t1..t5 = 0/50/67/75/80%）。
    # 回滚：删掉这一行重启即为老行为（qwen3_5::plan_host_state_image 默认不含跳过）。
    env['NINFER_STATE_SKIP_DFLASH_HOST'] = '1'
    # KVMem 五开关（一个配置，缺一不可）
    # 注意：KVMem 环要求 --max-concurrency 1，并发 >1 时必须禁用
    if cfg().get('parallel', 1) == 1:
        env.update({
            'NINFER_KV_WINDOW': '16384',
            'NINFER_KV_RETRIEVE': '8192',
            'NINFER_KV_RING': '1',
        })
    proc = subprocess.Popen([exe] + args_for(), cwd=cwd, stdout=fh,
                            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                            env=env, start_new_session=True)
    state.update(proc=proc, logfh=fh, started=time.time(), last_boot=time.time(),
                 status='starting', trimmed=False, adopted=False, need_restart=False,
                 run_cfg=dict(cfg()))
    port_cache_clear()
    log('启动 %s-%dK (MTP=%s 路数=%s pid=%s)'
        % (cfg()['engine'], cfg()['ctx'] // 1024, cfg()['mtp'], cfg()['n_max'], proc.pid))
    return proc


def stop():
    p = state['proc']
    if p:
        p.terminate()
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait(timeout=5)
        port_cache_clear()
        log('已停止 pid=%s' % p.pid)
    else:
        pid = pid_on_port(cur_port())
        if pid:
            try:
                os.kill(pid, 15)  # SIGTERM
                time.sleep(2)
                if pid_on_port(cur_port()):
                    os.kill(pid, 9)  # SIGKILL
            except Exception as e:
                log('停止外部引擎失败: %s' % e)
            log('已停止外部引擎 pid=%s（端口 %d）' % (pid, cur_port()))
    if state['logfh']:
        try:
            state['logfh'].close()
        except Exception:
            pass
    MODEL_CACHE['id'] = ''
    state.update(proc=None, logfh=None, started=None, status='stopped',
                 trimmed=False, adopted=False, run_cfg=None)


def restart():
    stop()
    port_cache_clear()
    for _ in range(40):
        if not port_open(cur_port(), 0.4):
            break
        time.sleep(0.5)
    port_cache_clear()
    start()
    log('已重启引擎')


# ---------------- 看门狗 ----------------
HEAL = {'busy': False}


def worker_death_detected():
    if cfg().get('engine') != 'ninfer' or not is_running():
        return False
    try:
        lines = tail_lines(300)
    except Exception:
        return False
    if not lines or lines[0].startswith('('):
        return False
    last_boot = -1
    for i, ln in enumerate(lines):
        if 'listening on' in ln:
            last_boot = i
    if last_boot < 0:
        return False
    return any('serve-fail:' in ln for ln in lines[last_boot + 1:])


def _heal_allowed():
    return time.time() - state.get('last_heal', 0.0) > 120.0


def _fire_heal(why):
    if HEAL['busy']:
        return
    HEAL['busy'] = True
    state['last_heal'] = time.time()
    log('自动恢复：%s -> 自动重启引擎' % why)

    def run():
        time.sleep(2.0)
        try:
            restart()
            log('自动恢复完成')
        except Exception as e:
            log('自动恢复失败: %s' % e)
        finally:
            HEAL['busy'] = False
    threading.Thread(target=run, daemon=True).start()


def watchdog_check():
    try:
        p = state['proc']
        if p is not None and p.poll() is not None:
            if _heal_allowed():
                _fire_heal('引擎进程意外退出 rc=%s' % p.returncode)
            return
        if worker_death_detected():
            if _heal_allowed():
                _fire_heal('推理 worker 死亡（日志出现 serve-fail）')
    except Exception as e:
        try:
            log('watchdog 异常: %s' % e)
        except Exception:
            pass


# ---------------- 日志解析（ninfer 格式）----------------
RE_REQ = re.compile(r'req#(\d+) done \|.*?prompt ([\d,]+) \| output (\d+)')
RE_TTFT = re.compile(r'TTFT ([\d.]+) ms')
RE_PREF = re.compile(r'prefill ([\d.]+)([km]?) tok/s')
RE_DEC = re.compile(r'decode ([\d.]+)([km]?) tok/s')
RE_ACC = re.compile(r'(?:dflash2|mtp) accepted (\d+)/(\d+)')
RE_CACHE = re.compile(r'cache ([\d,]+)')

LOG_STATE = {'path': None, 'off': 0, 'lines': []}


def _tok_rate(raw):
    m = re.match(r'([\d.]+)([km]?)$', str(raw))
    if not m:
        return raw
    return '%.1f' % (float(m.group(1)) * {'k': 1000.0, 'm': 1000000.0, '': 1.0}[m.group(2)])


def read_log():
    p = log_path()
    try:
        st = os.stat(p)
    except Exception:
        return ['(当前组合还没跑过，暂无日志)']
    S = LOG_STATE
    if S['path'] != p or st.st_size < S['off']:
        S['path'] = p
        S['off'] = max(0, st.st_size - 300000)
        S['lines'] = []
    if st.st_size > S['off']:
        try:
            with open(p, 'rb') as f:
                f.seek(S['off'])
                data = f.read()
            S['off'] += len(data)
            new = [l for l in data.decode('utf-8', 'replace').splitlines() if l.strip()]
            S['lines'] = (S['lines'] + new)[-4000:]
        except Exception:
            pass
    return S['lines'] if S['lines'] else ['(当前组合还没跑过，暂无日志)']


def tail_lines(n=80):
    ls = read_log()
    return ls[-n:] if ls else ['(当前组合还没跑过，暂无日志)']


def _line_time(line):
    m = re.match(r'\d{4}-\d{2}-\d{2} (\d{2}:\d{2}:\d{2})', line)
    return m.group(1) if m else '-'


def _fmt_row(r):
    t = r['t']

    def num(v):
        try:
            return '{:,}'.format(int(str(v).replace(',', '')))
        except Exception:
            return None

    def spd(v):
        try:
            return '{:,.1f}'.format(float(v))
        except Exception:
            return None

    return (t, num(r['in']) or r['in'], num(r['out']) or r['out'],
            num(r['cache']) or r['cache'], spd(r['pre']) or r['pre'],
            spd(r['dec']) or r['dec'])


def recent_requests(n=30):
    rows = []
    for line in read_log()[-6000:]:
        m = RE_REQ.search(line)
        if m:
            t = _line_time(line)
            ttft = RE_TTFT.search(line)
            pre = RE_PREF.search(line)
            dec = RE_DEC.search(line)
            ca = RE_CACHE.search(line)
            rows.append({'t': t, 'in': m.group(2).replace(',', ''),
                         'out': m.group(3),
                         'cache': ca.group(1).replace(',', '') if ca else '-',
                         'pre': _tok_rate(pre.group(1) + pre.group(2)) if pre else '-',
                         'dec': _tok_rate(dec.group(1) + dec.group(2)) if dec else '-'})
    rows.reverse()
    return [_fmt_row(r) for r in rows[:n]]


def boot_free():
    data = _tail_bytes(log_path(), 200000).decode('utf-8', 'replace')
    for line in reversed(data.splitlines()):
            if 'capacity' in line and 'free' in line:
                m = re.search(r'free ([\d.]+) GiB', line)
                if m:
                    return int(float(m.group(1)) * 1024)  # GiB -> MiB
    return None
