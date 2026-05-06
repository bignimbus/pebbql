# pebbql

A Pebble watchface that uses the GraphQL hexagraph as the clock face. The
hexagon's perimeter is the dial; hour and minute are angular wedges that sweep
continuously around it, clipped to the shape's footprint (hex with vertex
bumps) and masked into the visible white "petal" regions by the logo's own
inner-triangle cutouts.

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
                  (cutouts overdraw the hex with the same color, no
                  visible effect; vertex circles produce the bumps
                  protruding past the hex edges)
3. wedges       — hour, then minute on top; both flow over the hex
                  and bumps and bleed past onto bg
4. outside-     — paint bg over everything outside the (hex ∪ bumps)
   shape mask     shape, confining the wedges to the visible footprint
                  while preserving wedge color that flowed onto the
                  bumps
5. PDC pass 2   — outer hex set to GColorClear (so the pass-1 hex
                  isn't repainted over the wedges); cutouts paint
                  bg (revealing the GraphQL pattern through the
                  wedges); vertex circles set to GColorClear so they
                  don't repaint over the wedge color the mask just
                  preserved on the bumps
```

The vertex circles are intentionally inert in pass 2 — they get drawn
once in pass 1 and the wedge color flows over them like over any other
part of the hex. No per-vertex angular logic, no "popping" at threshold
transitions.

`gdraw_command_set_hidden(cmd, true)` is documented but appears to be a
no-op for path commands in this SDK — `GColorClear` (alpha=0) is the
reliable way to suppress a PDC command without removing it from the list.

#### Outside-shape mask

A single `gpath_draw_filled` polygon paints bg everywhere outside the
visible shape. The polygon is a screen-sized rect (CW) with a hex-plus-
bumps-shaped hole (CCW), joined by a degenerate "tunnel" that traverses
the same line in both directions and cancels under non-zero winding:

```
[0..3]   screen rect corners, CW
[4]      screen-rect start corner again (close outer)
[5..58]  inner hole: 6 vertices × 9 arc points each (CCW)
         visited in reverse SVG order (V_0, V_5, V_4, V_3, V_2, V_1)
[59]     first arc point again (close inner)
         → implicit close from [59] back to [0] = tunnel out, same
           line as [4]→[5], so the tunnel is invisible
```

The hole replaces each hex vertex with the outer 240° arc of that
vertex's bump circle, approximated as `BUMP_ARC_SEGMENTS` (= 8) chord
segments. Implicit straight lines between consecutive vertices' arcs
form the hex edges with the bump radius cut off at each end.

For each vertex `V_i`, the arc:
- starts at `V_i + r·dir((i+2)·60°)` — intersection with edge V_i→V_{i+1}
- ends at `V_i + r·dir((i+4)·60°)` — intersection with edge V_i→V_{i-1}
- sweeps -240° through the outward direction (`i·60°`)

The hex vertices come from PDC `cmd[0]` (see "PDC `cmd[0]` has 7
points, not 6" below) and the bump radius from `cmd[5]`, so the mask
follows whatever the SVG defines without hand-derived constants. The
mask polygon is built once at `window_load` and reused every frame.

The 8-segment chord approximation is inscribed in the bump circle, so
the bump's effective outline becomes a slight 8-gon. Chord depth is
~0.034·r (sub-pixel at the rendered radius); raise `BUMP_ARC_SEGMENTS`
if visible faceting ever shows up.

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
[0]       outer hexagon            → pass 1 logo / pass 2 GColorClear
[1..4]    inner-triangle cutouts   → pass 1 logo / pass 2 bg
[5..10]   vertex circles           → pass 1 logo / pass 2 GColorClear
```

The bump radius for the outside-shape mask is also read from `cmd[5]`
(all 6 circles share the same radius post-`scale_cb`). If you reorder
the SVG, update `HEX_PDC_INNER_FIRST` / `HEX_PDC_INNER_LAST` and the
bump-radius read in `prv_window_load`.

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
