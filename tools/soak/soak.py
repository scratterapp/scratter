#!/usr/bin/env python3
"""Soak test: let the daemon play for hours and check its memory stays flat.

    tools/soak/soak.py                      4 hours of the test film
    tools/soak/soak.py --hours 0.5          a shorter run
    tools/soak/soak.py -- --no-audio        anything after -- goes to the daemon

It starts the daemon (by default build/linux-cef/scratterd --test-film), and
once a minute writes one line to memory.csv:

  - how much memory the daemon and each kind of CEF helper process uses, and
    the total. CEF runs the page, the audio decoding and Widevine in separate
    processes (docs/learning/cef-host.md), all started by the daemon, so a
    leak can be in any of them.
  - how many seconds of audio the daemon played since the last line, read
    with `amctl status --json`. While playing that should be about a minute
    each time; much less means the sound stopped or kept cutting out.
  - how many times the test page reloaded the film since the last line.
    Google's test license server sometimes refuses a license and the film
    stops; the page then reloads it (src/daemon/cef_host/test_film_page.hpp).
    A gap in the sound in the same minute is the server's doing, not ours,
    so the summary counts those apart and doesn't fail the run for them.

At the end it stops the daemon the way Ctrl-C would, reads the "N late audio
buffers, M frames dropped" line the daemon prints when it quits, and writes
summary.txt: how fast memory grew per hour after a warm-up, and whether the
audio kept going. Everything lands in soak-results/<date-time>/, next to
the daemon's own output in daemon.log.

Linux only: it reads memory use from /proc. Needs Python 3 and nothing else.

Exit code: 0 when the run passed (or was too short to judge memory, but
otherwise went fine), 1 when it failed, 2 for bad arguments.
"""

import argparse
import csv
import datetime
import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# The first part of a run doesn't count towards the memory verdict: Chromium
# fills its caches and the page loads the film, so memory rises for a while
# before it settles. 15 minutes, or a quarter of a short run.
WARM_UP_SECONDS = 15 * 60

# A slope worked out from less than this, after the warm-up, says more about
# noise than about leaks, so the summary says "too short to judge" instead.
MIN_JUDGED_SECONDS = 30 * 60

# A line counts as "the sound stopped" when the daemon played less than this
# share of the time since the previous line.
MIN_PLAYED_SHARE = 0.9

# Process kinds, in the order they appear in memory.csv. "daemon" is the daemon
# itself (which is also CEF's browser process); the rest are CEF's helpers,
# named after their --type= argument (see docs/learning/cef-host.md).
KINDS = ["daemon", "renderer", "gpu", "utility", "zygote", "other"]


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Play for hours and log the daemon's memory use.",
        epilog="Arguments after -- go to the daemon instead of --test-film.",
    )
    parser.add_argument("--hours", type=float, default=4.0, help="how long to play (default 4)")
    parser.add_argument(
        "--interval", type=float, default=60.0, help="seconds between lines (default 60)"
    )
    parser.add_argument(
        "--build",
        type=Path,
        default=REPO / "build" / "linux-cef",
        help="build folder holding scratterd and amctl (default build/linux-cef)",
    )
    parser.add_argument(
        "--name",
        default=None,
        help="the daemon's --name, so a soak can run next to a daemon you use",
    )
    parser.add_argument(
        "--max-growth",
        type=float,
        default=10.0,
        help="MB per hour of growth after the warm-up that counts as a leak (default 10)",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="folder for the results (default soak-results/<date-time>)",
    )
    # Splitting at -- by hand keeps argparse from reading the daemon's flags.
    argv = sys.argv[1:]
    daemon_args = ["--test-film"]
    if "--" in argv:
        split = argv.index("--")
        argv, daemon_args = argv[:split], argv[split + 1 :]
    args = parser.parse_args(argv)
    if args.hours <= 0 or args.interval <= 0:
        parser.error("--hours and --interval must be more than 0")
    args.daemon_args = daemon_args
    return args


def find_program(build: Path, name: str) -> Path:
    path = build / name
    if not path.is_file():
        sys.exit(f"can't find {path}; build first, or point --build at the right folder")
    return path


# ---- Memory ------------------------------------------------------------------


