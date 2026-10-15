# -*- coding: utf-8 -*-
"""面板日志（web.log）——写一条、带轮转。全项目唯一日志出口。"""
import os
import sys
import time
from datetime import datetime

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # webui/
LOG_FILE = os.path.join(BASE, 'web.log')


def log(*a):
    try:
        if os.path.getsize(LOG_FILE) > 2 * 1024 * 1024:
            with open(LOG_FILE, 'rb') as f:
                tail = f.read()[-200000:]
            with open(LOG_FILE, 'wb') as f:
                f.write(b'[rotated]\n' + tail)
    except Exception:
        pass
    try:
        with open(LOG_FILE, 'a', encoding='utf-8') as f:
            f.write('[%s] %s\n' % (datetime.now().strftime('%H:%M:%S'), ' '.join(str(x) for x in a)))
    except Exception as e:
        try:
            sys.stderr.write('[log-fail] %r: %s\n' % (a, e))
        except Exception:
            pass
