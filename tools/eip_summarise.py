#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Turns a directory of eip_rpi_eval.sh runs into the comparison tables.

Kept separate from the runs themselves so a table can be regenerated without
re-measuring, and so the parsing is visible rather than hidden in a shell
pipeline.
"""
import csv, os, re, sys

def parse_probe(path):
    """One block per CIP connection ID; direction is decided by the addresses."""
    txt = open(path).read() if os.path.exists(path) else ""
    head = {}
    m = re.search(r"capture duration\s*:\s*([\d.]+)", txt)
    head["secs"] = float(m.group(1)) if m else 0.0
    m = re.search(r"configured RPI\s*:\s*(\d+)", txt)
    head["rpi"] = int(m.group(1)) if m else 0
    m = re.search(r"CIP conn timeout\s*:\s*(\d+)", txt)
    head["budget"] = int(m.group(1)) if m else 0

    conns = []
    for block in txt.split("--- connection ")[1:]:
        c = {}
        m = re.match(r"0x([0-9A-F]+)\s+([\d.]+) -> ([\d.]+)", block)
        if not m:
            continue
        c["id"], c["src"], c["dst"] = m.group(1), m.group(2), m.group(3)
        def g(pat, cast=float, default=0):
            mm = re.search(pat, block)
            return cast(mm.group(1)) if mm else default
        c["packets"]  = g(r"packets\s*:\s*(\d+)", int)
        c["span"]     = g(r"packets\s*:\s*\d+ over ([\d.]+) s")
        c["expected"] = g(r"expected @ RPI\s*:\s*(\d+)", int)
        c["mean"]     = g(r"cycle mean\s*:\s*([\d.]+)")
        c["min"]      = g(r"cycle min / max\s*:\s*(\d+)", int)
        c["max"]      = g(r"cycle min / max\s*:\s*\d+ / (\d+)", int)
        c["sd"]       = g(r"cycle stddev\s*:\s*([\d.]+)")
        c["jitter"]   = g(r"max jitter[^:]*:\s*(\d+)", int)
        c["p50"]      = g(r"P50 / P95 / P99\s*:\s*(\d+)", int)
        c["p95"]      = g(r"P50 / P95 / P99\s*:\s*\d+ / (\d+)", int)
        c["p99"]      = g(r"P50 / P95 / P99\s*:\s*\d+ / \d+ / (\d+)", int)
        c["p999"]     = g(r"P99\.9 / P99\.99\s*:\s*(\d+)", int)
        c["late2x"]   = g(r"deltas >2xRPI\s*:\s*(\d+)", int)
        c["over_tmo"] = g(r"deltas >CIP budget\s*:\s*(\d+)", int)
        c["seq_lost"] = g(r"seq gaps / lost\s*:\s*\d+ / (\d+)", int)
        c["seq_gaps"] = g(r"seq gaps / lost\s*:\s*(\d+)", int)
        c["maxcons"]  = g(r"max consecutive missed\s*:\s*(\d+)", int)
        conns.append(c)
    return head, conns

def parse_core(path):
    """Timeouts on the IPC leg, from the PLC core's own summary line."""
    txt = open(path).read() if os.path.exists(path) else ""
    m = re.search(r"exchanges=(\d+) timeouts=(\d+)", txt)
    d = {"exchanges": int(m.group(1)), "timeouts": int(m.group(2))} if m else {}
    m = re.search(r"fresh=(\d+) stale=(\d+)\s+corrupt=(\d+)", txt)
    if m:
        d.update(fresh=int(m.group(1)), stale=int(m.group(2)), corrupt=int(m.group(3)))
    d["failsafe_events"] = len(re.findall(r"applying HOLD failsafe", txt))
    return d

