#!/usr/bin/env python3
"""How much memory, CPU and GPU a running daemon costs, process by process.

    tools/perf/footprint.py                 watch the running scratterd for 30 s
    tools/perf/footprint.py --seconds 60    watch longer
    tools/perf/footprint.py --threads 5     also name each process's 5 busiest threads
    sudo python3 tools/perf/footprint.py    also see what sandboxed processes use

Start the daemon first (in another terminal window), let it play for a minute,
then run this. It finds the daemon and every process the daemon started (with
CEF on, that's Chromium's renderer, GPU, utility and zygote processes) and
prints, for each one:

  - RSS: everything of the process that sits in RAM, shared libraries included.
    libcef.so is mapped by every CEF process, so adding RSS up counts it
    several times over.
  - PSS: the same, but a page shared by N processes counts 1/N in each. The
    PSS column adds up to what the whole thing really costs the machine.
  - own: anonymous plus shared memory (RssAnon + RssShmem), the memory the
    process asked for itself. The soak test watches the same number.
  - CPU: share of one CPU core used while watching. 100% is one core flat out.
  - wakeups/s: how often its threads were switched in, per second. Each one
    costs a little power, so an idle player wants few. With --threads N, the
    N threads that woke most are listed under each process, by name.
  - GPU: whether it has the graphics card open (/dev/dri), and how much video
    memory it holds if the driver says (amdgpu and i915/xe do).

PSS and GPU need the same access as a debugger. Chromium's sandboxed processes
refuse that to normal users, so without sudo those columns show "?" for them.
sudo only reads /proc here; it doesn't start or change anything.

Linux only. Needs Python 3 and nothing else. docs/learning/performance.md
explains the numbers.
"""

import argparse
import os
import re
import sys
import time
from pathlib import Path

DAEMON_NAME = "scratterd"
TICKS_PER_SECOND = os.sysconf("SC_CLK_TCK")

# Spec section 1 and docs/RATIONALE.md.
TARGET_HEADLESS_MB = 115
TARGET_DAEMON_CPU_PERCENT = 0.5


def parse_arguments():
    parser = argparse.ArgumentParser(description="Show what a running daemon costs.")
    parser.add_argument("--pid", type=int, help="the daemon's pid (default: find scratterd)")
    parser.add_argument(
        "--seconds", type=float, default=30.0, help="how long to measure CPU (default 30)"
    )
    parser.add_argument(
        "--threads", type=int, default=0, metavar="N",
        help="also show each process's N busiest threads (default 0)"
    )
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be more than 0")
    if args.threads < 0:
        parser.error("--threads can't be below 0")
    return args


# ---- Finding the processes ----------------------------------------------------


def stat_fields(pid: int) -> list[str] | None:
    """/proc/PID/stat after the program name, or None if the process is gone.

    The program name sits in parentheses and can hold spaces, so split after
    the last ")". Field 0 here is the state, 1 the parent pid, 11 and 12 the
    user and system CPU time in clock ticks.
    """
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return None
    return stat[stat.rindex(")") + 2 :].split()


def find_daemon() -> int:
    found = []
    for entry in os.listdir("/proc"):
        if entry.isdigit():
            try:
                if Path(f"/proc/{entry}/comm").read_text().strip() == DAEMON_NAME:
                    found.append(int(entry))
            except OSError:
                pass
    if not found:
        sys.exit(f"no {DAEMON_NAME} is running; start it first, or pass --pid")
    if len(found) > 1:
        sys.exit(f"{len(found)} {DAEMON_NAME} processes are running; pick one with --pid")
    return found[0]


def descendants(root_pid: int) -> list[int]:
    """The pids of every process started by root_pid, its children's children too."""
    children: dict[int, list[int]] = {}
    for entry in os.listdir("/proc"):
        if entry.isdigit() and (fields := stat_fields(int(entry))):
            children.setdefault(int(fields[1]), []).append(int(entry))
    found = []
    todo = [root_pid]
    while todo:
        for child in children.get(todo.pop(), []):
            found.append(child)
            todo.append(child)
    return found