def descendants(root_pid: int) -> list[int]:
    """The pids of every process started by root_pid, its children's children too."""
    children: dict[int, list[int]] = {}
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            stat = Path(f"/proc/{entry}/stat").read_text()
        except OSError:
            continue  # it quit while we looked
        # The second field is the program name in parentheses, which can hold
        # spaces, so count fields from the last ")". The parent pid is the
        # second field after it.
        ppid = int(stat[stat.rindex(")") + 2 :].split()[1])
        children.setdefault(ppid, []).append(int(entry))
    found = []
    todo = [root_pid]
    while todo:
        pid = todo.pop()
        for child in children.get(pid, []):
            found.append(child)
            todo.append(child)
    return found


def memory_kb(pid: int) -> tuple[int, int, int] | None:
    """(resident, anonymous, shared) memory of one process in KiB, or None if it's gone.

    Resident (VmRSS) is all of the process's memory that's in RAM right now,
    including shared libraries it shares with other processes. Anonymous
    (RssAnon) leaves out memory backed by files, which is where a leak shows:
    memory the program asked for and never gave back. Shared (RssShmem) is
    memory set up to be shared between processes: Chromium passes audio and
    other data between its processes that way, so a leak can hide there too.
    /proc/PID/status is
    readable for any process; smaps, with fairer numbers, needs debugger-level
    access that a sandboxed Chromium process may refuse.
    """
    try:
        status = Path(f"/proc/{pid}/status").read_text()
    except OSError:
        return None
    fields = dict(line.split(":", 1) for line in status.splitlines() if ":" in line)

    def kb(key: str) -> int:
        return int(fields.get(key, "0 kB").split()[0])

    return kb("VmRSS"), kb("RssAnon"), kb("RssShmem")


def process_kind(pid: int) -> str:
    try:
        cmdline = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
    except OSError:
        return "other"
    for arg in cmdline:
        if arg.startswith(b"--type="):
            kind = arg[len(b"--type=") :].decode(errors="replace")
            # Chromium calls the GPU process "gpu-process".
            kind = "gpu" if kind.startswith("gpu") else kind
            return kind if kind in KINDS else "other"
    return "other"


def measure(daemon_pid: int) -> dict:
    """Memory per process kind, in MB, plus totals and the number of processes."""
    resident = dict.fromkeys(KINDS, 0)
    anonymous = 0
    shared = 0
    processes = 0
    for pid in [daemon_pid, *descendants(daemon_pid)]:
        mem = memory_kb(pid)
        if mem is None:
            continue
        kind = "daemon" if pid == daemon_pid else process_kind(pid)
        resident[kind] += mem[0]
        anonymous += mem[1]
        shared += mem[2]
        processes += 1
    row = {f"{kind}_mb": round(kb / 1024, 1) for kind, kb in resident.items()}
    row["total_mb"] = round(sum(resident.values()) / 1024, 1)
    row["total_anon_mb"] = round(anonymous / 1024, 1)
    row["total_shmem_mb"] = round(shared / 1024, 1)
    row["processes"] = processes
    return row


# ---- Playback ----------------------------------------------------------------


def playback_status(amctl: Path, name: str | None) -> dict | None:
    """What `amctl status --json` says, or None if it couldn't tell."""
    command = [str(amctl)] + (["--name", name] if name else []) + ["status", "--json"]
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=10)
        return json.loads(done.stdout) if done.returncode == 0 else None
    except (subprocess.TimeoutExpired, json.JSONDecodeError):
        return None


# ---- Summary -----------------------------------------------------------------


def slope_per_hour(times: list[float], values: list[float]) -> float:
    """The straight line that best fits the points (least squares), as change per hour.

    Better than "last minus first", which a single spike at either end can
    swing either way.
    """
    n = len(times)
    mean_t = sum(times) / n
    mean_v = sum(values) / n
    spread = sum((t - mean_t) ** 2 for t in times)
    if spread == 0:
        return 0.0
    per_second = sum((t - mean_t) * (v - mean_v) for t, v in zip(times, values)) / spread
    return per_second * 3600


