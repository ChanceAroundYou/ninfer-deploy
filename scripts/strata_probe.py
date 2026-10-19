#!/usr/bin/env python3
"""Two probes for the conversation pool (Strata).

MODE=mech  four sessions, three turns each, round-robin (interleaved arrival).  Answers: does every
           session keep its own cache while the others run, and how many entries stay parked?
MODE=size  three single-turn sessions of ~20K / ~50K / ~100K tokens.  Answers: what one parked
           conversation costs in host RAM (the engine's own snapshot_bytes), to size the pool.

The engine writes its `conversation cache:` lines to the config's "log" file (not to the server's
stdout), so the run points that at a per-run path and reads it back at the end.
"""
import json, os, re, subprocess, sys, time, urllib.request

BASE = "http://127.0.0.1:18080"
LAB = "/home/ET/chenran/strata-lab-20261017"
PY = LAB + "/.venv/bin/python"
CFG = LAB + "/strata-iq2_xs.json"
SRV = "/tmp/strata-probe.server.log"
ENG = "/tmp/strata-probe.engine.log"
TMPCFG = "/tmp/strata-probe.json"
MODE = os.environ.get("MODE", "mech")

WORDS = ("alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima mike "
         "november oscar papa quebec romeo sierra tango uniform victor whiskey xray yankee "
         "zulu anchor beacon canyon dagger ember falcon glacier harbor ingot jigsaw kernel "
         "lantern meadow nimbus orchard pebble quarry ripple summit tundra urchin vessel "
         "willow xenon yeast zephyr amber basalt cinder dune estuary fjord granite hollow "
         "islet juniper kelp lagoon marsh nectarine opal prairie quartz reef savanna thicket "
         "upland vale wetland yarrow zenith alcove bluff creek dell escarpment fen gorge").split()


def block(n, seed):
    return " ".join(WORDS[(i * 13 + seed) % len(WORDS)] for i in range(n))


def start(mib, slots, prompt_cache=6):
    with open(CFG) as f:
        c = json.load(f)
    a = list(c["args"])
    for flag in ("--prompt-cache", "--conversation-cache-mib", "--conversation-cache-slots"):
        while flag in a:
            i = a.index(flag)
            del a[i:i + 2]
    a += ["--prompt-cache", str(prompt_cache),
          "--conversation-cache-mib", str(mib),
          "--conversation-cache-slots", str(slots)]
    c["args"] = a
    c["log"] = ENG
    with open(TMPCFG, "w") as f:
        json.dump(c, f)
    env = dict(os.environ, STRATA_REQUEST_LINES="1", CUDA_VISIBLE_DEVICES="3")
    lf = open(SRV, "w")
    p = subprocess.Popen([PY, "-m", "serve.server", "--engine", "strata",
                          "--config", TMPCFG, "--port", "18080"],
                         cwd=LAB, stdout=lf, stderr=subprocess.STDOUT, env=env)
    for _ in range(300):
        if p.poll() is not None:
            raise RuntimeError("engine exited early; see " + SRV)
        try:
            if urllib.request.urlopen(BASE + "/health", timeout=3).status == 200:
                return p, lf
        except Exception:
            pass
        time.sleep(1)
    raise RuntimeError("health timeout")