def process_kind(pid: int, daemon_pid: int) -> str:
    if pid == daemon_pid:
        return "daemon"
    try:
        cmdline = Path(f"/proc/{pid}/cmdline").read_bytes().replace(b"\0", b" ").decode()
    except (OSError, UnicodeDecodeError):
        return "?"
    kind = re.search(r"--type=([\w-]+)", cmdline)
    if not kind:
        return cmdline.split(" ")[0].rsplit("/", 1)[-1] or "?"  # not Chromium's
    # Utility processes say which service they run, like
    # --utility-sub-type=audio.mojom.AudioService.
    service = re.search(r"--utility-sub-type=(\w+)\.", cmdline)
    return f"{kind[1]}: {service[1]}" if service else kind[1]


# ---- Reading one process ------------------------------------------------------


def kb_fields(path: str) -> dict[str, int] | None:
    """The "Name: 123 kB" lines of a /proc file, or None if it can't be read."""
    try:
        text = Path(path).read_text()
    except OSError:
        return None
    fields = {}
    for line in text.splitlines():
        name, _, value = line.partition(":")
        parts = value.split()
        if parts and parts[0].isdigit():
            fields[name] = int(parts[0])
    return fields


def cpu_ticks(pid: int) -> int | None:
    fields = stat_fields(pid)
    return int(fields[11]) + int(fields[12]) if fields else None


def context_switches(pid: int) -> dict[str, int] | None:
    """Times each thread of the process was switched in so far, by thread id.

    /proc/PID/status only counts the main thread, so read every thread's.
    They're kept apart because Chromium starts and ends threads all the time:
    a total would drop when a busy thread ends, and the difference between
    two totals could even come out below zero.
    """
    counts = {}
    try:
        threads = os.listdir(f"/proc/{pid}/task")
    except OSError:
        return None
    for tid in threads:
        fields = kb_fields(f"/proc/{pid}/task/{tid}/status")
        if fields:
            counts[tid] = (fields.get("voluntary_ctxt_switches", 0) +
                           fields.get("nonvoluntary_ctxt_switches", 0))
    return counts


def busiest_threads(pid: int, before: dict[str, int], after: dict[str, int], elapsed: float,
                    count: int) -> list[tuple[str, float]]:
    """The `count` threads that woke most, as (name, wake-ups per second).

    Threads with the same name are added together; Chromium runs several
    "ThreadPoolForeg" workers, for example. Linux keeps only the first 15
    letters of a thread's name.
    """
    by_name: dict[str, int] = {}
    for tid, n in after.items():
        try:
            name = Path(f"/proc/{pid}/task/{tid}/comm").read_text().strip()
        except OSError:
            continue  # it ended just now
        by_name[name] = by_name.get(name, 0) + n - before.get(tid, 0)
    busiest = sorted(by_name.items(), key=lambda item: item[1], reverse=True)[:count]
    return [(name, n / elapsed) for name, n in busiest if n > 0]


def gpu_use(pid: int) -> tuple[bool, int] | None:
    """(has the GPU open, video memory in KiB), or None if we may not look.

    Every open file is a link in /proc/PID/fd; the graphics card is
    /dev/dri/cardN or /dev/dri/renderDN. Drivers that support it list the
    memory each open file holds in /proc/PID/fdinfo/N, as drm-memory-vram or
    drm-total-vram (an older and a newer name).
    """
    try:
        fds = os.listdir(f"/proc/{pid}/fd")
    except OSError:
        return None
    opened = False
    vram_kb = 0
    for fd in fds:
        try:
            target = os.readlink(f"/proc/{pid}/fd/{fd}")
        except OSError:
            continue
        if not target.startswith("/dev/dri/"):
            continue
        opened = True
        info = kb_fields(f"/proc/{pid}/fdinfo/{fd}") or {}
        vram_kb += info.get("drm-total-vram", info.get("drm-memory-vram", 0))
    return opened, vram_kb


# ---- Printing -----------------------------------------------------------------


def mb(kb: int | None) -> str:
    return "?" if kb is None else f"{kb / 1024:.1f}"


