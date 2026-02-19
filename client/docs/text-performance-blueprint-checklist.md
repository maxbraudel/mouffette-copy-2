# Text Performance Blueprint Checklist

## Purpose
This checklist operationalizes the text zoom performance blueprint into measurable milestones.

## Runtime Flags
- `MOUFFETTE_TEXT_PROFILING=1` enables text raster performance snapshots.
- `MOUFFETTE_TEXT_HOT_LOGS=1` enables verbose text hot-path logs (disabled by default).
- `MOUFFETTE_CANVAS_PROFILING=1` enables canvas zoom/relayout perf snapshots.
- `MOUFFETTE_FORCE_FULL_VIEWPORT_UPDATE=1` forces full viewport redraw mode for artifact fallback.
- `MOUFFETTE_TARGETED_ZOOM_RELAYOUT=1` enables targeted overlay relayout during zoom.

## Glyph Key Contract
- Unique glyph identity is glyph-index based (`QGlyphRun` + `QRawFont`) rather than Unicode codepoint.
- Cache key dimensions:
	- font family/style/pixel-size,
	- glyph index,
	- fill color + stroke color,
	- highlight signature (enabled + color) for strict style invariants,
	- stroke width,
	- quantized scale bucket.
- Expected behavior: repeated glyph keys reuse rendered bitmaps; misses should track newly introduced unique keys.

## Quality Constraints
- Visual parity target: no perceptible difference at 100% zoom versus baseline renderer.
- During zoom, temporary quality drift is acceptable only if no persistent artifacts remain after raster settles.

## Rollout Feature Flags
- `text.render.scheduler.v2` (alias: `MOUFFETTE_TEXT_RENDER_SCHEDULER_V2=1`)
- `text.cache.policy.v2` (alias: `MOUFFETTE_TEXT_CACHE_POLICY_V2=1`)
- `canvas.zoom.coalescer` (alias: `MOUFFETTE_TARGETED_ZOOM_RELAYOUT=1`)
- `text.renderer.gpu` (alias: `MOUFFETTE_TEXT_RENDERER_GPU=1`)
- `text.renderer.glyph_atlas.v1` (alias: `MOUFFETTE_TEXT_GLYPH_ATLAS_V1=1`)

## Scenarios
### S1: Single Heavy Text
- Setup: 1 text item, large content, border enabled.
- Action: rapid zoom in/out for 20 seconds.
- Capture: `[TextPerf]`, `[TextGlyphCache]`, `[RemoteTextGlyphCache]` and `[CanvasPerf]` logs.

### S2: Dense Mixed Scene
- Setup: 20 text items + media items.
- Action: continuous pinch/ctrl-wheel zoom for 30 seconds.
- Capture: same logs + subjective stutter assessment.

### S3: Edit + Navigation Stress
- Setup: active inline text editing on one item, other items present.
- Action: zoom + pan + selection changes for 20 seconds.
- Capture: logs + correctness checks (caret, selection, text color).

## Acceptance Thresholds
### M1 (Stabilization)
- No severe visible hitching in S1.
- Canvas relayout calls significantly lower than zoom events in S1/S2.
- No regressions in fit-to-text and Alt-resize.

### M2 (Pipeline Consolidation)
- Stale and dropped jobs near-zero under sustained zoom in S2.
- p95 raster duration lower than baseline in S1/S2.
- Glyph cache hit rate climbs after warmup and stays high in zoom/pan loops.
- Glyph cache misses scale with new unique glyph/style keys, not total text length.

### M3 (Interaction Cleanup)
- Stable interaction in S3 with no flicker/selection lag.
- No zoom-triggered UI desync.

### M4 (Renderer Prototype)
- Functional parity between baseline and new backend path.
- Lower frame-time variance under S2.

## Regression Checklist
- Create/edit/commit/cancel text.
- Fit-to-text on/off transitions.
- Alt-resize axis and corner behavior.
- Text color, border, highlight correctness.
- Selection chrome and overlay panel positioning during zoom/pan.

## Notes
- Keep hot logs disabled during normal testing to avoid measurement skew.
- Use full viewport mode only as fallback for visual artifact investigation.
- Keep glyph atlas path flag OFF by default until two clean validation cycles complete.

## Scenario Replay Automation
- Capture logs for each scenario run with profiling enabled:
	- `MOUFFETTE_TEXT_PROFILING=1 MOUFFETTE_CANVAS_PROFILING=1 ./build/MouffetteClient.app/Contents/MacOS/MouffetteClient > /tmp/mouffette-s1.log 2>&1`
- Run the summary utility:
	- `python3 client/tools/text_perf_summary.py /tmp/mouffette-s1.log`
- Apply the same flow for S2 and S3 (`/tmp/mouffette-s2.log`, `/tmp/mouffette-s3.log`) and compare:
	- drop/stale ratio trend (target near-zero in S2),
	- average p95 trend (target lower than baseline),
	- glyph-cache hit/miss trend (target increasing hit rate after warmup),
	- relayout/zoom ratio (target significantly below 1.0).

## GPU Rollout Criteria
- Keep `MOUFFETTE_TEXT_RENDERER_GPU=1` opt-in until all of the following are true:
	- S1/S2/S3 pass with functional parity (no text alignment/caret/selection regressions),
	- no statistically significant regression in average p95 raster duration,
	- no sustained increase in stale/dropped ratio under S2,
	- no new visual artifacts in freeze-zone compositing and present-only path.
- Promote default to GPU path only after two consecutive validation runs with matching results.
- If a regression appears, immediately roll back via flag and keep scheduler/cache v2 enabled.

## Glyph Atlas Rollout Criteria
- Keep `MOUFFETTE_TEXT_GLYPH_ATLAS_V1=1` opt-in for first pass.
- Promote to default only when:
	- host and remote text output are visually equivalent across S1/S2/S3,
	- no new stale-raster artifacts appear during zoom/pan and Alt-resize,
	- cache hit ratio remains stable after warmup in repeated replay runs.
- Instant rollback path: disable `MOUFFETTE_TEXT_GLYPH_ATLAS_V1` while retaining scheduler/cache-v2 improvements.
