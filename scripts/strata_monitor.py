#!/usr/bin/env python3
"""Read-only Strata monitor for the live engine on device B.

One JSON line per check, then a verdict.  Exit 0 when everything is inside the thresholds,
exit 1 when any alert fires.  Nothing is written to the engine or to disk.

What each alert means:

  evictions > 0        the parked set went over the byte budget, so the oldest conversation was
                       dropped.  Its next turn re-reads from the system-prompt root, not from its
                       own checkpoint - at 40K tokens that is roughly 15 s instead of 0.4 s.
  bytes / budget >= .8 approaching that line.
  disk_read_mb > 0     the PLE table is being read back off disk: the mlock guarantee is being
                       exercised, i.e. its pages really were reclaimed.
  VmLck == 0           nothing is locked, so RLIMIT_MEMLOCK is still under the PLE table.
  engine VmSwap > 0    the engine's own pages went to swap.
  swap free < 200 MiB  no headroom left for a spike (environmental, not ours to fix).
  hit_rate < 0.5       the last requests got little back from the cache.

Run on device B:  python3 scripts/strata_monitor.py
Self-test (proves the alerts can fail):  python3 scripts/strata_monitor.py --selftest
"""
import json
import os
import subprocess
import sys
import urllib.request

BASE = os.environ.get("STRATA_BASE", "http://127.0.0.1:18790")
BUDGET_WARN = 0.8            # fraction of the conversation-cache budget
SWAP_FREE_MIN_MIB = 200      # environmental headroom
HIT_RATE_MIN = 0.5           # last-request reuse below this means the cache is not helping
PLATE_ROWS = 320001536       # the engine's own PLE table row count, from its startup line
PLE_ROW_BYTES = 4            # bytes per row as the engine reads it; used only to size the alert text


def metrics():
    with urllib.request.urlopen(BASE + "/metrics", timeout=15) as r:
        return json.loads(r.read().decode())


def proc_status(pid):
    """VmRSS / VmLck / VmSwap in MiB for one pid."""
    out = {}
    with open("/proc/%d/status" % pid) as f:
        for line in f:
            key = line.split(":")[0]
            if key in ("VmRSS", "VmLck", "VmSwap"):
                out[key] = int(line.split()[1]) // 1024
    return out


def locked_limit(pid):
    """RLIMIT_MEMLOCK in MiB as the process actually sees it."""
    with open("/proc/%d/limits" % pid) as f:
        for line in f:
            if line.startswith("Max locked memory"):
                parts = line.split()
                v = parts[3]
                return "infinity" if v == "unlimited" else int(v) // 1024
    return None


def engine_pid():
    """The engine child of the serve.server process; -1 when the layout is not the running one."""
    pid = int(os.environ.get("STRATA_PID", "0"))
    if pid:
        return pid
    out = subprocess.run(["pgrep", "-f", "serve.server"], capture_output=True, text=True).stdout.split()
    if not out:
        return -1
    parent = int(out[0])
    kids = subprocess.run(["pgrep", "-P", str(parent)], capture_output=True, text=True).stdout.split()
    return int(kids[0]) if kids else parent


def swap_free_mib():
    out = subprocess.run(["swapon", "--show", "--bytes"], capture_output=True, text=True).stdout.splitlines()
    total = used = 0
    for line in out[1:]:
        parts = line.split()
        if len(parts) >= 4:
            total += int(parts[2])
            used += int(parts[3])
    return (total - used) // (1024 * 1024) if total else None


def checks(m, status, limit, swap_free):
    cc, hw, eng = m["conversation_cache"], m["hardware"], m["engine"]
    rows = []

    def row(name, value, alert):
        rows.append({"check": name, "value": value, "alert": alert})

    budget = cc.get("budget_mib") or 0
    used = (cc.get("bytes") or 0) / (1024 * 1024)
    row("evictions", cc.get("evictions"), bool(cc.get("evictions")))
    row("budget_used", round(used / budget, 3) if budget else None, bool(budget and used / budget >= BUDGET_WARN))
    row("parked", cc.get("parked"), False)
    row("reused", "%d/%d" % (cc.get("requests_reused"), cc.get("requests")), False)
    row("disk_read_mb", hw.get("disk_read_mb"), bool(hw.get("disk_read_mb")))
    row("vram_free_mib", eng.get("vram_free_mib"), False)

    lck = status.get("VmLck")
    row("vmlck_mib", lck, bool(lck == 0 and limit is not None))
    row("locked_limit_mib", limit, bool(limit is not None and limit != "infinity" and limit < 256))
    row("vmswap_mib", status.get("VmSwap"), bool(status.get("VmSwap")))
    row("swap_free_mib", swap_free, bool(swap_free is not None and swap_free < SWAP_FREE_MIN_MIB))

    hits = [r.get("hit_rate") for r in m.get("requests", []) if r.get("hit_rate") is not None]
    last = hits[0] if hits else None
    row("last_hit_rate", last, bool(last is not None and last < HIT_RATE_MIN))
    return rows


def report(rows):
    for r in rows:
        print(json.dumps(r, sort_keys=True), flush=True)
    fired = [r["check"] for r in rows if r["alert"]]
    print(json.dumps({"verdict": "ALERT" if fired else "ok", "fired": fired}, sort_keys=True), flush=True)
    return 1 if fired else 0


def selftest():
    """A metrics snapshot that must trip every alert, so the checks are shown to be able to fail."""
    m = {
        "engine": {"vram_free_mib": 8994},
        "hardware": {"disk_read_mb": 12.0},
        "conversation_cache": {"budget_mib": 131072, "bytes": int(0.85 * 131072 * 1024 * 1024),
                               "parked": 90, "evictions": 3, "requests": 40, "requests_reused": 12},
        "requests": [{"hit_rate": 0.2}],
    }
    status = {"VmRSS": 84000, "VmLck": 0, "VmSwap": 4}
    return report(checks(m, status, 64, 12))


def main():
    if "--selftest" in sys.argv:
        return selftest()
    m = metrics()
    pid = engine_pid()
    if pid < 0:
        print(json.dumps({"error": "engine pid not found; set STRATA_PID"}, sort_keys=True), flush=True)
        return 2
    return report(checks(m, proc_status(pid), locked_limit(pid), swap_free_mib()))


if __name__ == "__main__":
    sys.exit(main())
