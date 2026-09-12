#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Samples what a timing fault would show up in, once per second.

The reason this exists alongside the wire probe: a late CIP frame proves that
something was late, not what. CPU time, run-queue delay, involuntary context
switches and interface counters, taken at the same second, are what separate
"the scheduler did not run the stack" from "the NIC dropped it".

  eip_sampler.py <seconds> <out.csv> <netns>[,<netns>...] -- <name>=<pid> ...
"""
import os, sys, time

def read(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return ""

def proc_stat(pid):
    """utime, stime, threads, and the scheduler's own view of delay."""
    s = read(f"/proc/{pid}/stat")
    if not s:
        return None
    # comm may contain spaces and parentheses; split on the last ')'.
    rest = s[s.rfind(")") + 2:].split()
    utime, stime = int(rest[11]), int(rest[12])
    threads = int(rest[17])
    st = read(f"/proc/{pid}/status")
    vol = nonvol = 0
    rss = 0
    for line in st.splitlines():
        if line.startswith("voluntary_ctxt_switches:"):     vol = int(line.split()[1])
        elif line.startswith("nonvoluntary_ctxt_switches:"): nonvol = int(line.split()[1])
        elif line.startswith("VmRSS:"):                      rss = int(line.split()[1])
    # /proc/<pid>/schedstat: run time, *run-queue wait time*, timeslices.
    # The middle field is the one that matters here - it is literally how long
    # the task was runnable but not running.
    sched = read(f"/proc/{pid}/schedstat").split()
    runq = int(sched[1]) if len(sched) >= 2 else 0
    return dict(utime=utime, stime=stime, threads=threads, vol=vol,
                nonvol=nonvol, rss_kb=rss, runq_ns=runq)

def netns_counters(ns, dev):
    out = os.popen(f"ip netns exec {ns} cat /proc/net/dev 2>/dev/null").read()
    for line in out.splitlines():
        if line.strip().startswith(dev + ":"):
            f = line.split(":")[1].split()
            return dict(rx_pkts=int(f[1]), rx_err=int(f[2]), rx_drop=int(f[3]),
                        tx_pkts=int(f[9]), tx_err=int(f[10]), tx_drop=int(f[11]))
    return dict(rx_pkts=0, rx_err=0, rx_drop=0, tx_pkts=0, tx_err=0, tx_drop=0)

def netns_udp(ns):
    out = os.popen(f"ip netns exec {ns} cat /proc/net/snmp 2>/dev/null").read()
    lines = out.splitlines()
    for i, line in enumerate(lines):
        if line.startswith("Udp:") and not line.split()[1].isdigit():
            keys = line.split()[1:]
            vals = lines[i + 1].split()[1:]
            d = dict(zip(keys, map(int, vals)))
            return dict(in_dg=d.get("InDatagrams", 0), no_port=d.get("NoPorts", 0),
                        in_err=d.get("InErrors", 0), rcvbuf_err=d.get("RcvbufErrors", 0),
                        snd_err=d.get("SndbufErrors", 0))
    return dict(in_dg=0, no_port=0, in_err=0, rcvbuf_err=0, snd_err=0)

def cpu_lines():
    out = {}
    for line in read("/proc/stat").splitlines():
        if line.startswith("cpu"):
            f = line.split()
            if f[0] == "cpu" or f[0][3:].isdigit():
                out[f[0]] = list(map(int, f[1:8]))
    return out

def main():
    secs = int(sys.argv[1]); path = sys.argv[2]
    namespaces = sys.argv[3].split(",")
    procs = {}
    for a in sys.argv[sys.argv.index("--") + 1:]:
        n, p = a.split("=")
        procs[n] = int(p)

    hz = os.sysconf("SC_CLK_TCK")
    names = sorted(procs)
    cols = ["t"]
    for n in names:
        cols += [f"{n}_cpu_pct", f"{n}_thr", f"{n}_rss_kb",
                 f"{n}_vol_cs", f"{n}_nonvol_cs", f"{n}_runq_us"]
    for ns in namespaces:
        cols += [f"{ns}_rx", f"{ns}_rx_err", f"{ns}_rx_drop",
                 f"{ns}_tx", f"{ns}_tx_err", f"{ns}_tx_drop",
                 f"{ns}_udp_inerr", f"{ns}_udp_rcvbuferr", f"{ns}_udp_noport"]
    ncpu = len([k for k in cpu_lines() if k != "cpu"])
    cols += [f"cpu{i}_busy_pct" for i in range(ncpu)] + ["cpu_all_busy_pct"]

    prev_p = {n: proc_stat(p) for n, p in procs.items()}
    prev_n = {ns: netns_counters(ns, "vethA" if ns.endswith("A") else "vethB")
              for ns in namespaces}
    prev_u = {ns: netns_udp(ns) for ns in namespaces}
    prev_c = cpu_lines()

    with open(path, "w") as f:
        f.write(",".join(cols) + "\n")
        t0 = time.monotonic()
        for _ in range(secs):
            time.sleep(1.0)
            row = [f"{time.monotonic() - t0:.1f}"]
            for n in names:
                cur = proc_stat(procs[n])
                pv = prev_p[n]
                if cur is None or pv is None:
                    row += ["", "", "", "", "", ""]
                else:
                    dt_cpu = (cur["utime"] + cur["stime"]) - (pv["utime"] + pv["stime"])
                    row += [f"{100.0 * dt_cpu / hz:.1f}", str(cur["threads"]),
                            str(cur["rss_kb"]),
                            str(cur["vol"] - pv["vol"]),
                            str(cur["nonvol"] - pv["nonvol"]),
                            f"{(cur['runq_ns'] - pv['runq_ns']) / 1000.0:.0f}"]
                prev_p[n] = cur if cur else pv
            for ns in namespaces:
                dev = "vethA" if ns.endswith("A") else "vethB"
                c = netns_counters(ns, dev); p = prev_n[ns]
                u = netns_udp(ns);           q = prev_u[ns]
                row += [str(c["rx_pkts"] - p["rx_pkts"]), str(c["rx_err"] - p["rx_err"]),
                        str(c["rx_drop"] - p["rx_drop"]),
                        str(c["tx_pkts"] - p["tx_pkts"]), str(c["tx_err"] - p["tx_err"]),
                        str(c["tx_drop"] - p["tx_drop"]),
                        str(u["in_err"] - q["in_err"]),
                        str(u["rcvbuf_err"] - q["rcvbuf_err"]),
                        str(u["no_port"] - q["no_port"])]
                prev_n[ns] = c; prev_u[ns] = u
            cur_c = cpu_lines()
            for i in range(ncpu):
                k = f"cpu{i}"
                a, b = cur_c.get(k), prev_c.get(k)
                if a and b:
                    tot = sum(a) - sum(b)
                    idle = (a[3] - b[3]) + (a[4] - b[4])
                    row.append(f"{100.0 * (tot - idle) / tot:.1f}" if tot else "")
                else:
                    row.append("")
            a, b = cur_c.get("cpu"), prev_c.get("cpu")
            tot = sum(a) - sum(b); idle = (a[3] - b[3]) + (a[4] - b[4])
            row.append(f"{100.0 * (tot - idle) / tot:.1f}" if tot else "")
            prev_c = cur_c
            f.write(",".join(row) + "\n")
            f.flush()

main()
