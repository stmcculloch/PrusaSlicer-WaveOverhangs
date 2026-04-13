# Wave Overhangs Guide

Wave overhangs are an experimental PrusaSlicer feature for printing unsupported overhang regions with wave-shaped toolpaths instead of ordinary support material. The goal is simple: keep the part printable while reducing cleanup work, material use, and ugly support scars.

This fork started with the core wave-overhang algorithm and then quickly added the controls needed to make it usable on real parts: pattern choices, overlap tuning, line spacing and width, speed and fan overrides, and several geometry fixes for thin regions and perimeter transitions.

## Why use wave overhangs?

Wave overhangs are most useful when support removal is expensive, messy, or risky:

- Functional parts where support marks are unacceptable.
- Mechanical parts made in batches, where post-processing time becomes the real bottleneck.
- Parts with holes or branching overhang geometry, where simple inset-based overhang strategies fall apart.
- Shallow overhang surfaces that behave like a series of tiny 90 degree overhangs when viewed layer by layer.

Compared with the earlier arc-overhang approach, wave overhangs produce smoother-looking toolpaths and handle more general shapes, including holes in the overhanging region.

## Quick start

If you just want a sensible starting point:

1. Enable `Use wave overhangs (Experimental)`.
2. Leave `Outer perimeters during wave overhangs` at `1`.
3. Use the `Smart` pattern.
4. Start with `Wave overhang perimeter overlap = 0.1 mm`.
5. Leave `Wave overhang line spacing = 0.35 mm` and `Wave overhang line width = 0.4 mm`.
6. Start with `Wave overhang print speed = 2 mm/s`, `Wave overhang travel speed = 40 mm/s`, and `Wave overhang fan speed = 100%`.

For parts where warping is a concern, a few print choices are worth trying:

- Use only `1` bottom shell above the reclaimed overhang when possible.
- Prefer sparse infill above the wave layer, especially `Lightning` or low-density `Gyroid`.
- Treat fiber-filled PLA or similarly stiff materials as promising candidates when appearance and warping matter more than price.

## What each major feature adds

### Native wave-overhang generation inside PrusaSlicer

The first major step was moving wave overhangs from a research idea and post-processing workflow into PrusaSlicer itself. That matters because the feature can now work directly from sliced geometry, with access to perimeters, line widths, speeds, cooling, and overhang regions without an extra tool in the middle.

For users, this means wave overhangs behave like a real slicer feature instead of a manual g-code experiment.

### Outer perimeter preservation

`Outer perimeters during wave overhangs` controls how many normal perimeter shells are kept around the overhang region while the inside is filled by waves.

Why it matters:

- Keeping one regular perimeter helps preserve edge quality.
- The wave region does not blindly replace every shell.
- The slicer now caps this value to the real local perimeter count, preventing invalid settings from breaking a slice.

In practice, `1` is the intended default for most prints.

### Perimeter overlap for better bonding

Real prints quickly showed a failure mode where the wave field and the kept perimeter did not bond strongly enough, causing detached edges. To address that, this fork added `Wave overhang perimeter overlap`.

What it does:

- Pushes the wave growth boundary slightly toward the kept perimeter.
- Lets the last wave sit closer to the perimeter shell.
- Reduces the visible and structural gap between the wave region and the wall.

The default is `0.1 mm`. In early print discussions, `0.05 mm` to `0.2 mm` emerged as a practical tuning range when chasing perimeter adhesion problems.

### Pattern selection: Monotonic, Zig Zag, and Smart

One of the biggest usability additions was giving users a real choice in how wave lines are printed.

`Monotonic`

- Prints each wave line in the same direction.
- Most predictable behavior.
- Gives earlier lines more time to cool.
- Usually means more travel and more retraction activity.

`Zig Zag`

- Connects lines into a back-and-forth path.
- Reduces travel moves and retractions.
- Later updates added a depth-first ordering so the toolpath follows local branches instead of jumping around the part.
- Can be attractive on simple geometry, but may build up more heat or droop on complex shapes.

`Smart`

- Keeps the overall look of `Monotonic`.
- Chooses the better-supported end of each line as the starting point.
- Helps avoid cases where extrusion starts from the weakest end of a track.
- Became the default because it is expected to outperform plain monotonic printing in most cases.

If you are unsure, use `Smart`.

## Geometry and process controls

Wave overhangs became much more practical once the fork added dedicated tuning controls instead of forcing one hard-coded behavior.

### Line spacing and line width

These two settings control how tightly the wave field is packed:

- `Wave overhang line spacing` is the centerline distance between adjacent wave paths.
- `Wave overhang line width` is the actual extrusion width used for those paths.

If width is larger than spacing, neighboring waves intentionally overlap a little. That overlap can improve lateral support and adhesion, but too much can also add stress or over-extrusion artifacts.

Defaults:

- `Line spacing = 0.35 mm`
- `Line width = 0.4 mm`

### Print speed, travel speed, and fan speed

Wave overhangs are sensitive enough that they need their own motion and cooling controls:

- `Wave overhang print speed`
- `Wave overhang travel speed`
- `Wave overhang fan speed`

These let the unsupported region print slowly and cool aggressively without forcing the rest of the part to use the same settings.

Defaults:

- `Print speed = 2 mm/s`
- `Travel speed = 40 mm/s`
- `Fan speed = 100%`

## Choosing the right pattern

Use this rule of thumb:

- Choose `Smart` first. It is the safest default and the best general-purpose option.
- Choose `Monotonic` if you want the most predictable, one-direction-at-a-time behavior.
- Choose `Zig Zag` if your main goal is minimizing travel and retractions, and the geometry is simple enough that extra local heat buildup is unlikely to hurt the print.

## Current limitations

Wave overhangs are still experimental. Based on both the development history and early print feedback, these are the main caveats:

- Warping is still a real challenge, especially on large unsupported spans.
- The pattern that looks best in preview may not be the best one thermally.
- Complex parts with branching or holes often benefit from `Smart` more than `Zig Zag`.
- Large reclaimed overhang regions may still be better served by ordinary supports when reliability matters more than experimentation.

## See also

- [README](../README.md)
- [Wave-overhang releases](https://github.com/stmcculloch/PrusaSlicer-WaveOverhangs/releases)
