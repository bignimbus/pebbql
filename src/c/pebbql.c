#include <pebble.h>

// =====================================================================
// Theme: tokens persisted to storage, resolved to GColor at draw time.
// Storing tokens (not raw GColor bytes) lets each platform map a choice
// like "rhodamine" to a sensible value — magenta on color displays,
// white on monochrome, etc.
// =====================================================================

typedef enum {
  THEME_LOGO_WHITE     = 0,
  THEME_LOGO_BLACK     = 1,
  THEME_LOGO_RHODAMINE = 2,
} ThemeLogo;

typedef enum {
  THEME_BG_DARK  = 0,
  THEME_BG_LIGHT = 1,
} ThemeBg;

typedef enum {
  THEME_NOTCH_RHODAMINE = 0,
  THEME_NOTCH_CYAN      = 1,
  THEME_NOTCH_WHITE     = 2,
  THEME_NOTCH_BLACK     = 3,
} ThemeNotch;

typedef struct __attribute__((packed)) {
  uint8_t bg;
  uint8_t logo;
  uint8_t hour;
  uint8_t minute;
} Theme;

#define PERSIST_KEY_THEME 1

static Theme s_theme;

static GColor color_bg(ThemeBg t) {
  return (t == THEME_BG_LIGHT) ? GColorWhite : GColorBlack;
}

static GColor color_logo(ThemeLogo t, ThemeBg bg) {
  switch (t) {
    case THEME_LOGO_BLACK:     return GColorBlack;
    case THEME_LOGO_RHODAMINE: return PBL_IF_COLOR_ELSE(
        GColorFashionMagenta,
        bg == THEME_BG_LIGHT ? GColorBlack : GColorWhite);
    case THEME_LOGO_WHITE:
    default:                   return GColorWhite;
  }
}

static GColor color_notch(ThemeNotch t, ThemeBg bg) {
  GColor contrast = (bg == THEME_BG_LIGHT) ? GColorBlack : GColorWhite;
  switch (t) {
    case THEME_NOTCH_BLACK:     return GColorBlack;
    case THEME_NOTCH_WHITE:     return GColorWhite;
    case THEME_NOTCH_CYAN:      return PBL_IF_COLOR_ELSE(GColorPictonBlue, contrast);
    case THEME_NOTCH_RHODAMINE:
    default:                    return PBL_IF_COLOR_ELSE(GColorFashionMagenta, contrast);
  }
}

static void theme_load(void) {
  s_theme = (Theme) {
    .bg     = THEME_BG_DARK,
    .logo   = THEME_LOGO_WHITE,
    .hour   = THEME_NOTCH_RHODAMINE,
    .minute = THEME_NOTCH_CYAN,
  };
  if (persist_exists(PERSIST_KEY_THEME)) {
    persist_read_data(PERSIST_KEY_THEME, &s_theme, sizeof(s_theme));
  }
}

// Hook for the eventual config-message handler — left wired up but unused.
static void __attribute__((unused)) theme_save(void) {
  persist_write_data(PERSIST_KEY_THEME, &s_theme, sizeof(s_theme));
}

// =====================================================================
// Drawing
// =====================================================================

static Window *s_window;
static Layer  *s_face_layer;
static GDrawCommandImage *s_hexagraph;

// Logo geometry resolved at window_load. The PDC's source viewbox is
// 100x100; we scale it once to fit the smallest screen dimension.
static GPoint s_logo_origin;
static GPoint s_logo_center;

#define LOGO_VIEWBOX        100

// =====================================================================
// Time markers — continuous angular sweep
//
// Hour and minute each have a continuous angular position (derived from
// the time) and an angular half-width. The position swings around the
// perimeter at the natural rate — hour: 360° / 12h = 30°/hour, minute:
// 360° / 60min = 6°/min. The wedge is a triangle from the logo center
// extending past the screen edge.
//
// Render pipeline per frame:
//   1. fill bg
//   2. PDC pass 1 — outer hexagon only (others painted same color)
//   3. wedges — drawn over the hex, also bleed past it onto bg
//   4. outside-hex mask — polygon "screen rect with hexagonal hole"
//      painted in bg, restoring bg outside the hex shape and confining
//      the wedges to the hex's footprint
//   5. PDC pass 2 — inner-triangle cutouts paint bg over the wedge in
//      cutout areas (revealing the GraphQL pattern), vertex circles
//      paint logo or wedge color depending on whether the wedge
//      angularly covers them
//
// The mask polygon's hex hole uses the actual outer-hexagon vertices
// from PDC cmd[0], so the geometry follows the SVG/PDC source of truth
// without hand-derived constants.
// =====================================================================

