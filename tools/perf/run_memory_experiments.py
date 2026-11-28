#!/usr/bin/env python3
"""Run CEF memory experiments one at a time against linux-cef-release and report results.

Run as your normal user (NOT sudo!) from the repo root so scratterd talks to
your PipeWire session and browser profile. The script calls `sudo` only for
`tools/perf/footprint.py`, which needs root to read /proc/PID/smaps_rollup for
Chromium's sandboxed subprocesses.

    python3 tools/perf/run_memory_experiments.py
    python3 tools/perf/run_memory_experiments.py --warmup 30 --measure 30
    python3 tools/perf/run_memory_experiments.py --include-network-in-process
    python3 tools/perf/run_memory_experiments.py --dry-run

Requires an official AAC-enabled CEF build in third_party/cef/linux64 and an
installed Widevine CDM (see docs/CEF.md). Always restores any edited files on
exit or Ctrl-C.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime
import difflib
import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CEF_HOST_PATH = REPO_ROOT / "src/daemon/cef_host/cef_host.cpp"
TEST_FILM_PAGE_PATH = REPO_ROOT / "src/daemon/cef_host/test_film_page.hpp"
FOOTPRINT_SCRIPT = REPO_ROOT / "tools/perf/footprint.py"
BUILD_DIR = REPO_ROOT / "build/linux-cef-release"
DAEMON_NAME = "scratter-mem-bench"

DISABLE_FEATURES_ANCHOR = (
    '        command_line->AppendSwitchWithValue("disable-features",\n'
    '                                            "AudioServiceOutOfProcess,PartitionAllocDanglingPtr");'
)
PASSWORD_STORE_ANCHOR = '        command_line->AppendSwitchWithValue("password-store", "basic");'
SHAKA_CONFIGURE_ANCHOR = (
    "  player.configure({\n"
    "    manifest: { disableVideo: true, disableText: true },\n"
    "    preferredAudioCodecs: ['mp4a.40.2'],\n"
    "    drm: drmFor(clear),\n"
    "  });"
)


@dataclasses.dataclass(frozen=True)
class Experiment:
    key: str
    label: str
    description: str
    disable_features_extra: tuple[str, ...] = ()
    enable_features_extra: tuple[str, ...] = ()
    cef_switches: tuple[tuple[str, str], ...] = ()
    shaka_streaming: str | None = None
    v8_group: bool = False
    informational_only: bool = False


EXPERIMENTS: list[Experiment] = [
    Experiment(
        key="0_baseline",
        label="Step 0: Baseline",
        description="Clean main (no changes)",
    ),
    Experiment(
        key="1_v8_conservative",
        label="Exp 1: V8 conservative",
        description="--js-flags=--optimize-for-size --max-old-space-size=48",
        cef_switches=(("js-flags", "--optimize-for-size --max-old-space-size=48"),),
        v8_group=True,
    ),
    Experiment(
        key="2_v8_jitless",
        label="Exp 2: V8 jitless",
        description="--js-flags=--jitless --optimize-for-size --max-old-space-size=32",
        cef_switches=(("js-flags", "--jitless --optimize-for-size --max-old-space-size=32"),),
        v8_group=True,
    ),
    Experiment(
        key="3_storage_in_process",
        label="Exp 3: Storage in-process",
        description="Add StorageServiceOutOfProcess to --disable-features",
        disable_features_extra=("StorageServiceOutOfProcess",),
    ),
    Experiment(
        key="4_cache_and_shaka_caps",
        label="Exp 4: Cache + Shaka caps",
        description="1 MB disk/media caches + Shaka bufferingGoal=10 / bufferBehind=5",
        cef_switches=(
            ("disk-cache-size", "1048576"),
            ("media-cache-size", "1048576"),
        ),
        shaka_streaming="    streaming: { bufferingGoal: 10, bufferBehind: 5 },",
    ),
]

NETWORK_IN_PROCESS_EXP = Experiment(
    key="5_network_in_process_info",
    label="Exp 5 (info): Network in-process",
    description="--enable-features=NetworkServiceInProcess2 (drops network sandbox; info only)",
    enable_features_extra=("NetworkServiceInProcess2",),
    informational_only=True,
)


@dataclasses.dataclass
class FootprintTotals:
    processes: int | None = None
    rss_mb: float | None = None
    pss_mb: float | None = None
    pss_partial: bool = False
    own_mb: float | None = None
    cpu_pct: float | None = None
    wakeups_per_s: float | None = None
    vram_ok: bool | None = None
    raw_output: str = ""


@dataclasses.dataclass
class RunResult:
    experiment: Experiment
    playing: FootprintTotals | None = None
    paused: FootprintTotals | None = None
    seek_ok: bool = False
    seek_detail: str = ""
    exit_code: int | None = None
    late_buffers: int | None = None
    dropped_frames: int | None = None
    clean_shutdown: bool = False
    anomalies: list[str] = dataclasses.field(default_factory=list)
    error: str | None = None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Measure CEF memory experiments one at a time on linux-cef-release."
    )
    parser.add_argument(
        "--warmup",
        type=float,
        default=60.0,
        help="seconds to let --test-film play before measuring (default: 60)",
    )
    parser.add_argument(
        "--measure",
        type=float,
        default=30.0,
        help="seconds for each footprint.py sample (default: 30)",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=REPO_ROOT / "memory-experiment-results",
        help="folder where logs and report.md are written",
    )
    parser.add_argument(
        "--include-network-in-process",
        action="store_true",
        help="also measure NetworkServiceInProcess2 as an informational data point",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="verify every experiment patch applies cleanly and print unified diffs, then exit",
    )
    return parser.parse_args()


def render_patched_files(
    orig_cef_host: str, orig_test_film: str, exp: Experiment
) -> tuple[str, str]:
    cef = orig_cef_host
    film = orig_test_film

    if DISABLE_FEATURES_ANCHOR not in cef:
        raise RuntimeError(f"Could not find --disable-features anchor in {CEF_HOST_PATH}")
    if PASSWORD_STORE_ANCHOR not in cef:
        raise RuntimeError(f"Could not find --password-store anchor in {CEF_HOST_PATH}")
    if SHAKA_CONFIGURE_ANCHOR not in film:
        raise RuntimeError(f"Could not find player.configure anchor in {TEST_FILM_PAGE_PATH}")

    if exp.disable_features_extra:
        features = ",".join(
            ["AudioServiceOutOfProcess", "PartitionAllocDanglingPtr", *exp.disable_features_extra]
        )
        replacement = (
            '        command_line->AppendSwitchWithValue("disable-features",\n'
            f'                                            "{features}");'
        )
        cef = cef.replace(DISABLE_FEATURES_ANCHOR, replacement, 1)

    extra_lines: list[str] = []
    if exp.enable_features_extra:
        enabled = ",".join(exp.enable_features_extra)
        extra_lines.append(
            f'        command_line->AppendSwitchWithValue("enable-features", "{enabled}");'
        )
    for switch, value in exp.cef_switches:
        extra_lines.append(
            f'        command_line->AppendSwitchWithValue("{switch}", "{value}");'
        )
    if extra_lines:
        cef = cef.replace(
            PASSWORD_STORE_ANCHOR,
            PASSWORD_STORE_ANCHOR + "\n" + "\n".join(extra_lines),
            1,
        )

    if exp.shaka_streaming is not None:
        patched_shaka = (
            "  player.configure({\n"
            "    manifest: { disableVideo: true, disableText: true },\n"
            "    preferredAudioCodecs: ['mp4a.40.2'],\n"
            "    drm: drmFor(clear),\n"
            f"{exp.shaka_streaming}\n"
            "  });"
        )
        film = film.replace(SHAKA_CONFIGURE_ANCHOR, patched_shaka, 1)

    return cef, film


def parse_footprint_output(text: str) -> FootprintTotals:
    totals = FootprintTotals(raw_output=text)
    if match := re.search(r"watching \S+ \(pid \d+\) and (\d+) processes", text):
        totals.processes = int(match.group(1)) + 1
    if match := re.search(
        r"^total\s+(\d+(?:\.\d+)?)\s+(\d+(?:\.\d+)?)(\+?)\s+(\d+(?:\.\d+)?)\s+"
        r"(\d+(?:\.\d+)?)\s+(\d+(?:\.\d+)?)",
        text,
        re.MULTILINE,
    ):
        totals.rss_mb = float(match.group(1))
        totals.pss_mb = float(match.group(2))
        totals.pss_partial = match.group(3) == "+"
        totals.own_mb = float(match.group(4))
        totals.cpu_pct = float(match.group(5))
        totals.wakeups_per_s = float(match.group(6))
    if "GPU: no process has the graphics card open" in text:
        totals.vram_ok = True
    elif "GPU: open in" in text:
        totals.vram_ok = "0.0 MB VRAM" in text or "VRAM" not in text
    return totals


def ensure_clean_targets() -> None:
    status = subprocess.run(
        [
            "git",
            "status",
            "--porcelain",
            str(CEF_HOST_PATH.relative_to(REPO_ROOT)),
            str(TEST_FILM_PAGE_PATH.relative_to(REPO_ROOT)),
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    if status.stdout.strip():
        sys.exit(
            "Refusing to run with uncommitted edits in cef_host.cpp or test_film_page.hpp:\n"
            + status.stdout
        )


def ensure_configured() -> None:
    if not (REPO_ROOT / "third_party/cef/linux64").exists():
        sys.exit(
            "third_party/cef/linux64 not found. Build/link CEF with AAC first (see docs/CEF.md)."
        )
    if not (BUILD_DIR / "build.ninja").exists():
        print("Configuring linux-cef-release...", flush=True)
        subprocess.run(
            ["cmake", "--preset", "linux-cef-release"],
            cwd=REPO_ROOT,
            check=True,
        )


def build_daemon() -> None:
    subprocess.run(
        ["cmake", "--build", "--preset", "linux-cef-release", "--target", "scratterd", "amctl"],
        cwd=REPO_ROOT,
        check=True,
    )


def run_footprint(pid: int, seconds: float) -> FootprintTotals:
    subprocess.run(["sudo", "-v"], check=True)
    proc = subprocess.run(
        [
            "sudo",
            sys.executable,
            str(FOOTPRINT_SCRIPT),
            "--pid",
            str(pid),
            "--seconds",
            f"{seconds:.1f}",
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return parse_footprint_output(proc.stdout)


def check_seek(amctl: Path) -> tuple[bool, str]:
    seek = subprocess.run(
        [str(amctl), "--name", DAEMON_NAME, "seek", "1:00"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        timeout=15,
    )
    if seek.returncode != 0:
        return False, f"amctl seek failed ({seek.returncode}): {seek.stderr.strip()}"
    time.sleep(3.0)
    status = subprocess.run(
        [str(amctl), "--name", DAEMON_NAME, "status", "--json"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        timeout=15,
    )
    if status.returncode != 0:
        return False, f"amctl status failed ({status.returncode}): {status.stderr.strip()}"
    try:
        data = json.loads(status.stdout)
    except json.JSONDecodeError as exc:
        return False, f"bad status JSON: {exc}"
    pos_s = data.get("position_micros", 0) / 1_000_000.0
    state = data.get("state", "?")
    ok = state == "playing" and 55.0 <= pos_s <= 75.0
    return ok, f"state={state}, pos={pos_s:.1f}s"


def run_one_experiment(
    exp: Experiment,
    orig_cef: str,
    orig_film: str,
    run_dir: Path,
    warmup_s: float,
    measure_s: float,
) -> RunResult:
    result = RunResult(experiment=exp)
    print(f"\n=== {exp.label} ===", flush=True)
    print(f"    {exp.description}", flush=True)

    new_cef, new_film = render_patched_files(orig_cef, orig_film, exp)
    CEF_HOST_PATH.write_text(new_cef, encoding="utf-8")
    TEST_FILM_PAGE_PATH.write_text(new_film, encoding="utf-8")

    try:
        print("  Building scratterd + amctl...", flush=True)
        build_daemon()
    except subprocess.CalledProcessError as exc:
        result.error = f"build failed (exit {exc.returncode})"
        return result

    daemon_bin = BUILD_DIR / "scratterd"
    amctl_bin = BUILD_DIR / "amctl"
    stderr_log = run_dir / f"{exp.key}_daemon.log"

    daemon: subprocess.Popen[bytes] | None = None
    try:
        with stderr_log.open("wb") as log_file:
            daemon = subprocess.Popen(
                [str(daemon_bin), "--name", DAEMON_NAME, "--test-film"],
                cwd=REPO_ROOT,
                stdout=subprocess.DEVNULL,
                stderr=log_file,
            )

            print("  Waiting for Widevine test film playback...", flush=True)
            deadline = time.monotonic() + 45.0
            playing_seen = False
            while time.monotonic() < deadline:
                if daemon.poll() is not None:
                    result.error = f"scratterd exited early with code {daemon.returncode}"
                    return result
                text = stderr_log.read_text(encoding="utf-8", errors="replace")
                if "page: playing the Widevine test film" in text:
                    playing_seen = True
                    break
                time.sleep(0.5)

            if not playing_seen:
                result.error = "timed out waiting for 'page: playing the Widevine test film'"
                return result

            print(f"  Playing! Warming up for {warmup_s:.0f} s...", flush=True)
            end_warmup = time.monotonic() + warmup_s
            while time.monotonic() < end_warmup:
                if daemon.poll() is not None:
                    result.error = f"scratterd exited during warmup with code {daemon.returncode}"
                    return result
                time.sleep(1.0)

            print(f"  Measuring PLAYING ({measure_s:.0f} s)...", flush=True)
            result.playing = run_footprint(daemon.pid, measure_s)
            (run_dir / f"{exp.key}_playing.txt").write_text(
                result.playing.raw_output, encoding="utf-8"
            )
            print(
                f"    -> {result.playing.processes} procs, "
                f"PSS {fmt_mb(result.playing.pss_mb)} MB, "
                f"own {fmt_mb(result.playing.own_mb)} MB, "
                f"CPU {fmt_pct(result.playing.cpu_pct)}",
                flush=True,
            )

            print("  Checking seek 1:00...", flush=True)
            result.seek_ok, result.seek_detail = check_seek(amctl_bin)
            if not result.seek_ok:
                result.anomalies.append(f"seek check failed ({result.seek_detail})")

            print("  Pausing...", flush=True)
            pause = subprocess.run(
                [str(amctl_bin), "--name", DAEMON_NAME, "pause"],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
                timeout=15,
            )
            if pause.returncode != 0:
                result.anomalies.append(f"amctl pause failed: {pause.stderr.strip()}")
            time.sleep(5.0)

            print(f"  Measuring PAUSED ({measure_s:.0f} s)...", flush=True)
            result.paused = run_footprint(daemon.pid, measure_s)
            (run_dir / f"{exp.key}_paused.txt").write_text(
                result.paused.raw_output, encoding="utf-8"
            )
            print(
                f"    -> PSS {fmt_mb(result.paused.pss_mb)} MB, "
                f"CPU {fmt_pct(result.paused.cpu_pct)}, "
                f"wakeups {fmt_num(result.paused.wakeups_per_s)}/s",
                flush=True,
            )

            print("  Sending SIGINT (Ctrl-C)...", flush=True)
            daemon.send_signal(signal.SIGINT)
            try:
                result.exit_code = daemon.wait(timeout=15.0)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait()
                result.error = "scratterd hung on SIGINT (killed after 15 s)"
                return result
            daemon = None
    finally:
        if daemon is not None and daemon.poll() is None:
            daemon.send_signal(signal.SIGINT)
            try:
                daemon.wait(timeout=10.0)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait()

    log_text = stderr_log.read_text(encoding="utf-8", errors="replace")
    if match := re.search(
        r"stopped cleanly, (\d+) late audio buffers, (\d+) frames dropped", log_text
    ):
        result.late_buffers = int(match.group(1))
        result.dropped_frames = int(match.group(2))
        result.clean_shutdown = result.exit_code == 0
    else:
        result.anomalies.append("missing 'stopped cleanly' summary line")

    for pattern, note in [
        (r"FATAL", "FATAL line in daemon stderr"),
        (r"stack smashing", "stack smashing detected"),
        (r"Shaka error", "Shaka error logged"),
        (r"the page's renderer", "renderer process terminated"),
        (r"reloading the film", "page reloaded the film"),
    ]:
        if re.search(pattern, log_text):
            result.anomalies.append(note)

    return result


def pick_combined_winners(baseline: RunResult, results: list[RunResult]) -> Experiment | None:
    if baseline.playing is None or baseline.playing.pss_mb is None or baseline.playing.own_mb is None:
        return None
    base_pss = baseline.playing.pss_mb
    base_own = baseline.playing.own_mb
    base_cpu = baseline.playing.cpu_pct or 0.0

    def is_clean_winner(r: RunResult) -> bool:
        if r.experiment.informational_only or r.experiment.key == "0_baseline":
            return False
        if r.error or not r.clean_shutdown or not r.seek_ok or r.anomalies:
            return False
        if (r.late_buffers or 0) > 0 or (r.dropped_frames or 0) > 0:
            return False
        if r.playing is None or r.playing.pss_mb is None or r.playing.own_mb is None:
            return False
        saved_pss = base_pss - r.playing.pss_mb
        saved_own = base_own - r.playing.own_mb
        cpu_delta = (r.playing.cpu_pct or 0.0) - base_cpu
        return (saved_pss >= 10.0 or saved_own >= 10.0) and cpu_delta <= 1.5

    v8_candidates = [r for r in results if r.experiment.v8_group and is_clean_winner(r)]
    other_winners = [r for r in results if not r.experiment.v8_group and is_clean_winner(r)]

    chosen: list[Experiment] = []
    if v8_candidates:
        best_v8 = min(v8_candidates, key=lambda r: r.playing.pss_mb or 1e9)  # type: ignore[union-attr]
        chosen.append(best_v8.experiment)
    chosen.extend(r.experiment for r in other_winners)

    if len(chosen) < 2:
        return None

    disable_extra: list[str] = []
    switches: list[tuple[str, str]] = []
    shaka_streaming: str | None = None
    names: list[str] = []
    for exp in chosen:
        names.append(exp.key)
        disable_extra.extend(exp.disable_features_extra)
        switches.extend(exp.cef_switches)
        if exp.shaka_streaming is not None:
            shaka_streaming = exp.shaka_streaming

    return Experiment(
        key="6_combined_winners",
        label="Exp 6: Combined winners",
        description="Combined safe winners: " + ", ".join(names),
        disable_features_extra=tuple(disable_extra),
        cef_switches=tuple(switches),
        shaka_streaming=shaka_streaming,
    )


def fmt_mb(v: float | None, partial: bool = False) -> str:
    if v is None:
        return "?"
    return f"{v:.1f}" + ("+" if partial else "")


def fmt_delta(v: float | None, base: float | None) -> str:
    if v is None or base is None:
        return ""
    d = v - base
    return f" ({d:+.1f})"


def fmt_pct(v: float | None) -> str:
    return "?" if v is None else f"{v:.1f}%"


def fmt_num(v: float | None) -> str:
    return "?" if v is None else f"{v:.0f}"


def build_report(results: list[RunResult], run_dir: Path) -> str:
    base = results[0]
    base_pss = base.playing.pss_mb if base.playing else None
    base_own = base.playing.own_mb if base.playing else None

    lines = [
        "# Scratter CEF Memory Experiment Report",
        "",
        f"- Date: {datetime.datetime.now().isoformat(timespec='seconds')}",
        f"- Logs folder: `{run_dir}`",
        "",
        "## Summary",
        "",
        "| Run | Procs | PSS playing (MB) | Own playing (MB) | CPU play | PSS paused (MB) | CPU pause | Wakeups pause | Late/drop | Ctrl-C | Notes |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- | --- |",
    ]
    for r in results:
        p = r.playing
        q = r.paused
        is_base = r.experiment.key == "0_baseline"
        pss_play = (
            fmt_mb(p.pss_mb, p.pss_partial)
            + ("" if is_base else fmt_delta(p.pss_mb, base_pss))
            if p
            else "—"
        )
        own_play = (
            fmt_mb(p.own_mb) + ("" if is_base else fmt_delta(p.own_mb, base_own))
            if p
            else "—"
        )
        notes_parts: list[str] = []
        if r.error:
            notes_parts.append(f"ERROR: {r.error}")
        if r.anomalies:
            notes_parts.append("; ".join(r.anomalies))
        if not notes_parts and r.seek_detail:
            notes_parts.append(f"seek ok ({r.seek_detail})")
        late_drop = (
            f"{r.late_buffers}/{r.dropped_frames}"
            if r.late_buffers is not None and r.dropped_frames is not None
            else "—"
        )
        ctrl_c = "ok" if r.clean_shutdown else (f"exit {r.exit_code}" if r.exit_code is not None else "fail")
        lines.append(
            f"| {r.experiment.label} "
            f"| {p.processes if p and p.processes is not None else '—'} "
            f"| {pss_play} "
            f"| {own_play} "
            f"| {fmt_pct(p.cpu_pct) if p else '—'} "
            f"| {fmt_mb(q.pss_mb, q.pss_partial) if q else '—'} "
            f"| {fmt_pct(q.cpu_pct) if q else '—'} "
            f"| {fmt_num(q.wakeups_per_s) if q else '—'} "
            f"| {late_drop} "
            f"| {ctrl_c} "
            f"| {'; '.join(notes_parts)} |"
        )

    lines.append("")
    lines.append("## Per-process breakdowns (playing)")
    for r in results:
        if r.playing and r.playing.raw_output:
            if (
                r.experiment.key in ("0_baseline", "6_combined_winners", "3_storage_in_process")
                or (
                    base.playing
                    and r.playing.processes != base.playing.processes
                )
            ):
                lines.append("")
                lines.append(f"### {r.experiment.label}")
                lines.append("```text")
                lines.append(r.playing.raw_output.rstrip())
                lines.append("```")

    return "\n".join(lines) + "\n"


def main() -> None:
    args = parse_args()

    if not args.dry_run and os.geteuid() == 0:
        sys.exit(
            "Run this script as your normal user, not with sudo "
            "(scratterd needs your PipeWire session; the script uses sudo only for footprint.py)."
        )

    orig_cef = CEF_HOST_PATH.read_text(encoding="utf-8")
    orig_film = TEST_FILM_PAGE_PATH.read_text(encoding="utf-8")

    plan = list(EXPERIMENTS)
    if args.include_network_in_process:
        plan.append(NETWORK_IN_PROCESS_EXP)

    if args.dry_run:
        for exp in plan:
            new_cef, new_film = render_patched_files(orig_cef, orig_film, exp)
            print(f"=== {exp.label} ({exp.description}) ===")
            diff_cef = "".join(
                difflib.unified_diff(
                    orig_cef.splitlines(keepends=True),
                    new_cef.splitlines(keepends=True),
                    fromfile="src/daemon/cef_host/cef_host.cpp",
                    tofile="src/daemon/cef_host/cef_host.cpp",
                    n=2,
                )
            )
            diff_film = "".join(
                difflib.unified_diff(
                    orig_film.splitlines(keepends=True),
                    new_film.splitlines(keepends=True),
                    fromfile="src/daemon/cef_host/test_film_page.hpp",
                    tofile="src/daemon/cef_host/test_film_page.hpp",
                    n=2,
                )
            )
            print(diff_cef or "(no cef_host.cpp diff)")
            if diff_film:
                print(diff_film)
        print("Dry run OK: every patch matched cleanly.")
        return

    ensure_clean_targets()
    ensure_configured()

    print("Caching sudo credentials for tools/perf/footprint.py...", flush=True)
    subprocess.run(["sudo", "-v"], check=True)

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir = args.out / stamp
    run_dir.mkdir(parents=True, exist_ok=True)

    results: list[RunResult] = []
    try:
        for exp in plan:
            results.append(
                run_one_experiment(exp, orig_cef, orig_film, run_dir, args.warmup, args.measure)
            )

        if results:
            combined = pick_combined_winners(results[0], results)
            if combined is not None:
                results.append(
                    run_one_experiment(
                        combined, orig_cef, orig_film, run_dir, args.warmup, args.measure
                    )
                )
            else:
                print(
                    "\nFewer than 2 standalone safe winners (>=10 MB saved, clean); "
                    "skipping combined run.",
                    flush=True,
                )
    finally:
        CEF_HOST_PATH.write_text(orig_cef, encoding="utf-8")
        TEST_FILM_PAGE_PATH.write_text(orig_film, encoding="utf-8")
        print("\nRestored cef_host.cpp and test_film_page.hpp to clean main.", flush=True)

    report = build_report(results, run_dir)
    report_path = run_dir / "report.md"
    report_path.write_text(report, encoding="utf-8")
    print("\n" + report)
    print(f"Saved full report to {report_path}")


if __name__ == "__main__":
    main()
