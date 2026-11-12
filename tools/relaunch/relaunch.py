#!/usr/bin/env python3
"""Relaunch test: open and close the window 50 times while music plays.

    tools/relaunch/relaunch.py                  50 times, against the clear film
    tools/relaunch/relaunch.py --times 10       fewer
    tools/relaunch/relaunch.py -- --test-film   anything after -- goes to the daemon

This is the spec's phase 5 gate (section 6, issue #43): closing am-ui and
starting it again must never make the daemon's sound skip, because the window
is only a remote control and the music lives in the daemon.

It starts the daemon (by default build/linux-cef-release/scratterd
--clear-film; a Release build, so the start-up times mean something) and
waits until it plays. Then, 50 times in a row, it starts am-ui with
--print-startup-time, waits until the window shows the daemon's state, and
closes it again: every other time cleanly and every other time with kill -9
(TerminateProcess on Windows), as if it crashed. A clean close is SIGTERM on
Linux, which SDL turns into the same quit as the window's close button; on
Windows it's WM_CLOSE, the message the close button itself sends. A window
that sticks around or crashes on a clean close fails the run.

Sound: while it plays, the daemon prints "audio: N late audio buffers, M
frames dropped so far" whenever either number goes up. A late buffer is a gap
the sound card had to fill with silence, a dropped frame is audio thrown away
because the ring buffer was full; either is a glitch you'd hear. The script
notes the numbers once playback has settled (the film's start can leave one
late buffer, see docs/BENCHMARKS.md), and both must be the same after the 50
windows. It also checks that the play position kept moving with the clock.

Start-up time: am-ui prints when its first frame reached the screen, and when
the first frame showing the daemon's state did. The script noted the time just
before it started am-ui, so it can give the whole time from launch, including
loading the program before main() runs. summary.txt lists the median, 90th
percentile and slowest of each; launches.csv has every launch.

Everything lands in relaunch-results/<date-time>/: summary.txt, launches.csv,
daemon.log and ui.log (what the windows printed besides the timings).

Linux and Windows. Needs a desktop session for the window; on Linux,
SDL_VIDEO_DRIVER=offscreen works without one. On Windows a clean close needs a
real window to send WM_CLOSE to, so offscreen windows can't be closed cleanly
there. On Windows the default build folder is build/windows-msvc-release, which
has no CEF: point --build at a CEF build (build/windows-cef) for the films, or
pass -- to play the tone. Python 3 and nothing else (on Windows, `py` runs it).
The daemon must not already be running under the same name; use --name to run
next to one you use.

The summary ends in PASSED or FAILED. Exit code: 0 for PASSED, 1 for FAILED,
2 for bad arguments.
"""

import argparse
import csv
import datetime
import json
import os
import queue
import re
import signal
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
WINDOWS = os.name == "nt"

# The clock am-ui's "steady clock" readings come from: std::chrono::steady_clock
# is CLOCK_MONOTONIC on Linux, which time.monotonic() reads, and
# QueryPerformanceCounter with MSVC, which time.perf_counter() reads on Windows
# (time.monotonic() there is GetTickCount64 until Python 3.13: another clock,
# and only 16 ms fine).
now = time.perf_counter if WINDOWS else time.monotonic

# What the daemon prints when there are new late buffers or dropped frames
# (main_daemon.cpp's tick), and when it quits.
SO_FAR_LINE = re.compile(r"audio: (\d+) late audio buffers, (\d+) frames dropped so far")
STOPPED_LINE = re.compile(r"stopped cleanly, (\d+) late audio buffers, (\d+) frames dropped")

# What am-ui --print-startup-time prints (main_client.cpp's StartupClock).
FIRST_FRAME = re.compile(
    r"am-ui: first frame ([\d.]+) ms after main\(\) \(steady clock ([\d.]+) s\)"
)
CONNECTED_FRAME = re.compile(
    r"am-ui: first frame with the daemon's state ([\d.]+) ms after main\(\) "
    r"\(steady clock ([\d.]+) s\)"
)

# The daemon prints "so far" lines at most once a second, so this long without
# a new one means nothing is still waiting to be printed.
QUIET_SECONDS = 1.5