// Wedge angular half-widths (degrees). Total wedge sweep = 2× this.
//   Hour:  30° wedge. Sweep rate 30°/hour.
//   Min:   10° wedge. Sweep rate 6°/min.
#define HOUR_WEDGE_HALF_DEG   6
#define MIN_WEDGE_HALF_DEG    3

static int     s_far_distance_px;       // wedge outer-corner distance, past screen
static int32_t s_hour_half_width;       // Pebble angle units
static int32_t s_min_half_width;
static GPoint  s_hex_vertices[6];       // outer hexagon corners, screen coords
static GRect   s_screen_bounds;         // for the outside-hex mask polygon

#define LOGO_MARGIN          12

// PDC command layout, fixed by tools/hexagraph.normalized.svg ordering:
//   [0]      outer hexagon          → logo color
//   [1..4]   inner-triangle cutouts → bg color  (creates the evenodd effect)
//   [5..10]  vertex circles         → logo color
#define HEX_PDC_INNER_FIRST   1
#define HEX_PDC_INNER_LAST    4
#define HEX_PDC_EXPECTED_LEN 11

// PDC scaling: walk every command, scale points + circle radii by num/den.
typedef struct { int32_t num; int32_t den; } ScaleCtx;

static bool scale_cb(GDrawCommand *cmd, uint32_t index, void *ctx) {
  ScaleCtx *s = (ScaleCtx *)ctx;
  uint16_t n = gdraw_command_get_num_points(cmd);
  for (uint16_t i = 0; i < n; i++) {
    GPoint p = gdraw_command_get_point(cmd, i);
    p.x = (int16_t)(((int32_t)p.x * s->num) / s->den);
    p.y = (int16_t)(((int32_t)p.y * s->num) / s->den);
    gdraw_command_set_point(cmd, i, p);
  }
  if (gdraw_command_get_type(cmd) == GDrawCommandTypeCircle) {
    uint16_t r = gdraw_command_get_radius(cmd);
    gdraw_command_set_radius(cmd, (uint16_t)(((int32_t)r * s->num) / s->den));
  }
  return true;
}

// PDC pass 1: paint everything in the logo color so only the outer
// hexagon shape appears (cutouts and vertex circles overdraw the same
// color, no visible effect). Edge highlights drawn after this pass sit
// on top of a clean filled hexagon, ready to be clipped by pass 2.
// Also unhides any command that pass 2 may have hidden last frame.
typedef struct { GColor logo; } RecolorPass1Ctx;

static bool recolor_pass1_cb(GDrawCommand *cmd, uint32_t index, void *ctx_) {
  RecolorPass1Ctx *ctx = ctx_;
  gdraw_command_set_hidden(cmd, false);
  gdraw_command_set_fill_color(cmd, ctx->logo);
  return true;
}

// Returns true when angle `a` lies within `half_width` of `center`
// (using the shortest signed difference, mod TRIG_MAX_ANGLE).
static bool angle_within(int32_t a, int32_t center, int32_t half_width) {
  int32_t d = a - center;
  while (d < 0) d += TRIG_MAX_ANGLE;
  while (d >= TRIG_MAX_ANGLE) d -= TRIG_MAX_ANGLE;
  if (d > TRIG_MAX_ANGLE / 2) d = TRIG_MAX_ANGLE - d;
  return d <= half_width;
}

// PDC pass 2: cutouts paint bg over the wedges drawn between passes
// (clipping them to the visible white petal regions); vertex circles
// paint hour/min color when their angle is inside the corresponding
// wedge, else logo. The outer hexagon is hidden — pass 1 drew it.
typedef struct {
  GColor  logo;
  GColor  bg;
  int32_t hour_angle;
  int32_t hour_half_width;
  GColor  hour_color;
  int32_t min_angle;
  int32_t min_half_width;
  GColor  min_color;
} RecolorPass2Ctx;

static bool recolor_pass2_cb(GDrawCommand *cmd, uint32_t index, void *ctx_) {
  RecolorPass2Ctx *ctx = ctx_;
  // Make the outer hexagon transparent so it doesn't repaint over the
  // wedges drawn after pass 1. (gdraw_command_set_hidden appears to be
  // a no-op for path commands in this SDK; GColorClear is reliable.)
  if (index == 0) {
    gdraw_command_set_fill_color(cmd, GColorClear);
    return true;
  }
  if (index >= HEX_PDC_INNER_FIRST && index <= HEX_PDC_INNER_LAST) {
    gdraw_command_set_fill_color(cmd, ctx->bg);
    return true;
  }
  // Vertex circles at indices 5..10. Min checked first so it wins when
  // both wedges cover the same vertex (matching min-on-top layering).
  if (index >= 5 && index <= 10) {
    int v = (int)index - 5;
    int32_t va = (int32_t)v * (TRIG_MAX_ANGLE / 6);
    if (angle_within(va, ctx->min_angle, ctx->min_half_width)) {
      gdraw_command_set_fill_color(cmd, ctx->min_color);
      return true;
    }
    if (angle_within(va, ctx->hour_angle, ctx->hour_half_width)) {
      gdraw_command_set_fill_color(cmd, ctx->hour_color);
      return true;
    }
  }
  gdraw_command_set_fill_color(cmd, ctx->logo);
  return true;
}

