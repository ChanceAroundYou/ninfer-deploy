# -*- coding: utf-8 -*-
"""HTTP 面板：/api 状态聚合、/do 动作、/chat 代理（含护栏落地）、静态页面。

从 Windows 托盘版迁移到 Linux webui：
  - 去掉托盘相关引用
  - 去掉 WSL 管理
  - 去掉 trim/free（Windows 专属）
  - 保留核心面板功能
"""
import json
import os
import re
import time
import threading
import urllib.request
import urllib.parse
import urllib.error
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from .config import *   # noqa: F401,F403
from . import config as _cfg_mod
from .engine import *   # noqa: F401,F403
from .engine import _OPENER   # ★ 下划线名不会被 import * 带入，须显式
from .engine import *   # noqa
from .logutil import log

_PAGE_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                         'app', 'page')
_STATIC = {'/static/style.css': ('style.css', 'text/css; charset=utf-8'),
           '/static/app.js': ('app.js', 'application/javascript; charset=utf-8')}


def page_index():
    with open(os.path.join(_PAGE_DIR, 'index.html'), 'rb') as f:
        return f.read()


def page_static(path):
    name, ctype = _STATIC[path]
    with open(os.path.join(_PAGE_DIR, name), 'rb') as f:
        return ctype, f.read()


def guard_state(content, prev_content, want_file=False):
    t = (content or '').strip()
    if len(t) < 10 or (want_file and len(t) < 2000):
        return 'EMPTY'
    if '<svg' in t and '</svg>' not in t:
        return 'TRUNCATED'
    if prev_content and t == prev_content.strip():
        return 'FIXED_POINT'
    return 'OK'


def guard_should_retry(state, retry_n):
    return state != 'OK' and retry_n < 2


def guard_reply(state):
    return {
        'EMPTY': '这一轮没有产出可用的内容（正文过短或为空）。请直接给出完整答案，不要只写计划。',
        'TRUNCATED': '这一轮的产物不完整（内容被截断，没有收尾）。请重新完整输出，不要中途停止。',
        'FIXED_POINT': '这一轮与上一轮逐字相同，等于没有进展。请换一个角度，给出新的内容。',
        'SAME_PLAN': '这一轮的计划与上一轮相同。请提出与上轮不同的方案。',
    }.get(state, '')


