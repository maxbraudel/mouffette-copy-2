#!/usr/bin/env python3
"""Test-only fixture generation and isolated legacy/shared comparisons.

The application never launches ffmpeg; this developer harness does.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--binary", type=Path, required=True)
parser.add_argument("--source", type=Path, required=True, help="At least 26 seconds of 1080p30 MP4")
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--generate", action="store_true")
parser.add_argument("--shared-only", action="store_true")
parser.add_argument("--fixtures-only", action="store_true")
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
fixtures = args.output / "fixtures"
fixtures.mkdir(exist_ok=True)

def ffmpeg(name, options):
    with (args.output / (name + "-fixture.log")).open("w") as log:
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *options,
                        str(fixtures / (name + ".mp4"))], stdout=log, stderr=log, check=True)

if args.generate:
    for index in range(10):
        ffmpeg("1080-" + str(index), ["-ss", str(index * 2), "-i", str(args.source),
               "-t", "6", "-c", "copy", "-avoid_negative_ts", "make_zero"])
    ffmpeg("4k", ["-f", "lavfi", "-i", "testsrc2=size=3840x2160:rate=30", "-t", "2",
                   "-c:v", "libx264", "-preset", "veryfast", "-crf", "18", "-threads", "2"])
    ffmpeg("hdr", ["-f", "lavfi", "-i", "testsrc2=size=256x144:rate=24", "-t", "1",
        "-c:v", "libx265", "-pix_fmt", "yuv420p10le", "-color_primaries", "bt2020",
        "-color_trc", "smpte2084", "-colorspace", "bt2020nc", "-x265-params",
        "pools=1:frame-threads=1:colorprim=9:transfer=16:colormatrix=9"])
    # MOV deliberately exercises lower-level engine alpha; import remains MP4-only.
    ffmpeg("alpha", ["-f", "lavfi", "-i", "color=c=red@0.5:s=128x96:r=10,format=argb",
                     "-t", "1", "-c:v", "qtrle", "-f", "mov"])

if args.fixtures_only:
    raise SystemExit(0)

scenarios = {"one": (1, 1), "same-ten": (1, 10), "distinct-four": (4, 1), "distinct-ten": (10, 1)}
results = {"platform": platform.platform(), "fixtureSource": str(args.source), "scenarios": {}}
env = {**os.environ, "QT_QPA_PLATFORM": "offscreen", "QT_QUICK_CONTROLS_STYLE": "Basic"}
for name, (files, copies) in scenarios.items():
    for engine in (["shared"] if args.shared_only else ["legacy", "shared"]):
        label = name + "-" + engine
        command = [str(args.binary), "--copies", str(copies)]
        if engine == "legacy": command.append("--legacy")
        command += [str(fixtures / ("1080-" + str(i) + ".mp4")) for i in range(files)]
        with (args.output / (label + ".json")).open("w") as out, (args.output / (label + ".log")).open("w") as log:
            subprocess.run(command, stdout=out, stderr=log, env=env, check=True)
        results["scenarios"][label] = json.loads((args.output / (label + ".json")).read_text())
        print(label, "complete", flush=True)
(args.output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