// Time → angular position. Hour completes 360° in 12h, minute in 60min.
// Both expressed in Pebble angle units (TRIG_MAX_ANGLE = full circle).
static int32_t hour_angle_at(int hour12, int minute) {
  return ((int32_t)(hour12 * 60 + minute) * TRIG_MAX_ANGLE) / (12 * 60);
}

static int32_t minute_angle_at(int minute) {
  return ((int32_t)minute * TRIG_MAX_ANGLE) / 60;
}

// Draw a wedge: a triangle from the logo center out to two corners
// past the screen edge, at angles center ± half_width. The corners are
// far enough out that the triangle reaches the screen edge in any
// direction; the rasterizer clips the parts off-screen for free.
static void draw_wedge(GContext *ctx, int32_t center, int32_t half_width,
                       GColor color) {
  int32_t theta1 = center - half_width;
  int32_t theta2 = center + half_width;

  GPoint p1 = {
    .x = s_logo_center.x + (int16_t)((sin_lookup(theta1) * s_far_distance_px) / TRIG_MAX_RATIO),
    .y = s_logo_center.y - (int16_t)((cos_lookup(theta1) * s_far_distance_px) / TRIG_MAX_RATIO),
  };
  GPoint p2 = {
    .x = s_logo_center.x + (int16_t)((sin_lookup(theta2) * s_far_distance_px) / TRIG_MAX_RATIO),
    .y = s_logo_center.y - (int16_t)((cos_lookup(theta2) * s_far_distance_px) / TRIG_MAX_RATIO),
  };

  GPoint corners[3] = { s_logo_center, p1, p2 };
  GPathInfo info = { .num_points = 3, .points = corners };
  GPath *path = gpath_create(&info);
  if (path) {
    graphics_context_set_fill_color(ctx, color);
    gpath_draw_filled(ctx, path);
    gpath_destroy(path);
  }
}

// Paints `color` over everything outside the outer hexagon. Uses the
// non-zero winding rule to fill the area between an outer screen-rect
// (CW) and an inner hex hole (CCW). The two duplicate vertices form a
// degenerate "tunnel" between the two loops that cancels itself out.
static void draw_outside_hex(GContext *ctx, GColor color) {
  GPoint mask[12];
  // Outer screen rectangle, CW from top-left.
  mask[0] = (GPoint){ s_screen_bounds.origin.x,
                      s_screen_bounds.origin.y };
  mask[1] = (GPoint){ s_screen_bounds.origin.x + s_screen_bounds.size.w,
                      s_screen_bounds.origin.y };
  mask[2] = (GPoint){ s_screen_bounds.origin.x + s_screen_bounds.size.w,
                      s_screen_bounds.origin.y + s_screen_bounds.size.h };
  mask[3] = (GPoint){ s_screen_bounds.origin.x,
                      s_screen_bounds.origin.y + s_screen_bounds.size.h };
  mask[4] = mask[0];                  // close outer loop
  // Inner hex hole — opposite winding to the outer, so the "between"
  // area fills under the non-zero rule.
  mask[5]  = s_hex_vertices[0];       // tunnel into the hole
  mask[6]  = s_hex_vertices[5];
  mask[7]  = s_hex_vertices[4];
  mask[8]  = s_hex_vertices[3];
  mask[9]  = s_hex_vertices[2];
  mask[10] = s_hex_vertices[1];
  mask[11] = s_hex_vertices[0];       // close inner loop; implicit
                                      // close to mask[0] is the tunnel
                                      // out — same line as mask[4]→[5],
                                      // so they cancel.

  GPathInfo info = { .num_points = 12, .points = mask };
  GPath *path = gpath_create(&info);
  if (path) {
    graphics_context_set_fill_color(ctx, color);
    gpath_draw_filled(ctx, path);
    gpath_destroy(path);
  }
}