def snapshot():
    c = cfg()
    st = probe()
    u, t = vram()
    load, av, tot = ram()
    phase, warn = 'idle', False
    stg, pct = engine_stage()
    if is_running():
        if stg:
            phase = 'loading %d%%' % pct
        elif not state['trimmed'] and uptime_s() < 40:
            phase = 'loading'
        else:
            warn = av < 2.0
    else:
        warn = av < 2.0
    eng = c['engine']

    run = state.get('run_cfg') if st != 'stopped' else None
    if run and not ctx_ok(run['engine'], run['ctx']):
        run = dict(run)
        run['ctx'] = CTX
    eff = run or c
    approx = bool(run) and run.get('approx', False)
    _think_on = bool(c.get('think', False))
    _out = int(c.get('out_len') or OUT_LEN)
    rec = rec_of(eng)
    saved = cfg_saved()

    def ctx_item(v):
        _blocked = (v != CTX)
        return {'v': v, 'label': '%dK' % (v // 1024),
                'dis': (not ctx_ok(eng, v)) or _blocked, 'sel': c['ctx'] == v,
                'rec': (rec['ctx'] == v), 'why': ctx_reason(eng, v),
                'gh': bool(run) and not approx and run['ctx'] == v,
                'sav': saved.get('ctx') == v}

    def nmax_item(v):
        return {'v': v, 'label': '%d 路' % v, 'dis': not nmax_ok(v), 'sel': c['n_max'] == v,
                'rec': rec['n_max'] == v, 'why': nmax_reason(v),
                'gh': bool(run) and not approx and run.get('mtp') and run['n_max'] == v,
                'sav': saved.get('n_max') == v}

    def par_item(v):
        return {'v': v, 'label': '%d' % v, 'dis': not par_ok(eng, v), 'sel': c['parallel'] == v,
                'rec': rec['parallel'] == v, 'why': par_reason(eng, v),
                'gh': bool(run) and not approx and run['parallel'] == v,
                'sav': saved.get('parallel') == v}

    def engine_item(k):
        return {'v': k, 'label': k, 'dis': False, 'sel': eng == k, 'rec': rec['engine'] == k,
                'why': '', 'gh': bool(run) and run['engine'] == k,
                'sav': saved.get('engine') == k}

    groups = [
        {'key': 'engine', 'title': '引擎', 'items': [engine_item(k) for k, _ in ENGINE_OPTS]},
        {'key': 'ctx', 'title': '上下文（锁定）', 'items': [ctx_item(v) for v in ctx_opts(eng)]},
        {'key': 'mtp', 'title': '投机解码',
         'items': [{'v': '1', 'label': '开', 'dis': False, 'sel': c['mtp'], 'rec': rec['mtp'],
                    'why': '聊天/代码等可预测文本：实测解码 +60%~160%',
                    'gh': bool(run) and not approx and run.get('mtp'),
                    'sav': bool(saved.get('mtp'))},
                   {'v': '0', 'label': '关', 'dis': False, 'sel': not c['mtp'], 'rec': not rec['mtp'],
                    'why': '随机性强的文本（如随机词检索）MTP 反而更慢',
                    'gh': bool(run) and not approx and not run.get('mtp'),
                    'sav': not saved.get('mtp')}]},
        {'key': 'think', 'title': '思考模式',
         'items': [{'v': '1', 'label': '开', 'dis': False, 'sel': _think_on, 'rec': False,
                    'gh': bool(run) and not approx and run.get('think', True),
                    'sav': bool(saved.get('think', True)),
                    'why': '模型先写推理链再回答：输出 token 数暴涨'},
                   {'v': '0', 'label': '关', 'dis': False, 'sel': not _think_on, 'rec': True,
                    'why': '直接给答案（推荐 ★）：09-30 头对头 A/B 就是在这个模式下测的',
                    'gh': bool(run) and not approx and not run.get('think', True),
                    'sav': not saved.get('think', True)}]},
        {'key': 'out_len', 'title': '输出上限',
         'items': [{'v': str(v), 'label': '%dK' % (v // 1024), 'dis': not out_ok(v),
                    'sel': _out == v,
                    'rec': v == OUT_LEN,
                    'gh': bool(run) and not approx and run.get('out_len') == v,
                    'sav': saved.get('out_len') == v,
                    'why': out_reason(v)} for v in OUT_OPTS]},
        {'key': 'n_max', 'title': 'MTP 路数', 'items': [nmax_item(v) for v in NMAX_OPTS]},
        {'key': 'parallel', 'title': '并行数', 'items': [par_item(v) for v in PAR_OPTS]},
    ]
    port = cur_port()
    return {'combo': '%s-%dK · MTP %s · 并行 %d' % (eff['engine'], eff['ctx'] // 1024,
            ('%d 路' % eff['n_max']) if eff['mtp'] else '关', eff['parallel']),
            'short': '%s-%dK' % (eff['engine'], eff['ctx'] // 1024),
            'ctx': eff['ctx'],
            'running_cfg': dict(run) if run else None,
            'cfg_drift': bool(run) and (run.get('engine') != c['engine'] or run.get('ctx') != c['ctx']
                                        or bool(run.get('mtp')) != bool(c['mtp'])
                                        or run.get('n_max') != c['n_max']
                                        or run.get('parallel') != c['parallel']),
            'state': st, 'phase': phase, 'warn': warn, 'host': HOST, 'port': port,
            'http': http_ok(port, '/health'),
            'model': engine_model() if st == 'running' else '',
            'pool': '显存内',
            'budget': '-',
            'uptime': uptime(), 'log': log_path(), 'vram_used': u, 'vram_total': t,
            'ram_load': load, 'ram_avail': av, 'ram_total': tot,
            'pid': (state['proc'].pid if state['proc']
                    else (pid_on_port(port) if port_open(port) else None)),
            'groups': groups, 'dirty': dirty(), 'need_restart': state.get('need_restart', False),
            'action': act_state(), 'action_over': _act_over_view(),
            'samp': samp_of(),
            'samp_mode': samp_mode(),
            'samp_custom': samp_custom(),
            'samp_presets': SAMP_PRESETS,
            'cfg_error': state.get('cfg_error', ''),
            'cfg_conflict': state.get('cfg_conflict', ''),
            'free_mib': boot_free()}


def stats_payload():
    u, t = vram()
    load, av, tot = ram()
    return {'vram_used': u, 'vram_total': t, 'ram_load': load,
            'ram_avail': av, 'ram_total': tot, 'reqs': recent_requests(30),
            'free_mib': boot_free(), 'cache': cache_meter()}


def status_payload():
    full = snapshot()
    out = {k: full[k] for k in ('state', 'phase', 'warn', 'short', 'combo', 'ctx',
                                'uptime', 'pid', 'action', 'action_over', 'need_restart',
                                'dirty', 'cfg_error', 'cfg_conflict')}
    # KV 计费表：走 /api/status 每 2.5s 刷新（/api/stats 前端只拉一次，见 app.js loadStats）
    out['cache'] = cache_meter() if is_running() else None
    out['free_mib'] = boot_free()
    return out


def model_hint():
    return DEFAULT_MODEL.get(cfg()['engine'], 'local')


def engine_model():
    global MODEL_CACHE, LM_CACHE
    if MODEL_CACHE['id'] and is_running():
        return MODEL_CACHE['id']
    if time.time() - LM_CACHE['t'] < 12.0:
        return MODEL_CACHE['id'] or model_hint()
    MODEL_CACHE['id'] = ''
    try:
        req = urllib.request.Request('http://127.0.0.1:%d/v1/models' % cur_port())
        with _OPENER.open(req, timeout=0.6) as r:
            d = json.loads(r.read().decode('utf-8'))
        for m in d.get('data', []):
            if m.get('id'):
                MODEL_CACHE['id'] = m['id']
                LM_CACHE = {'t': time.time(), 'id': m['id']}
                return m['id']
    except Exception:
        pass
    LM_CACHE = {'t': time.time(), 'id': ''}
    return model_hint()


def chat_proxy(self):
    """把浏览器的 /chat 请求转发给本地引擎并透传 SSE 流"""
    n = int(self.headers.get('Content-Length') or 0)
    raw = self.rfile.read(n) if n else b'{}'
    port = cur_port()
    if not port_open(port):
        body = json.dumps({'error': '引擎未运行，请先在「设置」页启动'}).encode('utf-8')
        self.send_response(503)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        return
    try:
        payload = json.loads(raw.decode('utf-8'))
    except Exception:
        payload = {}
    payload.setdefault('model', engine_model())
    _SKEY = {'temp': 'temperature', 'top_p': 'top_p', 'top_k': 'top_k',
             'min_p': 'min_p', 'presence': 'presence_penalty',
             'repeat': 'repeat_penalty'}
    for _k, _v in samp_of().items():
        payload.setdefault(_SKEY[_k], _v)
    payload.setdefault('max_tokens', 32768)
    for _m in payload.get('messages') or []:
        if isinstance(_m, dict):
            _m.pop('reasoning_content', None)
            _m.pop('reasoning', None)
    payload['stream'] = True
    payload['stream_options'] = {'include_usage': True}
    data = json.dumps(payload).encode('utf-8')
    if not admit_chat():
        n = cfg().get('parallel', 1)
        msg = ('并发已满：当前配置最大并发数为 %d。'
               '请等上一个回答结束。') % n
        body = json.dumps({'error': msg}).encode('utf-8')
        self.send_response(503)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Cache-Control', 'no-cache')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        log('并发已满，拒绝：并行度 %s，当前在飞 %s' % (n, inflight_info()))
        return
    req = urllib.request.Request('http://127.0.0.1:%d/v1/chat/completions' % port, data=data,
                                 headers={'Content-Type': 'application/json'})
    try:
        resp = _OPENER.open(req, timeout=900)
    except urllib.error.HTTPError as he:
        try:
            detail = he.read().decode('utf-8', 'replace')[:400]
        except Exception:
            detail = ''
        log('代理失败(HTTP %s): %s' % (he.code, detail))
        body = json.dumps({'error': '引擎返回 %s: %s' % (he.code, detail)}).encode('utf-8')
        self.send_response(502)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        return
    except Exception as e:
        log('代理失败: %s' % e)
        body = json.dumps({'error': '引擎请求失败: %s' % e}).encode('utf-8')
        self.send_response(502)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        return
    self.send_response(200)
    self.send_header('Content-Type', 'text/event-stream; charset=utf-8')
    self.send_header('Cache-Control', 'no-cache')
    self.send_header('Connection', 'close')
    self.send_header('X-Accel-Buffering', 'no')
    self.end_headers()
    _sink = []
    try:
        while True:
            try:
                chunk = resp.read1(65536)
            except AttributeError:
                chunk = resp.read(4096)
            if not chunk:
                break
            self.wfile.write(chunk)
            self.wfile.flush()
            _sink.append(chunk)
    except Exception as e:
        log('chat 透传中断（客户端断开或引擎异常）：%s' % e)
    finally:
        try:
            resp.close()
        except Exception:
            pass
        release_chat()
        try:
            text = b''.join(_sink).decode('utf-8', 'replace')
            parts = re.findall(r'"content":"((?:[^"\\]|\\.)*)"', text)
            content = ''.join(p.encode('utf-8').decode('unicode_escape', 'replace')
                              for p in parts)
            st = guard_state(content, '')
            if st != 'OK':
                log('护栏[%s]：本轮交付异常（正文字 %d，finish 可能异常）——'
                    '建议重试或调小 thinking-budget' % (st, len(content)))
        except Exception:
            pass


# ---------------- /chat admission control --------------------------------
INFLIGHT = {'cur': 0, 'wait': 0, 'max': 1}
_IN_CV = threading.Condition(threading.Lock())


def admit_chat():
    with _IN_CV:
        try:
            INFLIGHT['max'] = max(1, int(cfg().get('parallel', 1) or 1))
        except Exception:
            INFLIGHT['max'] = 1
        if INFLIGHT['cur'] < INFLIGHT['max']:
            INFLIGHT['cur'] += 1
            return True
        INFLIGHT['wait'] += 1
        end = time.time() + 15.0
        while INFLIGHT['cur'] >= INFLIGHT['max'] and time.time() < end:
            _IN_CV.wait(1.0)
        INFLIGHT['wait'] = max(0, INFLIGHT['wait'] - 1)
        if INFLIGHT['cur'] >= INFLIGHT['max']:
            return False
        INFLIGHT['cur'] += 1
        return True


def release_chat():
    with _IN_CV:
        INFLIGHT['cur'] = max(0, INFLIGHT['cur'] - 1)
        _IN_CV.notify_all()


def inflight_info():
    with _IN_CV:
        return {'cur': INFLIGHT['cur'], 'wait': INFLIGHT['wait'], 'max': INFLIGHT['max']}


# ---------------- long actions (start / stop / restart) ------------------
ACT = {'name': None, 'stage': '', 't0': 0.0, 'err': ''}
_ACT_LOCK = threading.Lock()

ACT_LABEL = {'start': '启动引擎', 'stop': '停止引擎', 'restart': '重启引擎'}


def act_state():
    with _ACT_LOCK:
        if not ACT['name']:
            return None
        if ACT['err'] and time.time() - ACT['t0'] > 90:
            ACT.update(name=None, stage='', err='')
            return None
        return {'name': ACT['name'], 'label': ACT_LABEL.get(ACT['name'], ACT['name']),
                'stage': ACT['stage'], 'err': ACT['err'], 'pct': ACT.get('pct', 0),
                'secs': int(time.time() - ACT['t0']) if ACT['t0'] else 0}


class WaitTimeout(RuntimeError):
    pass


ACT_OVER = {'item': None}


def _act_over(name, ok, msg=''):
    with _ACT_LOCK:
        ACT_OVER['item'] = {'label': ACT_LABEL.get(name, name), 'ok': ok,
                            'secs': int(time.time() - ACT['t0']) if ACT['t0'] else 0,
                            'msg': str(msg)[:140], 't': time.time()}


def _act_over_view():
    with _ACT_LOCK:
        it = ACT_OVER['item']
        if it and time.time() - it['t'] > (10 if it['ok'] else 90):
            it = None
    return it


def _wait_ready(timeout=300):
    t0 = time.time()
    while time.time() - t0 < timeout and not _cfg_mod.SHUTTING:
        time.sleep(2)
        if state.get('proc') and state['proc'].poll() is not None:
            raise RuntimeError('引擎进程在启动阶段退出（详情看「日志」页）')
        with _ACT_LOCK:
            stg, pct = engine_stage()
            if stg:
                ACT['stage'] = '加载权重 %d%%' % pct
                ACT['pct'] = pct
            else:
                ACT['stage'] = '等待引擎响应'
                ACT['pct'] = 0
        try:
            with _OPENER.open('http://127.0.0.1:%d/health' % cur_port(),
                              timeout=2) as r:
                if r.status == 200:
                    return True
        except Exception:
            pass
    if _cfg_mod.SHUTTING:
        raise WaitTimeout('面板正在退出，停止等待')
    raise WaitTimeout('引擎 %d 秒内没有就绪——它可能仍在加载，可到「日志」页查看；稍后可再试'
                      % timeout)


def _run_action(name):
    try:
        if name == 'stop':
            with _ACT_LOCK:
                ACT['stage'] = '正在停止引擎'
            stop()
            with _ACT_LOCK:
                ACT['stage'] = '端口已释放'
        elif name == 'start':
            with _ACT_LOCK:
                ACT['stage'] = '正在创建引擎进程'
            start()
            with _ACT_LOCK:
                p = state.get('proc')
                ACT['stage'] = '已接管现有引擎' if p is None else '进程已启动 pid=%s' % p.pid
            _wait_ready()
        else:
            with _ACT_LOCK:
                ACT['stage'] = '正在停止旧引擎'
            restart()
            with _ACT_LOCK:
                p = state.get('proc')
                ACT['stage'] = '已接管现有引擎' if p is None else '新进程已启动 pid=%s' % p.pid
            _wait_ready()
        log('操作完成：%s' % ACT_LABEL.get(name, name))
        with _ACT_LOCK:
            ACT.update(name=None, stage='', err='', pct=0)
        _act_over(name, True)
    except WaitTimeout as e:
        log('操作 %s 等待超时: %s' % (name, e))
        with _ACT_LOCK:
            ACT.update(name=None, stage='', err='', pct=0)
        _act_over(name, None, str(e))
    except Exception as e:
        log('操作 %s 失败: %s' % (name, e))
        with _ACT_LOCK:
            ACT.update(name=None, stage='', err='', pct=0)
        _act_over(name, False, str(e))


def queue_action(name):
    with _ACT_LOCK:
        if ACT['name'] and not ACT['err']:
            return {'ok': False, 'busy': True,
                    'msg': '已有操作在进行：%s（已 %d 秒），请等它结束'
                           % (ACT_LABEL.get(ACT['name'], ACT['name']),
                              int(time.time() - ACT['t0']))}
        ACT.update(name=name, stage='已提交', t0=time.time(), err='')
        threading.Thread(target=_run_action, args=(name,), daemon=True).start()
    return {'ok': True, 'queued': name, 'msg': '已提交：%s' % ACT_LABEL.get(name, name)}


def start_dash():
    if dash['server']:
        return

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _json(self, obj, code=200):
            body = json.dumps(obj, ensure_ascii=False).encode('utf-8')
            self.send_response(code)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _log_stream(self):
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream; charset=utf-8')
            self.send_header('Cache-Control', 'no-cache')
            self.send_header('Connection', 'keep-alive')
            self.end_headers()
            # 字节游标而不是行列表比较。tail_lines() 返回的是滑动窗口，
            # 只要日志多一行，lines[:len(sent)] == sent 就失败，于是整个
            # 尾部被原样重发 → 客户端每来一条新日志收 300 条消息。
            off = [-1]   # -1 = 首次连接，只补最后 50 KB（约 300 行）作为初始画面
            try:
                while True:
                    try:
                        size = os.path.getsize(log_path())
                    except Exception:
                        size = 0
                    if off[0] < 0:
                        off[0] = max(0, size - 50000)
                    elif size < off[0]:
                        off[0] = 0          # 日志被轮转/截断
                    if size > off[0]:
                        try:
                            with open(log_path(), 'rb') as f:
                                f.seek(off[0])
                                data = f.read()
                        except Exception:
                            data = b''
                        off[0] += len(data)
                        for line in data.decode('utf-8', 'replace').splitlines():
                            if not line.strip():
                                continue
                            payload = json.dumps(line, ensure_ascii=False)
                            self.wfile.write(('data: ' + payload + '\n\n').encode('utf-8'))
                        self.wfile.flush()
                    time.sleep(1)
            except (BrokenPipeError, ConnectionResetError):
                pass


        def do_POST(self):
            # 支持子路径前缀（如 /llm/chat）
            path = self.path
            if path.startswith('/llm'):
                path = path[4:]  # 去掉 /llm 前缀
            if path.startswith('/chat'):
                chat_proxy(self)
                return
            self._json({'ok': False}, 404)

        def do_GET(self):
            pa = self.path
            # 支持子路径前缀（如 /llm/api）
            if pa.startswith('/llm'):
                pa = pa[4:]  # 去掉 /llm 前缀
            if pa.startswith('/do'):
                q = urllib.parse.parse_qs(urllib.parse.urlparse(pa).query)
                act = (q.get('action') or [''])[0]
                try:
                    if act == 'set':
                        k = (q.get('k') or [''])[0]
                        v = (q.get('v') or [''])[0]
                        if k == 'mtp':
                            set_opt('mtp', v == '1')
                        elif k == 'think':
                            set_opt('think', v == '1')
                        elif k == 'engine':
                            set_opt('engine', v)
                        elif k in ('ctx', 'n_max', 'parallel', 'out_len'):
                            set_opt(k, v, int)
                    elif act == 'setport':
                        v = (q.get('v') or [''])[0]
                        try:
                            n = int(v)
                            if 1024 <= n <= 65535:
                                set_opt('port', n)
                                log('端口 -> %d' % n)
                            else:
                                log('端口越界被拒: %s' % v)
                        except ValueError:
                            log('端口无效被拒: %s' % v)
                    elif act in ('start', 'stop', 'restart'):
                        self._json(queue_action(act))
                        return
                    elif act == 'clearkv':
                        # 保留项：重启并清空 KV（约 13 秒实测，期间不可用）。
                        self._json(queue_action('restart'))
                        return
                    elif act == 'clearidle':
                        # 在线清空（仅空闲，忙则拒绝）：直调引擎 admin 端点，
                        # try_idle 模式不清一半、不碰在途请求。
                        try:
                            from .engine import engine_cache_clear
                            res = engine_cache_clear(mode='try_idle')
                        except Exception as e:
                            self._json({'ok': False,
                                        'msg': '在线清空失败: %s' % str(e)[:140]})
                            return
                        if res.get('ok'):
                            self._json({'ok': True,
                                        'msg': '在线清空完成：KV %s→%s · host %s→%s · %sms · failed=%s' % (
                                            res.get('kv_before'), res.get('kv_after'),
                                            res.get('host_before'), res.get('host_after'),
                                            res.get('duration_ms'), res.get('failed_requests'))})
                        else:
                            self._json({'ok': False,
                                        'msg': '在线清空被拒绝：%s（不清一半，在途请求不受影响）'
                                               % res.get('error', 'unknown')})
                        return
                    elif act == 'cleardrain':
                        # 高级档：drain（在途按可重试失败计数后清，需二次确认）。
                        try:
                            from .engine import engine_cache_clear
                            res = engine_cache_clear(mode='drain_and_fail', timeout=120)
                        except Exception as e:
                            self._json({'ok': False,
                                        'msg': 'drain 清空失败: %s' % str(e)[:140]})
                            return
                        if res.get('ok'):
                            self._json({'ok': True,
                                        'msg': 'drain 清空完成：KV %s→%s · failed=%s（在途已按可重试失败计数）' % (
                                            res.get('kv_before'), res.get('kv_after'),
                                            res.get('failed_requests'))})
                        else:
                            self._json({'ok': False,
                                        'msg': 'drain 清空失败：%s' % res.get('error', 'unknown')})
                        return
                    elif act == 'clearabort':
                        # 高级档：abort（含 transaction abort，需二次确认）。
                        try:
                            from .engine import engine_cache_clear
                            res = engine_cache_clear(mode='abort_and_clear', timeout=120)
                        except Exception as e:
                            self._json({'ok': False,
                                        'msg': 'abort 清空失败: %s' % str(e)[:140]})
                            return
                        if res.get('ok'):
                            self._json({'ok': True,
                                        'msg': 'abort 清空完成：KV %s→%s · failed=%s' % (
                                            res.get('kv_before'), res.get('kv_after'),
                                            res.get('failed_requests'))})
                        else:
                            self._json({'ok': False,
                                        'msg': 'abort 清空失败：%s' % res.get('error', 'unknown')})
                        return
                    elif act == 'setsamp':
                        if q.get('mode'):
                            set_samp_mode((q.get('mode') or [''])[0])
                            self._json({'ok': True,
                                        'msg': '采样模式已切换；面板转发立即生效'})
                        else:
                            set_samp_custom((q.get('k') or [''])[0], (q.get('v') or [''])[0])
                            self._json({'ok': True,
                                        'msg': '自定义采样已更新；面板转发立即生效'})
                        return
                    elif act == 'save':
                        ok = save_cfg(force=(q.get('force') or ['0'])[0] == '1')
                        self._json({'ok': True, 'refused': not ok,
                                    'msg': '配置已保存' if ok else
                                           (state.get('cfg_conflict') or '保存失败，详见「日志」页')})
                        return
                    elif act == 'reset':
                        reset_cfg()
                    else:
                        self._json({'ok': False,
                                    'msg': '未实现的动作: %s（后端没有这个分支）' % act})
                        return
                except Exception as e:
                    log('面板操作失败: %s' % e)
                self._json({'ok': True})
                return
            if pa.startswith('/log'):
                self._log_stream()
                return
            if pa.startswith('/api/stats'):
                try:
                    self._json(stats_payload())
                except Exception:
                    self._json({'ok': False}, 503)
                return
            if pa.startswith('/api/settings'):
                try:
                    self._json(snapshot())
                except Exception:
                    self._json({'ok': False}, 503)
                return
            if pa.startswith('/api/status'):
                try:
                    self._json(status_payload())
                except Exception:
                    self._json({'ok': False}, 503)
                return
            if pa.startswith('/api'):
                try:
                    self._json(snapshot())
                except ConnectionAbortedError:
                    pass
                except Exception:
                    import traceback
                    try:
                        log('snapshot 失败:\n%s' % traceback.format_exc())
                    except Exception:
                        pass
                return
            if self.path in _STATIC:
                ctype, body = page_static(self.path)
                self.send_response(200)
                self.send_header('Content-Type', ctype)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            body = page_index()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    srv = ThreadingHTTPServer(('0.0.0.0', DASH_PORT), H)
    dash['server'] = srv
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    log('面板服务 http://127.0.0.1:%d' % DASH_PORT)
