#!/usr/bin/env python3
import argparse
import re
import statistics
from pathlib import Path

TEXT_RE = re.compile(r"\[TextPerf\].*completed\s+(\d+).*dropped\s+(\d+).*stale\s+(\d+).*avgMs\s+([0-9.]+).*p95Ms\s+([0-9.]+)")
CANVAS_RE = re.compile(r"\[CanvasPerf\].*zoomEvents\s+(\d+).*relayouts\s+(\d+).*fullRelayouts\s+(\d+).*selectionChrome\s+(\d+)")


def parse_log(path: Path):
    text_samples = []
    canvas_samples = []

    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        mt = TEXT_RE.search(line)
        if mt:
            text_samples.append({
                "completed": int(mt.group(1)),
                "dropped": int(mt.group(2)),
                "stale": int(mt.group(3)),
                "avg": float(mt.group(4)),
                "p95": float(mt.group(5)),
            })
            continue

        mc = CANVAS_RE.search(line)
        if mc:
            canvas_samples.append({
                "zoom": int(mc.group(1)),
                "relayout": int(mc.group(2)),
                "full": int(mc.group(3)),
                "chrome": int(mc.group(4)),
            })

    return text_samples, canvas_samples


def summarize(text_samples, canvas_samples):
    if text_samples:
        completed = sum(s["completed"] for s in text_samples)
        dropped = sum(s["dropped"] for s in text_samples)
        stale = sum(s["stale"] for s in text_samples)
        avg_p95 = statistics.mean(s["p95"] for s in text_samples)
        avg_avg = statistics.mean(s["avg"] for s in text_samples)
        drop_ratio = (dropped / completed) if completed else 0.0
        stale_ratio = (stale / completed) if completed else 0.0
    else:
        completed = dropped = stale = 0
        avg_p95 = avg_avg = drop_ratio = stale_ratio = 0.0

    if canvas_samples:
        zoom = sum(s["zoom"] for s in canvas_samples)
        relayout = sum(s["relayout"] for s in canvas_samples)
        full = sum(s["full"] for s in canvas_samples)
        chrome = sum(s["chrome"] for s in canvas_samples)
        relayout_per_zoom = (relayout / zoom) if zoom else 0.0
    else:
        zoom = relayout = full = chrome = 0
        relayout_per_zoom = 0.0

    return {
        "completed": completed,
        "dropped": dropped,
        "stale": stale,
        "avg_p95": avg_p95,
        "avg_avg": avg_avg,
        "drop_ratio": drop_ratio,
        "stale_ratio": stale_ratio,
        "zoom": zoom,
        "relayout": relayout,
        "full": full,
        "chrome": chrome,
        "relayout_per_zoom": relayout_per_zoom,
    }


def print_report(summary):
    print("Text/Canvas Perf Summary")
    print("========================")
    print(f"Text completed jobs: {summary['completed']}")
    print(f"Text dropped jobs:   {summary['dropped']} ({summary['drop_ratio']:.2%})")
    print(f"Text stale jobs:     {summary['stale']} ({summary['stale_ratio']:.2%})")
    print(f"Text avg duration:   {summary['avg_avg']:.2f} ms")
    print(f"Text avg p95:        {summary['avg_p95']:.2f} ms")
    print()
    print(f"Canvas zoom events:  {summary['zoom']}")
    print(f"Canvas relayouts:    {summary['relayout']}")
    print(f"Canvas full relays:  {summary['full']}")
    print(f"Selection chrome:    {summary['chrome']}")
    print(f"Relayout/zoom ratio: {summary['relayout_per_zoom']:.3f}")


def main():
    parser = argparse.ArgumentParser(description="Summarize Mouffette text/canvas perf logs")
    parser.add_argument("log", type=Path, help="Path to captured application log file")
    args = parser.parse_args()

    if not args.log.exists():
        raise SystemExit(f"Log file not found: {args.log}")

    text_samples, canvas_samples = parse_log(args.log)
    summary = summarize(text_samples, canvas_samples)
    print_report(summary)


if __name__ == "__main__":
    main()
