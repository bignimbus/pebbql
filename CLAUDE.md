# pebbql

A Pebble watchface: the GraphQL hexagraph as the dial face, with a simple
analog hour and minute hand sweeping from the center.

## Build & install

```sh
pebble build                                           # all targetPlatforms
pebble install --emulator emery                        # or chalk for round
pebble install --emulator emery build/pebbql.pbw       # cwd-independent form
pebble install --phone <ip> build/pebbql.pbw --logs    # real device
```

`pebble install` looks for `build/pebbql.pbw` relative to cwd. After
`pebble wipe`, `pebble clean`, or any state reset, **rebuild before
installing** — the install error wording ("must run this command from a
project directory") is misleading; the real cause is usually a missing pbw.

### Recovering a wedged emulator

Symptoms: splash bar loops forever, `App install failed`, `ConnectionResetError`
when QEMU spawns. Almost always stale state, not application code.

```sh
pebble kill                  # graceful
pkill -f qemu-pebble         # kill any stragglers
pebble wipe                  # nuke SPI flash (loses installed apps + persist)
```

Multiple `qemu-pebble` processes from prior `pebble install` runs commonly
hold the emulator's tcp ports — that's the most frequent cause of "App
install failed."

## Architecture

### Theme: a single dark/light bool

`Theme { uint8_t dark }`, persisted under `PERSIST_KEY_THEME` (1).
`theme_load` defaults to dark on first boot. The AppMessage inbox handler
(`MESSAGE_KEY_DARK`) writes the new value and re-renders.

Color resolution at draw time:

|       | bg    | logo (color)   | logo (mono) | hand fill | hand border |
|-------|-------|----------------|-------------|-----------|-------------|
| dark  | black | rhodamine      | white       | white     | black       |
| light | white | rhodamine      | black       | black     | white       |

`PBL_IF_COLOR_ELSE` collapses the logo to the contrasting shade on B&W
platforms (aplite, diorite). The hand border ensures hands stay visible
when sweeping over a same-color region (e.g. a white minute hand over the
white logo cutouts in dark mode).

### Render pipeline (per frame)

```
1. fill bg
2. PDC pass     — hex + vertex circles in logo color, inner-triangle
                  cutouts in bg (replicates SVG evenodd by layering)
3. hour hand    — filled rectangle, then outlined for the border
4. minute hand  — same, on top
5. center pin   — filled circle + outline at the rotation point
```

That's it. No wedge math, no shape masks, no per-vertex logic.

### Hand math

Each hand is a 4-point rectangle from `s_logo_center` extending outward
by `length`, rotated to the time angle (Pebble convention: 0 = up, CW).

- `hour_angle = (h*60 + m) * TRIG_MAX_ANGLE / 720`
- `minute_angle = m * TRIG_MAX_ANGLE / 60`
- Length is a fraction of `s_clock_radius` (= half the rendered logo size)
  so platforms with different screen sizes look proportional.

Tunables at the top of the drawing section:

- `HOUR_LENGTH_NUM/DEN`, `HOUR_HALF_WIDTH`
- `MIN_LENGTH_NUM/DEN`, `MIN_HALF_WIDTH`
- `HAND_BORDER_WIDTH`, `CENTER_PIN_RADIUS`

## The PDC, and why this isn't simple

The PDC at `resources/hexagraph.pdc` is generated from
`tools/hexagraph.normalized.svg`. Don't try to convert the original SVGs in
`assets/` directly — both would silently produce a broken PDC.

### Regenerating

```sh
"$HOME/Library/Application Support/Pebble SDK/SDKs/4.9.169/.venv/bin/python" \
  tools/svg2pdc.py -p \
  -o resources/hexagraph.pdc tools/hexagraph.normalized.svg
```

### `tools/svg2pdc.py` is patched

Pulled from `pebble-examples/cards-example/tools/svg2pdc.py`. The upstream is
Python 2 — these patches were applied to make it run on the SDK's Python 3.13:

- `getchildren()` → `list(...)` (removed in 3.9)
- `pebble_image_routines.py:pebble_truncate_color_to_pebble_palette`: `/` → `//`
- `serialize_image` / `serialize_sequence`: `"PDCI"` / `"PDCS"` → bytes literals
- File write mode: `'w'` → `'wb'`
- `CircleCommand.__init__`: `self.radius = int(round(radius))`

Re-pulling from upstream without re-applying these will break the build.

### SVG normalization, why

`tools/hexagraph.normalized.svg` is the converter's actual input. Two things
distinguish it from the brand assets:

1. **Children wrapped in `<g fill="…">` groups.** svg2pdc inherits `fill` from
   `<g>` only, never from the root `<svg>` element. Without the wrapper,
   elements end up with `fill_color=0` and silently drop out of the PDC.

2. **Each sub-path is its own `<path>` element.** svg2pdc flattens
   `M…Z M…Z M…Z` in a single `<path>` into one self-intersecting polygon —
   `fill-rule="evenodd"` is lost. The hexagraph's negative-space triangle
   pattern depends on evenodd, so it has to be expressed as separate paths.

We replicate evenodd by **layering**: outer hexagon + 6 vertex circles get
the logo color; the 4 inner triangles get bg and draw on top, producing the
cutout effect through pure layering. Both colors come from the theme, so
flipping the toggle flips the cutouts too — the logo always reads correctly.

### Command index layout (fragile)

`recolor_cb` decides fill colors by command index. The layout is fixed by
the order of elements in `hexagraph.normalized.svg`:

```
[0]       outer hexagon            → logo
[1..4]    inner-triangle cutouts   → bg
[5..10]   vertex circles           → logo
```

If you reorder the SVG, update `HEX_PDC_INNER_FIRST` / `HEX_PDC_INNER_LAST`.

## Files

```
src/c/pebbql.c                  watchface
src/pkjs/index.js               settings UI (data: URL, no Clay)
resources/hexagraph.pdc         generated; bundled as raw resource
tools/svg2pdc.py                converter (patched for Py3)
tools/pebble_image_routines.py  converter helper (patched for Py3)
tools/hexagraph.normalized.svg  converter input (hand-massaged)
assets/GraphQL Logo (*).{svg,png}   original brand assets, untouched
```