static void face_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  GColor logo = color_logo(s_theme.logo, s_theme.bg);
  GColor bg   = color_bg(s_theme.bg);

  graphics_context_set_fill_color(ctx, bg);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  if (!s_hexagraph) return;

  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  int32_t ha = hour_angle_at(t->tm_hour % 12, t->tm_min);
  int32_t ma = minute_angle_at(t->tm_min);

  GColor hour_color = color_notch(s_theme.hour,   s_theme.bg);
  GColor min_color  = color_notch(s_theme.minute, s_theme.bg);

  GDrawCommandList *cmds = gdraw_command_image_get_command_list(s_hexagraph);

  // Pass 1: outer hexagon only — cutouts and vertex circles painted in
  // logo color, so they overdraw to no visible effect.
  RecolorPass1Ctx rc1 = { .logo = logo };
  gdraw_command_list_iterate(cmds, recolor_pass1_cb, &rc1);
  gdraw_command_image_draw(ctx, s_hexagraph, s_logo_origin);

  // Hour wedge, then minute wedge on top. Both bleed past the hex
  // onto the surrounding bg; the next step trims that bleed.
  draw_wedge(ctx, ha, s_hour_half_width, hour_color);
  draw_wedge(ctx, ma, s_min_half_width,  min_color);

  // Mask: paint bg over everything outside the hex shape, confining
  // the wedges to the hex's footprint.
  draw_outside_hex(ctx, bg);

  // Pass 2: cutouts mask the wedges to the visible petals; vertex
  // circles take the wedge color when the wedge angularly covers them
  // so the bumps participate in the sweep.
  RecolorPass2Ctx rc2 = {
    .logo            = logo,
    .bg              = bg,
    .hour_angle      = ha,
    .hour_half_width = s_hour_half_width,
    .hour_color      = hour_color,
    .min_angle       = ma,
    .min_half_width  = s_min_half_width,
    .min_color       = min_color,
  };
  gdraw_command_list_iterate(cmds, recolor_pass2_cb, &rc2);
  gdraw_command_image_draw(ctx, s_hexagraph, s_logo_origin);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  layer_mark_dirty(s_face_layer);
}

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_hexagraph = gdraw_command_image_create_with_resource(RESOURCE_ID_HEXAGRAPH_PDC);
  if (s_hexagraph) {
    int min_dim = bounds.size.w < bounds.size.h ? bounds.size.w : bounds.size.h;
    int target = min_dim - LOGO_MARGIN * 2;
    ScaleCtx sc = { .num = target, .den = LOGO_VIEWBOX };
    gdraw_command_list_iterate(
      gdraw_command_image_get_command_list(s_hexagraph),
      scale_cb, &sc);
    gdraw_command_image_set_bounds_size(s_hexagraph, GSize(target, target));

    s_logo_origin = (GPoint) {
      (bounds.size.w - target) / 2,
      (bounds.size.h - target) / 2,
    };
    s_logo_center = (GPoint) {
      s_logo_origin.x + target / 2,
      s_logo_origin.y + target / 2,
    };
    // Wedge outer corners sit this far from the logo center — past the
    // screen edge in every direction. (Sum of the two screen dimensions
    // is always > the diagonal, so the wedge corners are always
    // off-screen and the rasterizer clips for free.)
    s_far_distance_px = bounds.size.w + bounds.size.h;

    s_hour_half_width =
        ((int32_t)HOUR_WEDGE_HALF_DEG * TRIG_MAX_ANGLE) / 360;
    s_min_half_width  =
        ((int32_t)MIN_WEDGE_HALF_DEG  * TRIG_MAX_ANGLE) / 360;

    // Read the actual outer-hexagon corners from PDC cmd[0] (the path
    // is in PDC-local coords post-scale_cb; offset by s_logo_origin to
    // get screen coords). This is the source of truth for the hex
    // shape — the mask polygon below uses it to confine wedges to the
    // hex footprint without hand-derived geometry.
    //
    // svg2pdc stores 7 points for the hex path: index 0 is the SVG
    // Move's start (a duplicate of vertex 0), indices 1..6 are the
    // six hex corners. So we skip index 0.
    GDrawCommand *cmd0 = gdraw_command_list_get_command(
        gdraw_command_image_get_command_list(s_hexagraph), 0);
    for (int i = 0; i < 6; i++) {
      GPoint p = gdraw_command_get_point(cmd0, i + 1);
      s_hex_vertices[i] = (GPoint){
        .x = (int16_t)(p.x + s_logo_origin.x),
        .y = (int16_t)(p.y + s_logo_origin.y),
      };
    }
    s_screen_bounds = bounds;
  }

  s_face_layer = layer_create(bounds);
  layer_set_update_proc(s_face_layer, face_update_proc);
  layer_add_child(root, s_face_layer);
}

static void prv_window_unload(Window *window) {
  layer_destroy(s_face_layer);
  if (s_hexagraph) {
    gdraw_command_image_destroy(s_hexagraph);
    s_hexagraph = NULL;
  }
}

static void prv_init(void) {
  theme_load();

  s_window = window_create();
  window_set_background_color(s_window, color_bg(s_theme.bg));
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
}

static void prv_deinit(void) {
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