def chat(messages, max_tokens=32, rb=16):
    body = {"model": "qwen3.8-flash-next-iq2_xs", "messages": messages,
            "max_tokens": max_tokens, "reasoning_budget_tokens": rb,
            "stream": True, "temperature": 0.0}
    req = urllib.request.Request(BASE + "/v1/chat/completions",
                                 data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    t0 = time.time()
    usage, text = None, ""
    with urllib.request.urlopen(req, timeout=1800) as r:
        for raw in r:
            line = raw.decode().strip()
            if not line.startswith("data:"):
                continue
            pl = line[5:].strip()
            if pl == "[DONE]":
                break
            try:
                d = json.loads(pl)
            except Exception:
                continue
            if d.get("usage"):
                usage = d["usage"]
            for ch in d.get("choices", []):
                dl = ch.get("delta", {})
                text += (dl.get("content") or "") + (dl.get("reasoning_content") or "")
    return usage, round(time.time() - t0, 2), text


def msgs(sid, turns, turn_words):
    m = [{"role": "system", "content": "You are agent %d. Answer briefly." % sid}]
    for t in range(1, turns + 1):
        m.append({"role": "user", "content": "S%d T%d. %s" % (sid, t, block(turn_words, sid * 100 + t))})
        m.append({"role": "assistant", "content": "ack %d-%d" % (sid, t)})
    return m


def report(tag, u, wall):
    pt = u["prompt_tokens"]
    ct = (u.get("prompt_tokens_details") or {}).get("cached_tokens", 0)
    print(json.dumps({"tag": tag, "prompt": pt, "cached": ct, "read": pt - ct, "wall": wall}), flush=True)


def run_mech(mib, slots):
    """N sessions x 3 turns, round-robin: turn 1 of each, then turn 2 of each, then turn 3.
    With a budget smaller than one snapshot the pool evicts instead of holding: same shape, other MIB."""
    n = int(os.environ.get("NSESS", "4"))
    words = int(os.environ.get("WORDS", "2600"))
    p, lf = start(mib, slots)
    time.sleep(2)
    try:
        for turn in (1, 2, 3):
            for sid in range(1, n + 1):
                u, wall, _ = chat(msgs(sid, turn, words))
                report("s%d-t%d" % (sid, turn), u, wall)
        time.sleep(float(os.environ.get("SETTLE", "3")))
    finally:
        p.terminate()
        try:
            p.wait(timeout=90)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait()
        lf.close()


def msgs(sid, turns, turn_words, first_words=None):
    m = [{"role": "system", "content": "You are agent %d. Answer briefly." % sid}]
    for t in range(1, turns + 1):
        w = first_words if (t == 1 and first_words) else turn_words
        m.append({"role": "user", "content": "S%d T%d. %s" % (sid, t, block(w, sid * 100 + t))})
        m.append({"role": "assistant", "content": "ack %d-%d" % (sid, t)})
    return m


def run_fleet(mib, slots):
    """10 sessions of 20K..100K tokens, 3 agent-loop turns each, round-robin with a shuffled order
    inside every round.  This is the user's workload: parallel agents, each appending, arrival order
    arbitrary."""
    import random
    tok = [int(x) for x in os.environ.get(
        "TOKS", "20000,30000,40000,50000,60000,70000,80000,90000,100000,100000").split(",")]
    first = [round(t / 1.586) for t in tok]
    rounds = int(os.environ.get("ROUNDS", "3"))
    rng = random.Random(20261018)
    p, lf = start(mib, slots)
    time.sleep(6)
    res = {}
    ep = engine_pid(p.pid)
    base = rss_detail(ep)
    print(json.dumps({"tag": "baseline", **base, "host_mem_available_mib": mem_available_mib()}), flush=True)
    peak = dict(base)
    try:
        for turn in range(1, rounds + 1):
            order = list(range(len(tok)))
            rng.shuffle(order)
            for sid in order:
                u, wall, _ = chat(msgs(sid + 1, turn, 2600, first[sid]))
                pt = u["prompt_tokens"]
                ct = (u.get("prompt_tokens_details") or {}).get("cached_tokens", 0)
                res[(sid + 1, turn)] = (pt, ct)
                d = rss_detail(ep)
                if (d["rss_mib"] or 0) > (peak["rss_mib"] or 0):
                    peak = d
                print(json.dumps({"tag": "s%d-t%d" % (sid + 1, turn), "prompt": pt,
                                  "cached": ct, "read": pt - ct, "wall": wall,
                                  "engine_rss_mib": d["rss_mib"]}), flush=True)
        time.sleep(float(os.environ.get("SETTLE", "15")))
        d = rss_detail(ep)
        if (d["rss_mib"] or 0) > (peak["rss_mib"] or 0):
            peak = d
    finally:
        p.terminate()
        try:
            p.wait(timeout=120)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait()
        lf.close()
    print(json.dumps({"tag": "peak", **peak,
                      "delta_mib": (peak["rss_mib"] or 0) - (base["rss_mib"] or 0),
                      "host_mem_available_mib": mem_available_mib()}), flush=True)
    print("=== per-session hit rate ===", flush=True)
    for sid in range(1, len(tok) + 1):
        row = [res.get((sid, t), (0, 0)) for t in range(1, rounds + 1)]
        print("s%-3d %s" % (sid, "  ".join("t%d %6d/%6d (%5.1f%%)" %
              (t, c, pt, 100.0 * c / pt if pt else 0) for t, (pt, c) in enumerate(row, 1))), flush=True)


def run_conc(mib, slots):
    """N sessions x 3 turns, every round fired CONCURRENTLY (one thread per session).

    This is the requirement "a session's cache must not be bound to a worker/slot": with `parallel`
    slots the server picks a slot per request, so round 2 of session A may land on a different slot
    than round 1 did.  Each session must still recall its own cache."""
    from concurrent.futures import ThreadPoolExecutor
    n = int(os.environ.get("NSESS", "4"))
    words = int(os.environ.get("WORDS", "2600"))
    rounds = int(os.environ.get("ROUNDS", "3"))
    p, lf = start(mib, slots)
    time.sleep(2)
    res = {}
    try:
        for turn in range(1, rounds + 1):
            def one(sid):
                u, wall, _ = chat(msgs(sid, turn, words))
                return sid, u, wall
            t0 = time.time()
            with ThreadPoolExecutor(max_workers=n) as ex:
                for sid, u, wall in ex.map(one, range(1, n + 1)):
                    pt = u["prompt_tokens"]
                    ct = (u.get("prompt_tokens_details") or {}).get("cached_tokens", 0)
                    res[(sid, turn)] = (pt, ct, wall)
                    print(json.dumps({"tag": "s%d-t%d" % (sid, turn), "prompt": pt, "cached": ct,
                                      "read": pt - ct, "wall": wall}), flush=True)
            print(json.dumps({"tag": "round%d" % turn, "wall": round(time.time() - t0, 2)}), flush=True)
        time.sleep(float(os.environ.get("SETTLE", "8")))
    finally:
        p.terminate()
        try:
            p.wait(timeout=120)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait()
        lf.close()
    print("=== per-session hit rate (concurrent) ===", flush=True)
    for sid in range(1, n + 1):
        print("s%-3d %s" % (sid, "  ".join("t%d %6d/%6d (%5.1f%%)" %
              (t, res[(sid, t)][1], res[(sid, t)][0], 100.0 * res[(sid, t)][1] / res[(sid, t)][0])
              for t in range(1, rounds + 1))), flush=True)


def rss_of(pid):
    try:
        with open("/proc/%d/status" % pid) as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1]) // 1024          # MiB
    except OSError:
        pass
    return None