def parse_resources(path):
    if not os.path.exists(path):
        return {}
    rows = list(csv.DictReader(open(path)))
    if not rows:
        return {}
    def col(k, cast=float):
        return [cast(r[k]) for r in rows if r.get(k) not in (None, "")]
    out = {}
    for k in ("plcA_core", "plcA_stack", "plcB_core", "plcB_stack"):
        v = col(f"{k}_cpu_pct")
        out[f"{k}_cpu_mean"] = sum(v) / len(v) if v else 0
        out[f"{k}_cpu_max"]  = max(v) if v else 0
        r = col(f"{k}_runq_us")
        out[f"{k}_runq_max"] = max(r) if r else 0
        m = col(f"{k}_rss_kb", int)
        out[f"{k}_rss_first"] = m[0] if m else 0
        out[f"{k}_rss_last"]  = m[-1] if m else 0
    v = col("cpu_all_busy_pct")
    out["host_cpu_mean"] = sum(v) / len(v) if v else 0
    out["host_cpu_max"]  = max(v) if v else 0
    for k in ("plcA_rx_drop", "plcA_tx_drop", "plcB_rx_drop", "plcB_tx_drop",
              "plcA_rx_err", "plcB_rx_err",
              "plcA_udp_inerr", "plcB_udp_inerr",
              "plcA_udp_rcvbuferr", "plcB_udp_rcvbuferr"):
        try:
            out[k] = sum(col(k, int))
        except Exception:
            out[k] = 0
    return out

def meta(path):
    d = {}
    if os.path.exists(path):
        for line in open(path):
            for kv in line.split():
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    d[k] = v
    return d

def load(run_dir):
    head, conns = parse_probe(os.path.join(run_dir, "probe.txt"))
    r = dict(name=os.path.basename(run_dir), **head)
    r["meta"] = meta(os.path.join(run_dir, "meta.txt"))
    # 10.10.0.1 is the Adapter (CIP target): what it sends is T->O.
    r["t2o"] = [c for c in conns if c["src"].endswith(".1")]
    r["o2t"] = [c for c in conns if c["src"].endswith(".2")]
    r["coreA"] = parse_core(os.path.join(run_dir, "plcA-core.log"))
    r["coreB"] = parse_core(os.path.join(run_dir, "plcB-core.log"))
    r["res"] = parse_resources(os.path.join(run_dir, "resources.csv"))
    # Counted from the scanner's own log rather than from meta.txt: meta is
    # written at the end of a run, so a run that was interrupted has none, and
    # defaulting the ForwardOpen count to zero would hide a reconnect.
    log = os.path.join(run_dir, "plcB-stack.log")
    txt = open(log).read() if os.path.exists(log) else ""
    r["forward_opens"] = txt.count("Open IO connection")
    r["closed_by_timeout"] = txt.count("is closed by timeout")
    return r

def agg(conns, key, how="sum"):
    if not conns:
        return 0
    vals = [c[key] for c in conns]
    return sum(vals) if how == "sum" else max(vals)

def main():
    dirs = sorted(sys.argv[1:])
    print(f"{'run':26s} {'dir':5s} {'pkts':>8s} {'exp':>8s} {'deliv%':>7s} "
          f"{'mean':>8s} {'p50':>7s} {'p95':>7s} {'p99':>7s} {'p99.9':>8s} "
          f"{'max':>8s} {'sd':>7s} {'lost':>5s} {'>bud':>5s} {'cons':>5s} {'FO':>3s} {'CTO':>4s}")
    for d in dirs:
        r = load(d)
        for lbl, conns in (("T->O", r["t2o"]), ("O->T", r["o2t"])):
            if not conns:
                print(f"{r['name']:26s} {lbl:5s}  (no packets captured)")
                continue
            pk = agg(conns, "packets")
            ex = agg(conns, "expected")
            print(f"{r['name']:26s} {lbl:5s} {pk:8d} {ex:8d} "
                  f"{100.0*pk/ex if ex else 0:7.2f} "
                  f"{agg(conns,'mean','max'):8.0f} {agg(conns,'p50','max'):7d} "
                  f"{agg(conns,'p95','max'):7d} {agg(conns,'p99','max'):7d} "
                  f"{agg(conns,'p999','max'):8d} {agg(conns,'max','max'):8d} "
                  f"{agg(conns,'sd','max'):7.0f} {agg(conns,'seq_lost'):5d} "
                  f"{agg(conns,'over_tmo'):5d} {agg(conns,'maxcons','max'):5d} "
                  f"{r['forward_opens']:3d} {r['closed_by_timeout']:4d}")

if __name__ == "__main__":
    main()
