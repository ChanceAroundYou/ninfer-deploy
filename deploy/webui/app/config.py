# -*- coding: utf-8 -*-
"""配置与档位系统：config.json 读写、采样预设、共享状态。

从 Windows 托盘版迁移到 Linux webui：
  - 去掉 vision 档位（qwen3.8-27b 无 vision 权重）
  - 去掉 pool_of/budget_of（kvmem 残留）
  - 路径改为 ai 服务器路径
  - 默认 256K 满池配置
"""
import json
import os
import threading

from .logutil import log

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # webui/
CFG_FILE = os.path.join(BASE, 'config.json')
LOCK_PORT = 18789
DASH_PORT = 18790
HOST = '0.0.0.0'
PORT = 8080

CFG_LOCK = threading.Lock()


def _locked(fn):
    def wrapper(*a, **kw):
        with CFG_LOCK:
            return fn(*a, **kw)
    wrapper.__name__ = fn.__name__
    wrapper.__doc__ = fn.__doc__
    return wrapper


def cur_port():
    return int(cfg().get('port', PORT) or PORT)


# ai 服务器上的路径
MODEL_NINFER = '/home/ET/chenran/llm-deploy/models/swift-qwen38-q4km.ninfer'
EXE_NINFER = '/home/ET/chenran/llm-deploy/build-ninfer-sm86-clean/apps/ninfer-serve'
CWD_NINFER = '/home/ET/chenran/llm-deploy/build-ninfer-sm86-clean'
GPU_ID = '2'

# 256K 满池配置（2026-10-09 实测定案）
CTX = 262144
POOL = 716800
OUT_LEN = 32768

OUT_OPTS = [4096, 8192, 12288, 16384, 24576, 32768]


def out_ok(v, engine=None):
    try:
        v = int(v)
    except Exception:
        return False
    if v <= 0 or v not in OUT_OPTS:
        return False
    return v <= POOL


def out_reason(v):
    if v > POOL:
        return '超过设备池（%d）' % POOL
    if v == 32768:
        return '满池 256K 下的最大输出档'
    if v == 24576:
        return '实测长文不截断'
    if v == 16384:
        return '够写 6000+ 字的中文技术文档'
    return '更小的档，思考加正文更不容易撞顶'


ENGINE_OPTS = [('ninfer', 'ninfer sm_86（KVMem 环·256K·dflash2）')]
CTX_BY_ENGINE = {'ninfer': [CTX]}


def ctx_opts(engine=None):
    return [CTX]


def ctx_ok(engine, ctx):
    return ctx == CTX


def ctx_reason(engine, ctx):
    return '256K 满池配置（2026-10-09 实测定案）'


NMAX_OPTS = [2, 4, 6, 8, 10, 12]
PAR_OPTS = [1, 2, 4]


def nmax_ok(n):
    return n in NMAX_OPTS


def par_ok(engine, n):
    return n in [1, 2, 4]


def nmax_reason(n):
    if n == 12:
        return '甜点：实测 243.8 tok/s（2.1× 于 mtp/4）'
    if n == 2:
        return '保守档：接受率更高，但速度更低'
    return '中间档'


def par_reason(engine, n):
    if n == 4:
        return '4 路并发（实验性/未验证）：KVMem 环多序列支持未充分测试'
    if n == 2:
        return '2 路并发（实验性/未验证）：平衡吞吐与显存，但未充分测试'
    return '串行（推荐）：KVMem 环只服务一条活跃序列'


SAMP_PRESETS = {
    'think': {'temp': 1.0, 'top_p': 0.95, 'top_k': 20, 'min_p': 0.0,
              'presence': 1.5, 'repeat': 1.0},
    'nonthink': {'temp': 0.7, 'top_p': 0.80, 'top_k': 20, 'min_p': 0.0,
                 'presence': 1.5, 'repeat': 1.0},
}
SAMP_KEYS = ('temp', 'top_p', 'top_k', 'min_p', 'presence', 'repeat')
SAMP_MODES = ('auto', 'think', 'nonthink', 'custom')


def samp_mode():
    m = cfg().get('samp_mode')
    return m if m in SAMP_MODES else 'auto'


def samp_custom():
    s = dict(SAMP_PRESETS['think'])
    v = (cfg().get('sampling') or {}).get('custom')
    if isinstance(v, dict):
        s.update({k: v[k] for k in SAMP_KEYS if k in v})
    return s


def samp_of(engine=None):
    m = samp_mode()
    if m == 'think':
        return dict(SAMP_PRESETS['think'])
    if m == 'nonthink':
        return dict(SAMP_PRESETS['nonthink'])
    if m == 'custom':
        return samp_custom()
    return {}


SAMP_RANGE = {'temp': (0.0, 2.0), 'top_p': (0.0, 1.0), 'top_k': (0, 20),
              'min_p': (0.0, 1.0), 'presence': (-2.0, 2.0), 'repeat': (0.0, 2.0)}


def set_samp_custom(k, v):
    if k not in SAMP_KEYS:
        return
    try:
        v = round(float(v), 4)
    except Exception:
        return
    lo, hi = SAMP_RANGE[k]
    if v < lo or v > hi:
        v = max(lo, min(hi, v))
        log('采样 %s=%s 越界，钳到 %s（合法 %s~%s）' % (k, v, v, lo, hi))
    if k in ('top_k',) and int(v) != v:
        v = float(int(v))
    if samp_custom().get(k) == v:
        return
    state['cfg'].setdefault('sampling', {}).setdefault('custom', {})[k] = v
    log('自定义采样 %s=%s（面板转发立即生效）' % (k, v))


