# -*- coding: utf-8 -*-
"""NInfer WebUI 入口（Linux 版，无托盘）。

从 Windows 托盘版迁移而来，去掉 pystray 托盘部分，只保留浏览器面板。
面板地址：http://127.0.0.1:18790
"""
import os
import sys
import threading
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from app.config import *   # noqa: F401,F403
from app import config as _cfg_mod
from app.logutil import log
from app.engine import watchdog_check, start, is_running, probe
from app.web import start_dash, snapshot


def _watchdog_loop():
    """后台看门狗：引擎崩溃自动重启。"""
    while not _cfg_mod.SHUTTING:
        try:
            watchdog_check()
        except Exception:
            pass
        time.sleep(5)


def main():
    # 加载配置
    load_cfg()
    log('NInfer WebUI 启动（Linux 版，无托盘）')

    # 启动看门狗
    wd = threading.Thread(target=_watchdog_loop, daemon=True)
    wd.start()

    # 启动 HTTP 面板
    start_dash()

    # 自动启动引擎
    if cfg().get('autostart', True) and not is_running():
        log('自动启动引擎...')
        start()

    # 主线程保持运行
    try:
        while not _cfg_mod.SHUTTING:
            time.sleep(1)
    except KeyboardInterrupt:
        log('收到中断信号，正在退出...')
    finally:
        _cfg_mod.SHUTTING = True
        if is_running():
            from app.engine import stop
            stop()
        log('NInfer WebUI 已退出')


if __name__ == '__main__':
    main()