# The position has to move at least this share of the wall-clock time.
MIN_PLAYED_SHARE = 0.9


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Open and close am-ui many times while the daemon plays.",
        epilog="Arguments after -- go to the daemon instead of --clear-film.",
    )
    parser.add_argument("--times", type=int, default=50, help="how many windows (default 50)")
    parser.add_argument(
        "--build",
        type=Path,
        default=REPO / "build" / ("windows-msvc-release" if WINDOWS else "linux-cef-release"),
        help="build folder holding scratterd, amctl and am-ui "
        "(default build/linux-cef-release, or build/windows-msvc-release on Windows)",
    )
    parser.add_argument(
        "--name",
        default=None,
        help="the daemon's --name (am-ui and amctl get it too), to run next to a daemon you use",
    )
    parser.add_argument(
        "--start-timeout",
        type=float,
        default=60.0,
        help="seconds to wait for the daemon to start playing (default 60; CEF takes a while)",
    )
    parser.add_argument(
        "--window-timeout",
        type=float,
        default=15.0,
        help="seconds to wait for each window to show the daemon's state (default 15)",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="folder for the results (default relaunch-results/<date-time>)",
    )
    # Splitting at -- by hand keeps argparse from reading the daemon's flags.
    argv = sys.argv[1:]
    daemon_args = ["--clear-film"]
    if "--" in argv:
        split = argv.index("--")
        argv, daemon_args = argv[:split], argv[split + 1 :]
    args = parser.parse_args(argv)
    if args.times < 1:
        parser.error("--times must be at least 1")
    args.daemon_args = daemon_args
    return args


def find_program(build: Path, name: str) -> Path:
    if WINDOWS:
        name += ".exe"
    # The program is at the top of the build folder or, for am-ui in some
    # build layouts, next to its sources in src/client/. A build folder can
    # hold a stale copy in the other place from an older layout, so take the
    # newest of the two.
    found = [p for p in [build / name, build / "src" / "client" / name] if p.is_file()]
    if found:
        return max(found, key=lambda p: p.stat().st_mtime)
    sys.exit(f"can't find {name} in {build}; build first, or point --build at the right folder")


def with_name(program: Path, name: str | None, *rest: str) -> list[str]:
    return [str(program)] + (["--name", name] if name else []) + list(rest)


def playback_status(amctl: Path, name: str | None) -> dict | None:
    """What `amctl status --json` says, or None if it couldn't tell."""
    try:
        done = subprocess.run(
            with_name(amctl, name, "status", "--json"), capture_output=True, text=True, timeout=10
        )
        return json.loads(done.stdout) if done.returncode == 0 else None
    except (subprocess.TimeoutExpired, json.JSONDecodeError):
        return None


def glitches_so_far(log_path: Path) -> tuple[int, int]:
    """The newest (late buffers, dropped frames) the daemon printed, or (0, 0)."""
    found = SO_FAR_LINE.findall(log_path.read_text(errors="replace"))
    return (int(found[-1][0]), int(found[-1][1])) if found else (0, 0)


def settled_glitches(log_path: Path) -> tuple[int, int]:
    """Waits until the daemon has had a quiet moment, then reads the numbers."""
    numbers = glitches_so_far(log_path)
    while True:
        time.sleep(QUIET_SECONDS)
        newer = glitches_so_far(log_path)
        if newer == numbers:
            return numbers
        numbers = newer


def wait_until_playing(daemon, amctl: Path, args) -> dict | None:
    """The first status that shows the daemon playing, or None after the timeout."""
    deadline = now() + args.start_timeout
    while now() < deadline and daemon.poll() is None:
        status = playback_status(amctl, args.name)
        if status and status.get("state") == "playing":
            return status
        time.sleep(0.5)
    return None


# ---- One window ----------------------------------------------------------------


def read_lines(pipe, lines: queue.Queue):
    """Hands each line am-ui prints to `lines`, then None when it closes its end.

    A thread of its own, because select() can't wait on a pipe on Windows.
    """
    for raw in iter(pipe.readline, b""):
        lines.put(raw.decode(errors="replace"))
    lines.put(None)


