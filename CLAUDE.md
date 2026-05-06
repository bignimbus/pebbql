# pebbql

A Pebble watchface that uses the GraphQL hexagraph as the clock face. The
hexagon's perimeter is the dial; hour and minute are filled notches that walk
around it.

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

### PDC for the logo, procedural for the notches

Hexagraph loads once via `gdraw_command_image_create_with_resource`, gets
scaled to fit the screen in `window_load`, and is recolored each frame in
`face_update_proc` before drawing. The notches draw on top with
`graphics_fill_circle` so they can move every minute without touching the PDC.

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

`recolor_cb` decides logo-vs-bg by command index. The layout is fixed by
the order of elements in `hexagraph.normalized.svg`:

```
[0]       outer hexagon            → logo color
[1..4]    inner-triangle cutouts   → bg color
[5..10]   vertex circles           → logo color
```

If you reorder the SVG, update `HEX_PDC_INNER_FIRST` / `HEX_PDC_INNER_LAST`
in `src/c/pebbql.c`.

## Notch placement

`LOGO_VERTEX_PERMIL = 431` — distance from logo center to hexagon corner, in
permil of half-viewbox. The SVG corners are at `(50, 6.9)`, ~43.1 from
center `(50, 50)`. This puts the notches on the visible hexagon edge.

The vertex-circle centers (40.5%) are visibly *inside* the hexagon edge — an
earlier version of this code used that radius and the notches floated inside
the perimeter. If notches drift, this constant is the suspect.

## Files

```
src/c/pebbql.c                  watchface
resources/hexagraph.pdc         generated; bundled as raw resource
tools/svg2pdc.py                converter (patched for Py3)
tools/pebble_image_routines.py  converter helper (patched for Py3)
tools/hexagraph.normalized.svg  converter input (hand-massaged)
assets/GraphQL Logo (*).{svg,png}   original brand assets, untouched
```