def summarize(rows: list[dict], args, shutdown: dict) -> tuple[str, bool]:
    lines = []
    passed = True

    def fail(text: str):
        nonlocal passed
        passed = False
        lines.append("FAIL: " + text)

    elapsed = rows[-1]["elapsed_s"] if rows else 0
    lines.append(f"ran for {elapsed / 3600:.2f} hours, {len(rows)} lines in memory.csv")

    # Memory, after the warm-up.
    warm_up = min(WARM_UP_SECONDS, elapsed / 4)
    judged = [r for r in rows if r["elapsed_s"] >= warm_up]
    if len(judged) >= 2:
        times = [r["elapsed_s"] for r in judged]
        lines.append(f"memory after the first {warm_up / 60:.0f} minutes (start -> end, growth):")
        columns = ["total_mb", "total_anon_mb", "total_shmem_mb"] + [f"{k}_mb" for k in KINDS]
        for column in columns:
            values = [r[column] for r in judged]
            if max(values) == 0:
                continue  # no process of that kind ran
            growth = slope_per_hour(times, values)
            lines.append(
                f"  {column[:-3]:<11} {values[0]:8.1f} -> {values[-1]:8.1f} MB, "
                f"{growth:+7.2f} MB/hour"
            )
        # The verdict goes by memory a leak can live in: anonymous plus shared.
        leakable = [r["total_anon_mb"] + r["total_shmem_mb"] for r in judged]
        growth = slope_per_hour(times, leakable)
        if times[-1] - times[0] < MIN_JUDGED_SECONDS:
            lines.append("memory: too short to judge (needs 30 minutes after the warm-up)")
        elif growth > args.max_growth:
            fail(
                f"memory grows: {growth:+.2f} MB/hour (anonymous + shared), "
                f"more than {args.max_growth:g}. The columns above show which process."
            )
        else:
            lines.append(f"memory: flat ({growth:+.2f} MB/hour, limit {args.max_growth:g})")
    else:
        lines.append("memory: too short to judge")

    # Playback. A daemon with no audio output doesn't move its play position,
    # so there's nothing to check then.
    with_sound = [r for r in rows if r["sample_rate_hz"] not in ("", 0)]
    # A reload near the end of one line's minute can leave its gap in the next
    # line, so a reload excuses both.
    excused = set()
    for i, r in enumerate(rows):
        if r["reloads"]:
            excused.update({i, i + 1})
    stalls = [r for i, r in enumerate(rows) if r["stalled"] and i not in excused]
    reloads = sum(r["reloads"] for r in rows)
    if reloads:
        lines.append(
            f"note: the test license server cut the film off {reloads} times and the page "
            "reloaded it; gaps in those minutes and the next don't count"
        )
    if not with_sound:
        lines.append("playback: not checked (no audio output)")
    elif stalls:
        when = ", ".join(f"{r['elapsed_s'] / 60:.0f}" for r in stalls[:10])
        fail(
            f"the sound stopped or cut out in {len(stalls)} of {len(with_sound)} lines "
            f"(at minute {when}{', ...' if len(stalls) > 10 else ''})"
        )
    else:
        lines.append("playback: played the whole time")
    not_playing = [r for r in rows if r["state"] not in ("playing", "")]
    if not_playing:
        lines.append(
            f"note: the daemon wasn't playing in {len(not_playing)} lines "
            "(paused or stopped by hand?)"
        )

    # What the daemon said when it quit.
    if shutdown.get("crashed"):
        fail(f"the daemon quit by itself with exit code {shutdown['exit_code']}; see daemon.log")
    elif shutdown.get("late") is None:
        fail(
            f"the daemon didn't print its 'stopped cleanly' line (exit code "
            f"{shutdown.get('exit_code')}); see daemon.log"
        )
    else:
        lines.append(
            f"stopped cleanly: {shutdown['late']} late audio buffers, "
            f"{shutdown['dropped']} frames dropped"
        )
        # A few late buffers at the start, while the film loads, are normal.
        # Dropped frames mean the ring buffer overflowed.

    lines.append("PASSED" if passed else "FAILED")
    return "\n".join(lines) + "\n", passed


# ---- The run -----------------------------------------------------------------

# What the test page prints when the license server cut the film off.
RELOAD_LINE = "page: reloading the film"

STOPPED_LINE = re.compile(r"stopped cleanly, (\d+) late audio buffers, (\d+) frames dropped")