def post_close(pid: int) -> bool:
    """Windows: sends WM_CLOSE to the process's windows, like their close button.

    False if it has no window to send it to.
    """
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def each(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(each, 0)
    WM_CLOSE = 0x0010
    for hwnd in found:
        user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    return bool(found)


def close_window(ui: subprocess.Popen, clean: bool) -> str:
    """Closes am-ui cleanly or kills it. Returns a problem, or ""."""
    if not clean:
        ui.kill()  # SIGKILL on Linux, TerminateProcess on Windows
        return ""
    if not WINDOWS:
        ui.send_signal(signal.SIGTERM)
        return ""
    if post_close(ui.pid):
        return ""
    ui.kill()
    return "no window to send WM_CLOSE to"


def run_window(am_ui: Path, args, clean: bool, ui_log) -> dict:
    """Starts am-ui, waits for its first frame with the daemon's state, closes it.

    Returns one row of launches.csv. "problem" is empty when all went well.
    """
    row = {"how": "close" if clean else "kill -9", "problem": ""}
    launched = now()
    ui = subprocess.Popen(
        with_name(am_ui, args.name, "--print-startup-time"),
        stdout=subprocess.PIPE,
        stderr=ui_log,
    )
    lines: queue.Queue = queue.Queue()
    reader = threading.Thread(target=read_lines, args=(ui.stdout, lines), daemon=True)
    reader.start()
    deadline = launched + args.window_timeout
    connected = False
    while not connected:
        try:
            line = lines.get(timeout=max(0.0, deadline - now()))
        except queue.Empty:
            row["problem"] = "no frame with the daemon's state in time"
            break
        if line is None:
            row["problem"] = f"am-ui quit by itself (exit code {ui.wait()}); see ui.log"
            break
        if match := FIRST_FRAME.search(line):
            row["main_to_frame_ms"] = float(match[1])
            row["launch_to_frame_ms"] = round((float(match[2]) - launched) * 1000, 2)
        elif match := CONNECTED_FRAME.search(line):
            row["main_to_connected_ms"] = float(match[1])
            row["launch_to_connected_ms"] = round((float(match[2]) - launched) * 1000, 2)
            connected = True
        else:
            ui_log.write(line)

    if ui.poll() is None:
        if problem := close_window(ui, clean):
            row["problem"] = row["problem"] or problem
    try:
        ui.wait(timeout=5)
    except subprocess.TimeoutExpired:
        ui.kill()
        ui.wait()
        row["problem"] = row["problem"] or "still open 5 s after being asked to close"
    # The reader gets to the end of the pipe once am-ui is gone. Whatever it
    # read after the state line still goes to ui.log.
    reader.join(timeout=5)
    while not lines.empty():
        if (line := lines.get()) is not None:
            ui_log.write(line)
    ui.stdout.close()
    row["exit_code"] = ui.returncode
    if clean and ui.returncode != 0 and not row["problem"]:
        row["problem"] = f"exit code {ui.returncode} after a clean close; see ui.log"
    return row


# ---- Summary ---------------------------------------------------------------------


def spread(values: list[float]) -> str:
    if not values:
        return "no numbers"
    ordered = sorted(values)
    p90 = ordered[min(len(ordered) - 1, round(0.9 * (len(ordered) - 1)))]
    return (
        f"median {statistics.median(ordered):7.1f} ms, 90% {p90:7.1f} ms, "
        f"slowest {ordered[-1]:7.1f} ms"
    )


def summarize(rows, args, before, after, played, shutdown) -> tuple[str, bool]:
    lines = []
    passed = True

    def fail(text: str):
        nonlocal passed
        passed = False
        lines.append("FAIL: " + text)

    good = [r for r in rows if not r["problem"]]
    lines.append(f"{len(good)} of {args.times} windows opened, showed the daemon and closed")
    for i, r in enumerate(rows, 1):
        if r["problem"]:
            fail(f"window {i} ({r['how']}): {r['problem']}")

    lines.append("start-up, over the windows that showed the daemon:")
    for column, label in [
        ("launch_to_frame_ms", "launch to first frame"),
        ("main_to_frame_ms", "  of that, after main()"),
        ("launch_to_connected_ms", "launch to daemon's state"),
    ]:
        values = [r[column] for r in rows if column in r]
        lines.append(f"  {label:<26} {spread(values)}")

    late = after[0] - before[0]
    dropped = after[1] - before[1]
    lines.append(
        f"daemon's sound during the windows: {late} late audio buffers, {dropped} frames dropped "
        f"(before: {before[0]} late, {before[1]} dropped)"
    )
    if late or dropped:
        fail("the sound skipped while windows opened and closed")

    if played is None:
        lines.append("play position: not checked (no audio output)")
    else:
        moved, elapsed = played
        lines.append(f"play position moved {moved:.1f} s in {elapsed:.1f} s")
        if moved < MIN_PLAYED_SHARE * elapsed:
            fail("the play position fell behind the clock: the sound stopped or cut out")

    if shutdown["crashed"]:
        fail(f"the daemon quit by itself with exit code {shutdown['exit_code']}; see daemon.log")
    elif shutdown["late"] is None:
        fail(
            f"the daemon didn't print its 'stopped cleanly' line (exit code "
            f"{shutdown['exit_code']}); see daemon.log"
        )
    else:
        lines.append(
            f"daemon stopped cleanly ({shutdown['late']} late, {shutdown['dropped']} dropped "
            "over the whole run, its start and shutdown included)"
        )

    lines.append("PASSED" if passed else "FAILED")
    return "\n".join(lines) + "\n", passed


def stop_daemon(daemon: subprocess.Popen, log_path: Path) -> dict:
    """Stops the daemon like Ctrl-C would and reads its last words from the log."""
    crashed = daemon.poll() is not None
    if not crashed:
        # On Windows: Ctrl-Break to the daemon's own process group, which its
        # console handler (console_shutdown.cpp) treats like Ctrl-C.
        daemon.send_signal(signal.CTRL_BREAK_EVENT if WINDOWS else signal.SIGTERM)
        try:
            daemon.wait(timeout=30)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
    result = {"crashed": crashed, "exit_code": daemon.returncode, "late": None, "dropped": None}
    if match := STOPPED_LINE.search(log_path.read_text(errors="replace")):
        result["late"], result["dropped"] = int(match[1]), int(match[2])
    return result


# ---- The run -----------------------------------------------------------------------


def position_s(status: dict | None) -> float:
    return (status or {}).get("position_micros", 0) / 1e6


def main() -> int:
    args = parse_arguments()
    daemon_path = find_program(args.build, "scratterd")
    amctl = find_program(args.build, "amctl")
    am_ui = find_program(args.build, "am-ui")

    out = args.out or REPO / "relaunch-results" / datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    log_path = out / "daemon.log"

    command = with_name(daemon_path, args.name, *args.daemon_args)
    print(f"starting {' '.join(command)}")
    with open(log_path, "w") as log, open(out / "ui.log", "w") as ui_log:
        # A session (process group on Windows) of its own: Ctrl-C in this
        # terminal reaches only the script, and on Windows the script can send
        # the daemon a Ctrl-Break that reaches nobody else.
        if WINDOWS:
            group = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP}
        else:
            group = {"start_new_session": True}
        daemon = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, **group)
        rows = []
        before = after = (0, 0)
        played = None
        try:
            status = wait_until_playing(daemon, amctl, args)
            if status is None:
                print(f"the daemon didn't start playing. The end of {log_path}:")
                log.flush()
                print("".join(log_path.read_text(errors="replace").splitlines(True)[-10:]))
            else:
                before = settled_glitches(log_path)
                start_status = playback_status(amctl, args.name)
                started = now()
                print(f"playing; opening and closing the window {args.times} times")
                for i in range(1, args.times + 1):
                    row = run_window(am_ui, args, clean=i % 2 == 1, ui_log=ui_log)
                    rows.append(row)
                    took = row.get("launch_to_connected_ms")
                    took_text = f"{took:7.1f} ms" if took is not None else "      -   "
                    print(f"{i:3}  {row['how']:<8} {took_text}  {row['problem']}")
                    if daemon.poll() is not None:
                        print("the daemon quit")
                        break
                end_status = playback_status(amctl, args.name)
                elapsed = now() - started
                after = settled_glitches(log_path)
                # Only a daemon with sound output moves its play position. The
                # films go round to 0 at the end; add one length back for that.
                if start_status and end_status and end_status.get("sample_rate_hz"):
                    moved = position_s(end_status) - position_s(start_status)
                    length = end_status.get("duration_micros", 0) / 1e6
                    if moved < 0 and length > 0:
                        moved += length
                    played = (moved, elapsed)
        except KeyboardInterrupt:
            print("stopped early (Ctrl-C)")
        shutdown = stop_daemon(daemon, log_path)

    columns = [
        "how",
        "launch_to_frame_ms",
        "main_to_frame_ms",
        "launch_to_connected_ms",
        "main_to_connected_ms",
        "exit_code",
        "problem",
    ]
    with open(out / "launches.csv", "w", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)

    if not rows:
        summary, passed = "no windows were opened\nFAILED\n", False
    else:
        summary, passed = summarize(rows, args, before, after, played, shutdown)
    (out / "summary.txt").write_text(summary)
    print()
    print(summary, end="")
    print(f"(saved in {out}/summary.txt)")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