def mem_available_mib():
    with open("/proc/meminfo") as f:
        for line in f:
            if line.startswith("MemAvailable:"):
                return int(line.split()[1]) // 1024
    return None


def rss_detail(pid):
    """RSS split: anonymous (the expert tier, KV pools, snapshots) vs file-backed vs shmem."""
    d = {"rss_mib": None, "anon_mib": None, "file_mib": None, "shmem_mib": None}
    try:
        with open("/proc/%d/status" % pid) as f:
            for line in f:
                k = line.split(":")[0]
                if k in ("VmRSS", "RssAnon", "RssFile", "RssShmem"):
                    d[{"VmRSS": "rss_mib", "RssAnon": "anon_mib",
                       "RssFile": "file_mib", "RssShmem": "shmem_mib"}[k]] = int(line.split()[1]) // 1024
    except OSError:
        pass
    return d


def engine_pid(server_pid):
    out = subprocess.run(["pgrep", "-P", str(server_pid)], capture_output=True, text=True).stdout.split()
    return int(out[0]) if out else None


def run_rss(mib, slots):
    """Is `--conversation-cache-mib` PRE-ALLOCATED or grown on demand?

    Start the engine, sample its RSS before any request (nothing is parked yet), then park several
    conversations and sample again.  A pre-allocated pool would show its whole budget in the first
    sample; a lazy one starts at the same RSS as --conversation-cache-mib 0 and grows only by what
    is actually parked."""
    words = int(os.environ.get("WORDS", "31500"))            # ~50K tokens per session
    n = int(os.environ.get("NSESS", "6"))
    p, lf = start(mib, slots)
    time.sleep(6)
    ep = engine_pid(p.pid)
    print(json.dumps({"tag": "baseline", "mib_arg": mib, "engine_pid": ep,
                      "engine_rss_mib": rss_of(ep), "host_mem_available_mib": mem_available_mib()}),
          flush=True)
    try:
        for sid in range(1, n + 2):                           # the last one is the park trigger
            w = words if sid <= n else 300
            u, wall, _ = chat(msgs(sid, 1, w), max_tokens=8, rb=4)
            print(json.dumps({"tag": "after-s%d" % sid, "prompt": u["prompt_tokens"],
                              "engine_rss_mib": rss_of(ep), "host_mem_available_mib": mem_available_mib()}),
                  flush=True)
        time.sleep(float(os.environ.get("SETTLE", "10")))
        print(json.dumps({"tag": "settled", "engine_rss_mib": rss_of(ep),
                          "host_mem_available_mib": mem_available_mib()}), flush=True)
    finally:
        p.terminate()
        try:
            p.wait(timeout=120)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait()
        lf.close()