state = {'cfg': {}, 'saved': {}, 'proc': None, 'started': None,
         'logfh': None, 'status': 'stopped', 'trimmed': False, 'adopted': False,
         'need_restart': False, 'run_cfg': None}
dash = {'server': None}
GPU_CACHE = {'tv': 0.0, 'tr': 0.0, 'vram': (0, 0), 'ram': (0, 0.0, 0.0)}
MODEL_CACHE = {'id': ''}
LM_CACHE = {'t': 0.0, 'id': ''}

DEFAULT_MODEL = {'ninfer': 'qwen3.8-27b'}

BEST = {'engine': 'ninfer', 'ctx': CTX, 'mtp': True, 'n_max': 12, 'parallel': 1,
        'autostart': True, 'port': 8080, 'think': True,
        'out_len': OUT_LEN, 'samp_mode': 'auto'}

state['cfg'] = dict(BEST)
state['saved'] = dict(BEST)


def cfg():
    return state['cfg']


def cfg_saved():
    try:
        with open(CFG_FILE, encoding='utf-8') as fh:
            d = json.load(fh)
        return d if isinstance(d, dict) and d else dict(cfg())
    except Exception:
        return dict(cfg())


def load_cfg():
    try:
        c = json.load(open(CFG_FILE, encoding='utf-8'))
        for k in BEST:
            if k in c:
                state['cfg'][k] = c[k]
        if not ctx_ok(state['cfg']['engine'], state['cfg']['ctx']):
            state['cfg']['ctx'] = CTX
        if not nmax_ok(state['cfg']['n_max']):
            state['cfg']['n_max'] = 12
        if not par_ok(state['cfg']['engine'], state['cfg']['parallel']):
            state['cfg']['parallel'] = 1
        if isinstance(c.get('sampling'), dict) and \
                isinstance(c['sampling'].get('custom'), dict):
            state['cfg']['sampling'] = {'custom': dict(c['sampling']['custom'])}
        if state['cfg'].get('samp_mode') not in SAMP_MODES:
            state['cfg']['samp_mode'] = 'auto'
        state['saved'] = dict(state['cfg'])
        state.pop('cfg_error', None)
    except Exception as e:
        state['cfg_error'] = ('config.json 读取失败（已用默认组合）：%s'
                              % str(e)[:120])
        log(state['cfg_error'])


@_locked
def save_cfg(force=False):
    try:
        try:
            with open(CFG_FILE, encoding='utf-8') as f:
                disk = json.load(f)
        except Exception:
            disk = None
        if disk is not None and disk != state['saved'] and not force:
            state['cfg_conflict'] = ('config.json 在面板启动后被改动过，'
                                     '本次未写入；再点一次「保存配置」强制覆盖。')
            log('保存被拒：%s' % state['cfg_conflict'])
            return False
        tmp = CFG_FILE + '.tmp'
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump(state['cfg'], f, ensure_ascii=False, indent=1)
        os.replace(tmp, CFG_FILE)
        state['saved'] = dict(state['cfg'])
        state.pop('cfg_conflict', None)
        log('配置已保存: %s' % state['cfg'])
        return True
    except Exception as e:
        log('保存失败: %s' % e)
        return False


def dirty():
    return state['cfg'] != state['saved']


def sync_cfg_from_disk():
    if state['cfg'] != state['saved']:
        return
    try:
        with open(CFG_FILE, encoding='utf-8') as f:
            disk = json.load(f)
    except Exception:
        return
    if disk and disk != state['saved']:
        old = dict(state['cfg'])
        load_cfg()
        if state['cfg'] != old:
            log('config.json 已被外部修改，启动前改用磁盘配置：%s' % state['cfg'])


@_locked
def set_opt(k, v, cast=None):
    if cast:
        v = cast(v)
    if cfg().get(k) == v:
        return
    state['cfg'][k] = v
    if k == 'out_len' and not out_ok(v):
        log('输出上限 %s 非法（候选 %s）' % (v, OUT_OPTS))
        return
    if not ctx_ok(cfg()['engine'], cfg()['ctx']):
        state['cfg']['ctx'] = CTX
    if not par_ok(cfg()['engine'], cfg()['parallel']):
        state['cfg']['parallel'] = 1
    if not nmax_ok(cfg()['n_max']):
        state['cfg']['n_max'] = 12
    from .engine import is_running
    if is_running():
        state['need_restart'] = True
    log('设置 %s=%s（需重启=%s）' % (k, v, state['need_restart']))


REC_LINE = {'ninfer': {'ctx': CTX, 'parallel': 1, 'n_max': 12}}


def rec_of(eng=None):
    e = eng or cfg()['engine']
    d = dict(REC_LINE.get(e, REC_LINE['ninfer']))
    d['engine'] = e
    d['mtp'] = True
    return d


def reset_cfg():
    c = dict(state['cfg'])
    c.update(REC_LINE.get(c['engine'], {}))
    c['sampling'] = {'custom': dict(SAMP_PRESETS['think'])}
    c['samp_mode'] = 'auto'
    state['cfg'] = c
    if is_running():
        state['need_restart'] = True
    log('已重置为推荐组合: %s' % state['cfg'])


SHUTTING = False


def set_samp_mode(m):
    if m not in SAMP_MODES or samp_mode() == m:
        return
    state['cfg']['samp_mode'] = m
    log('采样模式 -> %s（面板转发立即生效）' % m)