def main():
    args = parse_arguments()
    daemon_pid = args.pid or find_daemon()
    pids = [daemon_pid, *descendants(daemon_pid)]

    ticks_before = {pid: cpu_ticks(pid) for pid in pids}
    switches_before = {pid: context_switches(pid) for pid in pids}
    started = time.monotonic()
    print(f"watching {DAEMON_NAME} (pid {daemon_pid}) and {len(pids) - 1} processes it "
          f"started for {args.seconds:.0f} s...", flush=True)
    time.sleep(args.seconds)
    elapsed = time.monotonic() - started

    header = (f"{'process':<32} {'pid':>7} {'RSS MB':>8} {'PSS MB':>8} {'own MB':>8} "
              f"{'CPU %':>6} {'wakeups/s':>10}  GPU")
    print()
    print(header)
    print("-" * len(header))

    totals = {"rss": 0, "pss": 0, "own": 0, "cpu": 0.0, "wakeups": 0.0}
    pss_complete = True
    gpu_known = True
    gpu_users = []
    vram_kb = 0
    for pid in pids:
        status = kb_fields(f"/proc/{pid}/status")
        ticks = cpu_ticks(pid)
        switches = context_switches(pid)
        if status is None or ticks is None or ticks_before[pid] is None:
            continue  # it quit while we watched
        kind = process_kind(pid, daemon_pid)
        rss = status.get("VmRSS", 0)
        own = status.get("RssAnon", 0) + status.get("RssShmem", 0)
        rollup = kb_fields(f"/proc/{pid}/smaps_rollup")
        pss = rollup.get("Pss") if rollup else None
        cpu = 100 * (ticks - ticks_before[pid]) / TICKS_PER_SECOND / elapsed
        wakeups = None
        if switches is not None and switches_before[pid] is not None:
            # Threads that ended while we watched aren't counted; ones that
            # started count from 0.
            before = switches_before[pid]
            wakeups = sum(n - before.get(tid, 0) for tid, n in switches.items()) / elapsed
        gpu = gpu_use(pid)

        totals["rss"] += rss
        totals["own"] += own
        totals["cpu"] += cpu
        totals["wakeups"] += wakeups or 0
        if pss is None:
            pss_complete = False
        else:
            totals["pss"] += pss
        if gpu is None:
            gpu_known = False
            gpu_text = "?"
        elif gpu[0]:
            gpu_users.append(kind)
            vram_kb += gpu[1]
            gpu_text = f"open, {mb(gpu[1])} MB VRAM" if gpu[1] else "open"
        else:
            gpu_text = "no"
        wakeups_text = "?" if wakeups is None else f"{wakeups:.0f}"
        print(f"{kind:<32} {pid:>7} {mb(rss):>8} {mb(pss):>8} {mb(own):>8} "
              f"{cpu:>6.1f} {wakeups_text:>10}  {gpu_text}")
        if args.threads and wakeups is not None:
            for name, rate in busiest_threads(pid, switches_before[pid], switches, elapsed,
                                              args.threads):
                print(f"  {name:<30} {'':>41} {rate:>10.0f}")

    print("-" * len(header))
    pss_total = mb(totals["pss"]) + ("+" if not pss_complete else "")
    print(f"{'total':<32} {'':>7} {mb(totals['rss']):>8} {pss_total:>8} "
          f"{mb(totals['own']):>8} {totals['cpu']:>6.1f} {totals['wakeups']:>10.0f}")
    print()

    # Against the goals in the spec (section 1) and docs/RATIONALE.md.
    if pss_complete:
        pss_mb = totals["pss"] / 1024
        verdict = "meets" if pss_mb < TARGET_HEADLESS_MB else "over"
        print(f"memory: {pss_mb:.0f} MB really used (PSS), goal under {TARGET_HEADLESS_MB} MB: "
              f"{verdict}")
    else:
        print(f"memory: PSS unreadable for some processes; run with sudo for the real total "
              f"(goal under {TARGET_HEADLESS_MB} MB)")
    verdict = "meets" if totals["cpu"] < TARGET_DAEMON_CPU_PERCENT else "over"
    print(f"CPU: {totals['cpu']:.1f}% of one core, all processes together, goal under "
          f"{TARGET_DAEMON_CPU_PERCENT}%: {verdict}")
    if not gpu_known:
        print("GPU: some processes wouldn't say; run with sudo to check (goal: nothing open)")
    elif gpu_users:
        print(f"GPU: open in {', '.join(gpu_users)}, {mb(vram_kb)} MB VRAM the driver reports "
              f"(goal: nothing open, 0 MB)")
    else:
        print("GPU: no process has the graphics card open (goal: nothing open): meets")


if __name__ == "__main__":
    main()
