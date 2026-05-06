# pebbql

A Pebble watchface that uses the GraphQL hexagraph as the clock face. The
hexagon's perimeter is the dial; hour and minute are angular wedges that sweep
continuously around it, clipped to the hex's footprint and masked into the
visible white "petal" regions by the logo's own inner-triangle cutouts.

## Build & install

```sh
pebble build                                           # all targetPlatforms
pebble install --emulator emery                        # or chalk for round
pebble install --emulator emery build/pebbql.pbw       # cwd-independent form
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

Two seams in `src/c/pebbql.c` that future work plugs into:

### `Theme` (token-based, persisted)

Four `uint8_t` token fields — `bg`, `logo`, `hour`, `minute` — persisted under
key `PERSIST_KEY_THEME` (1). `theme_load()` populates defaults on first boot;
`theme_save()` is wired but unused until an AppMessage inbox handler exists.

Tokens (not raw `GColor` bytes) because of the **B&W collapse**: a "rhodamine"
choice on aplite/diorite has no real magenta to render with. `color_logo`,
`color_notch`, `color_bg` resolve tokens to platform-appropriate colors,
falling back to the contrasting shade on monochrome.

To wire up the config UI later: `app_message_register_inbox_received` →
populate `s_theme` from the dictionary → `theme_save()` →
`layer_mark_dirty(s_face_layer)`. About 30 lines.

### PDC for the logo, procedural for the time wedges

Hexagraph loads once via `gdraw_command_image_create_with_resource` and is
scaled to fit the screen in `window_load`. The wedges are drawn procedurally
each frame as polygons, layered with the PDC in a multi-step pipeline that
uses the PDC's own command order to clip the wedges into the visible petals.

#### Wedge math

Each hand has a continuous angular position and a half-width:

- `hour_angle = (h*60 + m) * TRIG_MAX_ANGLE / 720` — full sweep over 12h
- `minute_angle = m * TRIG_MAX_ANGLE / 60` — full sweep over 60min
- `HOUR_WEDGE_HALF_DEG`, `MIN_WEDGE_HALF_DEG` control angular extent

The wedge polygon is just a triangle from the logo center outward to two
corners *past the screen edge* (`s_far_distance_px = bounds.w + bounds.h`,
guaranteed > diagonal). The rasterizer clips off-screen pixels for free.
Past-screen corners avoid sin/cos rounding artifacts at exact vertex angles.

#### Render pipeline (per frame, in `face_update_proc`)

```
1. fill bg
2. PDC pass 1   — paint everything in logo color, draw the image
                  (cutouts and vertex circles overdraw the outer hex
                  with the same color → only the hex shape is visible)
3. wedges       — hour, then minute on top; both bleed past the hex
4. outside-hex  — paint bg over everything outside the hex shape,
   mask           confining the wedges to the hex's footprint
5. PDC pass 2   — outer hex set to GColorClear (no-op so the pass-1
                  hex isn't repainted over the wedges); cutouts paint
                  bg (revealing the GraphQL pattern through the
                  wedges); vertex circles paint logo OR wedge color
                  depending on whether the wedge angularly covers
                  that vertex (so vertex bumps participate in the
                  sweep instead of stamping over it)
```

`gdraw_command_set_hidden(cmd, true)` is documented but appears to be a
no-op for path commands in this SDK — `GColorClear` (alpha=0) is the
reliable way to suppress a PDC command without removing it from the list.

#### Outside-hex mask

A single `gpath_draw_filled` polygon paints bg everywhere outside the hex.
The polygon is a screen-sized rect (CW) with a hex-shaped hole (CCW),
joined by a degenerate "tunnel" edge that traverses the same line in both
directions and cancels under non-zero winding:

```
[0..3]  screen rect corners, CW
[4]     screen-rect start corner again (close outer)
[5]     hex vertex 0  (tunnel target)
[6..10] hex vertices 5,4,3,2,1  (CCW)
[11]    hex vertex 0 again (close inner)
        → implicit close from [11] back to [0] = tunnel out, same line
          as [4]→[5], so the tunnel is invisible in the rasterized fill
```

The hex vertices come from the PDC itself (see "PDC `cmd[0]` has 7 points,
not 6" below) — no hand-derived geometry, so the mask follows whatever the
SVG defines.

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
the logo color; the 4 inner triangles get the **bg color** and draw on top,
producing the cutout effect through pure layering. Both colors come from
the theme, so flipping bg flips the cutouts too — the logo always reads
correctly.

### Command index layout (fragile)

The recolor callbacks decide fill colors by command index. The layout is
fixed by the order of elements in `hexagraph.normalized.svg`:

```
[0]       outer hexagon            → logo color (pass 1) / GColorClear (pass 2)
[1..4]    inner-triangle cutouts   → bg color
[5..10]   vertex circles           → logo OR wedge color (pass 2, by angle)
```

If you reorder the SVG, update `HEX_PDC_INNER_FIRST` / `HEX_PDC_INNER_LAST`
and the vertex-circle index range in `recolor_pass2_cb`.

### PDC `cmd[0]` has 7 points, not 6

Reading the outer hexagon's vertices from `cmd[0]` (so the mask polygon
matches the SVG without hand-derived constants) needs to skip the first
point. svg2pdc, via the `svg.path` library, expands `M50,6.9 L… Z` into:

```
[0] (50, 6.9)    ← Move's start (duplicate of vertex 0)
[1] (50, 6.9)    ← Line[0]'s start = vertex 0 (top)
[2] (87.3, 28.5) ← vertex 1 (upper-right)
[3] (87.3, 71.5) ← vertex 2 (lower-right)
[4] (50, 93.1)   ← vertex 3 (bottom)
[5] (12.7, 71.5) ← vertex 4 (lower-left)
[6] (12.7, 28.5) ← vertex 5 (upper-left)
```

`compare_points` in `parse_path` removes the *trailing* duplicate (Close's
start) but not the leading one. Read **indices 1..6** to get the 6 unique
hex corners; reading 0..5 silently drops vertex 5 and the mask collapses
on the upper-left side.

## Files

```
src/c/pebbql.c                  watchface
resources/hexagraph.pdc         generated; bundled as raw resource
tools/svg2pdc.py                converter (patched for Py3)
tools/pebble_image_routines.py  converter helper (patched for Py3)
tools/hexagraph.normalized.svg  converter input (hand-massaged)
assets/GraphQL Logo (*).{svg,png}   original brand assets, untouched
```