def stop_daemon(daemon: subprocess.Popen, log_path: Path) -> dict:
    """Stops the daemon like Ctrl-C would and reads its last words from the log."""
    crashed = daemon.poll() is not None
    if not crashed:
        daemon.send_signal(signal.SIGTERM)
        try:
            daemon.wait(timeout=30)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
    result = {"crashed": crashed, "exit_code": daemon.returncode, "late": None, "dropped": None}
    if match := STOPPED_LINE.search(log_path.read_text(errors="replace")):
        result["late"], result["dropped"] = int(match[1]), int(match[2])
    return result


def main() -> int:
    args = parse_arguments()
    daemon_path = find_program(args.build, "scratterd")
    amctl = find_program(args.build, "amctl")

    out = args.out or REPO / "soak-results" / datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    log_path = out / "daemon.log"
    csv_path = out / "memory.csv"

    command = [str(daemon_path)] + (["--name", args.name] if args.name else []) + args.daemon_args
    print(f"starting {' '.join(command)}")
    print(f"writing to {out}/ for {args.hours:g} hours; Ctrl-C stops early and still summarizes")

    columns = ["time", "elapsed_s"] + [f"{k}_mb" for k in KINDS]
    columns += ["total_mb", "total_anon_mb", "total_shmem_mb", "processes"]
    columns += ["state", "position_s", "played_s", "ring_fill", "sample_rate_hz", "stalled"]
    columns += ["reloads"]

    rows = []
    # Ctrl-C here should end the run early, not kill the script before it
    # stops the daemon and writes the summary.
    interrupted = False

    def on_ctrl_c(_signum, _frame):
        nonlocal interrupted
        interrupted = True

    with open(log_path, "w") as log, open(csv_path, "w", newline="") as csv_file:
        # start_new_session: Ctrl-C in this terminal reaches only the script,
        # which then stops the daemon itself.
        daemon = subprocess.Popen(
            command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True
        )
        signal.signal(signal.SIGINT, on_ctrl_c)
        writer = csv.DictWriter(csv_file, fieldnames=columns)
        writer.writeheader()

        start = time.monotonic()
        end = start + args.hours * 3600
        last = None  # the previous row, to see how far playback moved since
        log_read = 0  # how much of daemon.log the reload count has looked at
        next_line = start + args.interval
        while not interrupted and daemon.poll() is None:
            now = time.monotonic()
            if now < next_line:
                time.sleep(min(1.0, next_line - now))
                continue
            next_line += args.interval

            row = {"time": datetime.datetime.now().strftime("%H:%M:%S")}
            row["elapsed_s"] = round(now - start)
            row.update(measure(daemon.pid))
            status = playback_status(amctl, args.name) or {}
            row["state"] = status.get("state", "")
            row["position_s"] = round(status.get("position_micros", 0) / 1e6, 1)
            row["ring_fill"] = status.get("ring_buffer_fill", "")
            row["sample_rate_hz"] = status.get("sample_rate_hz", "")
            # Seconds of audio played since the last line. The position goes
            # back to 0 on stop, so a smaller number than last time isn't a
            # stall, just a stop.
            played = row["position_s"] - last["position_s"] if last else None
            row["played_s"] = round(played, 1) if played is not None else ""
            span = row["elapsed_s"] - last["elapsed_s"] if last else 0
            # Only a daemon with sound output moves its play position.
            row["stalled"] = (
                row["state"] == "playing"
                and row["sample_rate_hz"] not in ("", 0)
                and played is not None
                and 0 <= played < MIN_PLAYED_SHARE * span
            )
            # Reloads since the last line: the new part of daemon.log.
            with open(log_path, errors="replace") as daemon_log:
                daemon_log.seek(log_read)
                new_output = daemon_log.read()
                log_read = daemon_log.tell()
            row["reloads"] = new_output.count(RELOAD_LINE)
            writer.writerow(row)
            csv_file.flush()
            rows.append(row)
            last = row
            played_text = "-" if played is None else f"{played:.1f}"
            print(
                f"{row['time']}  {row['elapsed_s'] / 60:6.1f} min  "
                f"total {row['total_mb']:7.1f} MB  ({row['processes']} processes)  "
                f"{row['state'] or '?'}, played {played_text} s"
            )
            if now >= end:
                break

        if interrupted:
            print("stopped early (Ctrl-C)")
        shutdown = stop_daemon(daemon, log_path)

    summary, passed = summarize(rows, args, shutdown)
    (out / "summary.txt").write_text(summary)
    print()
    print(summary, end="")
    print(f"(saved in {out}/summary.txt)")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
