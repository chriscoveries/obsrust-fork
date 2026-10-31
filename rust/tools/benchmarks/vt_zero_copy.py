#!/usr/bin/env -S uv run --script
"""Record the VideoToolbox zero-copy timings of #755 with test/perf/zero-copy-harness.

Usage: uv run rust/tools/benchmarks/vt_zero_copy.py --harness <build>/test/perf/RelWithDebInfo/zero-copy-harness \\
           --metal-dir <build>/libobs-metal/RelWithDebInfo --output vt-zero-copy.json

Each round runs the four routes once, in a rotated order: the IOSurface
texture route; the forced raw fallback (GPU conversion into the prototype's
NV12 textures, the encoder takes raw frames); the planar raw route (the same
with the renderer's NV12 textures off, which is what master's Metal renderer
and VideoToolbox encoder do today); and the legacy raw input (GPU conversion
off). Before every run it waits until the 1-minute load is
below --max-load and no compiler is running, and it records the load before
and after the run. It never stops another process.

The report has record.py's shape (#142, #668): run metadata, then one row per
metric with each route's median, run-to-run range and per-run values, and the
texture/raw ratio of the medians and of each round's pair. CI does no timing
(testing-policy.md); this is a local recorder for one named machine.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
ROUTES = {
    "texture": [],
    "raw_fallback": ["--raw-fallback"],
    "raw_planar": ["--raw-planar"],
    "raw_no_gpu": ["--raw-no-gpu"],
}
METRICS = [
    "cpu_us_per_frame",
    "cpu_percent",
    "entry_to_output_p50_us",
    "entry_to_output_p95_us",
    "submit_to_output_p50_us",
    "submit_to_output_p95_us",
    # Frame health over the whole run: frames the render thread missed, and
    # frames the encoder was given twice (texture) or that video-io dropped
    # (raw). A route that repeats frames cannot be compared per frame.
    "render_lagged",
    "video_skipped",
]
BUILD_PROCESSES = ["clang", "clang++", "swift-frontend", "rustc", "ld"]
SOURCES = [
    "plugins/mac-videotoolbox",
    "libobs/obs-video-gpu-encode.c.h",
    "libobs/obs-video.c.h",
    "libobs/obs-texture-lease.c.h",
    "libobs-metal",
    "test/perf",
]


def source_hash() -> str:
    digest = hashlib.sha256()
    paths = []
    for entry in SOURCES:
        path = ROOT / entry
        paths.extend(sorted(p for p in path.rglob("*") if p.is_file()) if path.is_dir() else [path])
    for path in paths:
        digest.update(str(path.relative_to(ROOT)).encode() + b"\0")
        digest.update(path.read_bytes())
    return digest.hexdigest()


def text(command: list[str]) -> str:
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=False)
    return (result.stdout + result.stderr).strip()


def gpu_name() -> str:
    try:
        data = json.loads(text(["system_profiler", "-json", "SPDisplaysDataType"]))
        cards = data.get("SPDisplaysDataType", [])
        return ", ".join(f"{card.get('sppci_model', '?')} ({card.get('sppci_cores', '?')} cores)" for card in cards)
    except (json.JSONDecodeError, OSError):
        return ""


def builds_running() -> list[str]:
    return [name for name in BUILD_PROCESSES if subprocess.run(["pgrep", "-x", name], capture_output=True).returncode == 0]


def wait_quiet(max_load: float, poll: float, log) -> dict:
    """Block until the machine is quiet; return what was seen when it was."""
    waited = 0.0
    while True:
        load = os.getloadavg()
        running = builds_running()
        if load[0] < max_load and not running:
            return {"load_1m": load[0], "load_5m": load[1], "waited_s": round(waited, 1)}
        log(f"waiting: load {load[0]:.2f} {load[1]:.2f} {load[2]:.2f}, building: {running or 'none'}")
        time.sleep(poll)
        waited += poll


def parse_harness(output: str) -> dict:
    lines = [line for line in output.splitlines() if line.startswith("timing ")]
    if len(lines) != 1:
        raise ValueError(f"expected one timing line, got {len(lines)}:\n{output}")
    fields = dict(re.findall(r"(\w+)=(\S+)", lines[0]))
    first = next(line for line in output.splitlines() if line.startswith("resolution="))
    fields.update({f"run_{key}": value for key, value in re.findall(r"(\w+)=(\S+)", first)})
    return fields


def run_once(args, route: str, log) -> dict:
    quiet = wait_quiet(args.max_load, args.poll, log)
    env = dict(os.environ)
    env["DYLD_LIBRARY_PATH"] = str(args.metal_dir)
    command = [str(args.harness), str(args.frames), f"--warmup={args.warmup}", *ROUTES[route]]
    started = time.time()
    result = subprocess.run(command, env=env, text=True, capture_output=True, check=False)
    after = os.getloadavg()
    record = {
        "route": route,
        "command": [os.path.relpath(command[0], ROOT), *command[1:]],
        "returncode": result.returncode,
        "wall_s": round(time.time() - started, 3),
        "load_before": quiet,
        "load_after": {"load_1m": after[0], "load_5m": after[1]},
        "builds_after": builds_running(),
    }
    if result.returncode != 0:
        record["stderr"] = result.stderr[-4000:]
        record["stdout"] = result.stdout[-4000:]
        return record
    fields = parse_harness(result.stdout)
    record["metrics"] = {name: float(fields[name]) for name in METRICS}
    record["raw"] = fields
    return record


def summarize(runs: list[dict]) -> list[dict]:
    rows = []
    good = [run for run in runs if run["returncode"] == 0]
    rounds = sorted({run["round"] for run in good})
    for metric in METRICS:
        row = {"benchmark": metric}
        for route in ROUTES:
            points = [run["metrics"][metric] for run in good if run["route"] == route]
            row[route] = {
                "median": statistics.median(points) if points else None,
                "range": [min(points), max(points)] if points else None,
                "runs": points,
            }
        for raw in ["raw_fallback", "raw_planar", "raw_no_gpu"]:
            if row["texture"]["median"] and row[raw]["median"]:
                row[f"texture_over_{raw}"] = row["texture"]["median"] / row[raw]["median"]
                paired = []
                for index in rounds:
                    pair = {run["route"]: run["metrics"][metric] for run in good if run["round"] == index}
                    if "texture" in pair and pair.get(raw):
                        paired.append(pair["texture"] / pair[raw])
                row[f"paired_round_ratio_range_{raw}"] = [min(paired), max(paired)] if paired else None
        rows.append(row)
    return rows


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--harness", required=True, type=Path)
    parser.add_argument("--metal-dir", required=True, type=Path, help="directory holding libobs-metal.dylib")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--repeats", type=int, default=5, help="rounds; each runs every route once")
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--warmup", type=int, default=120)
    parser.add_argument("--max-load", type=float, default=3.0)
    parser.add_argument("--poll", type=float, default=30.0)
    args = parser.parse_args()
    if args.repeats < 5:
        parser.error("#755 asks for at least five runs per route")
    fingerprint = source_hash()

    def log(message: str) -> None:
        stamp = datetime.datetime.now().strftime("%H:%M:%S")
        print(f"[{stamp}] {message}", flush=True)

    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    order = list(ROUTES)
    runs = []
    for index in range(args.repeats):
        rotated = order[index % len(order):] + order[: index % len(order)]
        for route in rotated:
            log(f"round {index + 1}/{args.repeats}: {route}")
            record = run_once(args, route, log)
            record["round"] = index + 1
            runs.append(record)
            if record["returncode"] != 0:
                log(f"{route} failed ({record['returncode']}): {record.get('stderr', '')[-400:]}")
            else:
                metrics = record["metrics"]
                log(f"{route}: {metrics['cpu_us_per_frame']:.1f} us CPU/frame, "
                    f"entry->output p50 {metrics['entry_to_output_p50_us']:.0f} us, "
                    f"load {record['load_before']['load_1m']:.2f} -> {record['load_after']['load_1m']:.2f}")
        if source_hash() != fingerprint:
            raise ValueError("source changed during measurement; discard this run")

    report = {
        "schema": 1,
        "timestamp_utc": timestamp,
        "source_sha256": fingerprint,
        "base_commit": text(["git", "rev-parse", "HEAD"]),
        "cpu": text(["sysctl", "-n", "machdep.cpu.brand_string"]),
        "hw_model": text(["sysctl", "-n", "hw.model"]),
        "gpu": gpu_name(),
        "renderer": "Metal (libobs-metal.dylib)",
        "os": {
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
            "macos": text(["sw_vers", "-productVersion"]),
            "build": text(["sw_vers", "-buildVersion"]),
        },
        "workload": {
            "resolution": "1920x1080",
            "format": "NV12",
            "fps": 60,
            "source": "color_source",
            "encoder": "VideoToolbox H.264, bitrate 2500",
            "output": "null_output with CoreAudio AAC",
            "frames": args.frames,
            "warmup": args.warmup,
        },
        "quiet_rule": {"max_load_1m": args.max_load, "no_running": BUILD_PROCESSES},
        "command": [str(args.harness.name), str(args.frames), f"--warmup={args.warmup}"],
        "routes": {route: flags for route, flags in ROUTES.items()},
        "repeats": args.repeats,
        "results": summarize(runs),
        "runs": runs,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    failed = [run for run in runs if run["returncode"] != 0]
    log(f"Recorded {len(runs) - len(failed)} of {len(runs)} runs: {args.output}")
    if failed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