def run_mem(mib, slots):
    """Baseline footprint of the engine with the model loaded and NOTHING cached, and how much of it
    is hard: locked (unevictable) vs clean file-backed page cache (the kernel may drop it)."""
    p, lf = start(mib, slots)
    time.sleep(8)
    ep = engine_pid(p.pid)
    d = rss_detail(ep)
    fields = {}
    with open("/proc/%d/status" % ep) as f:
        for line in f:
            k = line.split(":")[0]
            if k in ("VmRSS", "VmHWM", "VmLck", "VmPin", "VmSwap"):
                fields[k] = int(line.split()[1]) // 1024
    cg = {}
    for line in open("/proc/%d/cgroup" % ep):
        parts = line.strip().split(":")
        if len(parts) == 3 and parts[1] == "memory":
            base = "/sys/fs/cgroup/memory" + parts[2]
            for f in ("memory.usage_in_bytes", "memory.limit_in_bytes"):
                try:
                    cg[f] = int(open(base + "/" + f).read().strip()) // (1024 * 1024)
                except OSError:
                    pass
            try:
                for line2 in open(base + "/memory.stat"):
                    k, v = line2.split()
                    if k in ("rss", "shmem", "file", "inactive_file", "total_inactive_file"):
                        cg["stat_" + k] = int(v) // (1024 * 1024)
            except OSError:
                pass
    print(json.dumps({"tag": "mem", "mib_arg": mib, **d, **fields, **cg,
                      "host_mem_total_mib": open("/proc/meminfo").read().split("MemTotal:")[1].split()[0],
                      "host_mem_available_mib": mem_available_mib()}), flush=True)
    p.terminate()
    try:
        p.wait(timeout=90)
    except subprocess.TimeoutExpired:
        p.kill(); p.wait()
    lf.close()


def run_size(mib, slots):
    """One session per target size, one turn each: the parked snapshot's own byte count."""
    words = [int(x) for x in os.environ.get("SIZES", "12500,31500,63000").split(",")]
    settle = float(os.environ.get("SETTLE", "3"))
    p, lf = start(mib, slots)
    time.sleep(2)
    try:
        for sid, w in enumerate(words, 1):
            u, wall, _ = chat(msgs(sid, 1, w), max_tokens=8, rb=4)
            report("size-s%d" % sid, u, wall)
        time.sleep(settle)
    finally:
        p.terminate()
        try:
            p.wait(timeout=90)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait()
        lf.close()


def dump():
    print("=== engine conversation-cache lines ===", flush=True)
    pat = re.compile(r"conversation cache:|conversation_cache_mib|conversation cache slots")
    with open(ENG, encoding="utf-8", errors="replace") as f:
        for line in f:
            if pat.search(line):
                print(line.rstrip(), flush=True)


mib, slots = int(os.environ.get("MIB", "131072")), int(os.environ.get("SLOTS", "1000"))
{"mech": run_mech, "size": run_size, "fleet": run_fleet, "conc": run_conc, "rss": run_rss, "mem": run_mem}[MODE](mib, slots)
dump()
