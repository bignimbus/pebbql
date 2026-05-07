#include <pebble.h>

// =====================================================================
// Theme: tokens persisted to storage, resolved to GColor at draw time.
// Storing tokens (not raw GColor bytes) lets each platform map a choice
// like "rhodamine" to a sensible fallback on monochrome displays.
//
// All four theme axes (bg, logo, hour, minute) draw from the same set
// of four colors. The Clay config UI hides rhodamine and gray on B&W
// platforms via per-option capabilities, so users can't pick them on
// aplite/diorite — but if a saved persist value somehow contains one
// (e.g., the user moved across watches), color_resolve falls back to
// a contrasting shade rather than crashing.
// =====================================================================

typedef enum {
  THEME_COLOR_RHODAMINE = 0,  // #e10098 on color, contrast on B&W
  THEME_COLOR_WHITE     = 1,  // #ffffff
  THEME_COLOR_BLACK     = 2,  // #000000
  THEME_COLOR_GRAY      = 3,  // #aaaaaa on color, contrast on B&W
} ThemeColor;

typedef struct __attribute__((packed)) {
  uint8_t bg;
  uint8_t logo;
  uint8_t hour;
  uint8_t minute;
} Theme;

#define PERSIST_KEY_THEME 1

static Theme s_theme;

// Background resolves with no contrast logic — it IS the contrast
// reference for everything else. On B&W, ambiguous colors fall back
// to black so the watch still has a defined bg.
static GColor color_bg(ThemeColor t) {
  switch (t) {
    case THEME_COLOR_BLACK:     return GColorBlack;
    case THEME_COLOR_WHITE:     return GColorWhite;
    case THEME_COLOR_RHODAMINE: return PBL_IF_COLOR_ELSE(GColorFashionMagenta, GColorBlack);
    case THEME_COLOR_GRAY:      return PBL_IF_COLOR_ELSE(GColorLightGray,    GColorBlack);
    default:                    return GColorBlack;
  }
}

// Foreground colors (logo, hour, minute) resolve with respect to the
// already-resolved bg, so rhodamine/gray on B&W collapse to whichever
// of black/white contrasts with bg.
static GColor color_themed(ThemeColor t, GColor bg) {
  GColor contrast = gcolor_equal(bg, GColorWhite) ? GColorBlack : GColorWhite;
  switch (t) {
    case THEME_COLOR_BLACK:     return GColorBlack;
    case THEME_COLOR_WHITE:     return GColorWhite;
    case THEME_COLOR_RHODAMINE: return PBL_IF_COLOR_ELSE(GColorFashionMagenta, contrast);
    case THEME_COLOR_GRAY:      return PBL_IF_COLOR_ELSE(GColorLightGray,    contrast);
    default:                    return contrast;
  }
}

static void theme_load(void) {
  s_theme = (Theme) {
    .bg     = THEME_COLOR_BLACK,
    .logo   = THEME_COLOR_RHODAMINE,
    .hour   = THEME_COLOR_WHITE,
    .minute = THEME_COLOR_GRAY,
  };
  if (persist_exists(PERSIST_KEY_THEME)) {
    persist_read_data(PERSIST_KEY_THEME, &s_theme, sizeof(s_theme));
  }
}

static void theme_save(void) {
  persist_write_data(PERSIST_KEY_THEME, &s_theme, sizeof(s_theme));
}

// =====================================================================
// Drawing
// =====================================================================

static Window *s_window;
static Layer  *s_face_layer;
static GDrawCommandImage *s_hexagraph;

#ifdef PBL_PLATFORM_EMERY
static GBitmap *s_stencil;
#endif

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
//   2. PDC pass 1 — hex + bumps painted in logo color
//   3. wedges — drawn over the shape, also bleed past it onto bg
//   4. outside-(hex ∪ bumps) mask — polygon "screen rect with hex-plus-
//      bumps hole" painted in bg, confining the wedges to the visible
//      shape's footprint while preserving the wedge color that flowed
//      over the bumps
//   5. PDC pass 2 — inner-triangle cutouts paint bg over the wedge in
//      cutout areas (revealing the GraphQL pattern); outer hex and
//      vertex circles painted GColorClear so they don't repaint over
//      the wedge color the mask just preserved
//
// The mask hole uses outer-hexagon vertices from PDC cmd[0] and the
// bump radius from cmd[5], so the geometry follows the SVG/PDC source
// of truth without hand-derived constants. Bumps are approximated as
// 8-chord polygons; sub-pixel error at the rendered radius.
// =====================================================================

// Wedge angular half-widths (degrees). Total wedge sweep = 2× this.
//   Hour:  12° wedge. Sweep rate 30°/hour.
//   Min:    6° wedge. Sweep rate 6°/min.
#define HOUR_WEDGE_HALF_DEG   6
#define MIN_WEDGE_HALF_DEG    4

static int     s_far_distance_px;       // wedge outer-corner distance, past screen
static int32_t s_hour_half_width;       // Pebble angle units
static int32_t s_min_half_width;
static GPoint  s_hex_vertices[6];       // outer hexagon corners, screen coords

#define LOGO_MARGIN          12

// PDC command layout, fixed by tools/hexagraph.normalized.svg ordering.
// Pass 1 paints all commands in logo color; pass 2 recolors selectively:
//   [0]      outer hexagon          (pass 2: GColorClear)
//   [1..4]   inner-triangle cutouts (pass 2: bg → creates the cutouts)
//   [5..10]  vertex circles         (pass 2: GColorClear)
#define HEX_PDC_INNER_FIRST   1
#define HEX_PDC_INNER_LAST    4

// Outside-(hex ∪ bumps) mask polygon, pre-built once at window_load.
//   4   screen-rect corners (CW in screen coords)
//   1   close screen rect
//  48   inner-hole boundary: 6 vertices × 9 arc points each (CCW)
//   1   close inner hole
// = 54 points total
#define BUMP_ARC_SEGMENTS    8
#define BUMP_ARC_POINTS      (BUMP_ARC_SEGMENTS + 1)
#define MASK_HOLE_POINTS     (6 * BUMP_ARC_POINTS)
#define MASK_TOTAL_POINTS    (5 + MASK_HOLE_POINTS + 1)
static GPoint s_outside_mask_points[MASK_TOTAL_POINTS];

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

// PDC pass 1: paint everything in the logo color so the visible
// shape — hex with vertex bumps — is laid down as a single solid
// silhouette. Cutouts (inner triangles) overdraw the hex with the
// same color, no visible effect. Wedges drawn after this pass flow
// over the silhouette and pass 2 reveals the cutouts.
typedef struct { GColor logo; } RecolorPass1Ctx;

static bool recolor_pass1_cb(GDrawCommand *cmd, uint32_t index, void *ctx_) {
  RecolorPass1Ctx *ctx = ctx_;
  gdraw_command_set_hidden(cmd, false);
  gdraw_command_set_fill_color(cmd, ctx->logo);
  return true;
}

// PDC pass 2: cutouts paint bg over the wedges drawn between passes,
// clipping them to the visible petals. Outer hex and vertex circles
// are inert — pass 1 painted them in logo color, the wedges drew over
// the parts in their angular range, and the outside-(hex ∪ bumps)
// mask preserves the result. (gdraw_command_set_hidden appears to be
// a no-op for these commands; GColorClear is the reliable suppressor.)
typedef struct { GColor bg; } RecolorPass2Ctx;

static bool recolor_pass2_cb(GDrawCommand *cmd, uint32_t index, void *ctx_) {
  RecolorPass2Ctx *ctx = ctx_;
  if (index >= HEX_PDC_INNER_FIRST && index <= HEX_PDC_INNER_LAST) {
    gdraw_command_set_fill_color(cmd, ctx->bg);
    return true;
  }
  gdraw_command_set_fill_color(cmd, GColorClear);
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

// Paints `color` over everything outside the (hex ∪ bumps) shape.
// Uses the non-zero winding rule to fill between an outer screen-rect
// (CW) and an inner shape-shaped hole (CCW). Hole boundary is
// pre-built in s_outside_mask_points at window_load.
static void draw_outside_hex(GContext *ctx, GColor color) {
  GPathInfo info = {
    .num_points = MASK_TOTAL_POINTS,
    .points     = s_outside_mask_points,
  };
  GPath *path = gpath_create(&info);
  if (path) {
    graphics_context_set_fill_color(ctx, color);
    gpath_draw_filled(ctx, path);
    gpath_destroy(path);
  }
}

static void face_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  GColor bg         = color_bg(s_theme.bg);
  GColor logo       = color_themed(s_theme.logo,   bg);
  GColor hour_color = color_themed(s_theme.hour,   bg);
  GColor min_color  = color_themed(s_theme.minute, bg);

  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  int32_t ha = hour_angle_at(t->tm_hour % 12, t->tm_min);
  int32_t ma = minute_angle_at(t->tm_min);

#ifdef PBL_PLATFORM_EMERY
  if (s_stencil) {
    // Stencil POC pipeline:
    //   1. fill logo color over the entire screen
    //   2. wedges
    //   3. stencil bitmap drawn on top — palette[1] = bg color, so the
    //      negative-space pixels paint bg over wedge bleed and the cutout
    //      regions, while petal pixels (palette[0] = transparent) leave
    //      logo/wedge color showing through.
    graphics_context_set_fill_color(ctx, logo);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    draw_wedge(ctx, ha, s_hour_half_width, hour_color);
    draw_wedge(ctx, ma, s_min_half_width,  min_color);
    GColor *palette = gbitmap_get_palette(s_stencil);
    APP_LOG(APP_LOG_LEVEL_INFO,
            "stencil fmt=%d palette=%p p[0]=0x%02x p[1]=0x%02x",
            gbitmap_get_format(s_stencil), palette,
            palette ? palette[0].argb : 0xff,
            palette ? palette[1].argb : 0xff);
    if (palette) {
      // Find whichever palette index has alpha=0 (the petal regions);
      // leave it transparent. Set the OTHER to bg for the negative
      // space + cutouts. Avoids guessing which index Pebble's PBI
      // converter assigned to which.
      if (palette[0].a == 0) {
        palette[1] = bg;
      } else if (palette[1].a == 0) {
        palette[0] = bg;
      } else {
        // Neither transparent — alpha got stripped during conversion.
        // Fall through; will look wrong but log will reveal cause.
        palette[1] = bg;
      }
    }
    graphics_context_set_compositing_mode(ctx, GCompOpSet);
    graphics_draw_bitmap_in_rect(ctx, s_stencil, bounds);
    return;
  }
#endif

  graphics_context_set_fill_color(ctx, bg);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  if (!s_hexagraph) return;

  GDrawCommandList *cmds = gdraw_command_image_get_command_list(s_hexagraph);

  // Pass 1: paint the whole PDC in logo color. The cutouts overdraw
  // the hex with the same color (no visible effect); the vertex
  // circles produce the visible bumps protruding past the hex edges.
  RecolorPass1Ctx rc1 = { .logo = logo };
  gdraw_command_list_iterate(cmds, recolor_pass1_cb, &rc1);
  gdraw_command_image_draw(ctx, s_hexagraph, s_logo_origin);

  // Hour wedge, then minute wedge on top. Both bleed past the shape
  // onto the surrounding bg; the next step trims that bleed.
  draw_wedge(ctx, ha, s_hour_half_width, hour_color);
  draw_wedge(ctx, ma, s_min_half_width,  min_color);

  // Mask: paint bg outside (hex ∪ bumps), confining the wedges to the
  // shape's footprint while preserving wedge color that flowed onto
  // the bumps.
  draw_outside_hex(ctx, bg);

  // Pass 2: cutouts paint bg, masking the wedges to the visible petals.
  // Outer hex and vertex circles are GColorClear — pass 1 painted them
  // and the mask preserved them along with whatever wedge color flowed
  // over the bumps; repainting now would obliterate that.
  RecolorPass2Ctx rc2 = { .bg = bg };
  gdraw_command_list_iterate(cmds, recolor_pass2_cb, &rc2);
  gdraw_command_image_draw(ctx, s_hexagraph, s_logo_origin);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  layer_mark_dirty(s_face_layer);
}

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

#ifdef PBL_PLATFORM_EMERY
  s_stencil = gbitmap_create_with_resource(RESOURCE_ID_STENCIL);
#endif

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

    // Read the visible shape's geometry from the PDC: outer-hexagon
    // corners from cmd[0], bump centers from cmd[5..10] (one circle
    // per hex vertex), bump radius from cmd[5] (all six share it
    // post-scale_cb). The mask traces around these without any
    // hand-derived constants.
    //
    // svg2pdc stores 7 points for the hex path: index 0 is the SVG
    // Move's start (a duplicate of vertex 0), indices 1..6 are the
    // six hex corners. So we skip index 0.
    //
    // The bump circles are NOT centered on the hex vertices — they're
    // inset inward along the V_i—O bisector by ~2.4 viewbox units in
    // the GraphQL hexagraph. That's why we have to read both.
    GDrawCommandList *cmd_list =
        gdraw_command_image_get_command_list(s_hexagraph);
    GDrawCommand *cmd0 = gdraw_command_list_get_command(cmd_list, 0);
    for (int i = 0; i < 6; i++) {
      GPoint p = gdraw_command_get_point(cmd0, i + 1);
      s_hex_vertices[i] = (GPoint){
        .x = (int16_t)(p.x + s_logo_origin.x),
        .y = (int16_t)(p.y + s_logo_origin.y),
      };
    }
    GPoint bump_centers[6];
    for (int i = 0; i < 6; i++) {
      GDrawCommand *bump_cmd =
          gdraw_command_list_get_command(cmd_list, 5 + i);
      GPoint p = gdraw_command_get_point(bump_cmd, 0);
      bump_centers[i] = (GPoint){
        .x = (int16_t)(p.x + s_logo_origin.x),
        .y = (int16_t)(p.y + s_logo_origin.y),
      };
    }
    GDrawCommand *cmd5 = gdraw_command_list_get_command(cmd_list, 5);
    int32_t bump_r = gdraw_command_get_radius(cmd5);

    // Bump arc's angular half-span — the angle, measured from C_i,
    // between the outward direction (toward V_i) and a hex edge's
    // intersection with the bump perimeter. Same for all six bumps by
    // symmetry; depends only on d/r and the hex interior angle, all
    // fixed by the SVG geometry, so it's a compile-time constant.
    //
    // Derivation: in triangle (C_i, V_i, P_entry) the angle at V_i
    // is 180°-hex_interior = 60°. Law of sines gives
    //   half_span = 120° - asin(d * sin(60°) / r)
    // where d = |V_i - C_i| ≈ 2.42, r = bump radius ≈ 8.82, both in
    // viewbox units. ratio = 0.274·sin(60°) ≈ 0.237; asin(0.237) ≈
    // 13.7°; half_span ≈ 106.3°. In Pebble angle units (full circle
    // = TRIG_MAX_ANGLE = 65536), 106.3° ≈ 19345.
    //
    // Hardcoded rather than computed because Pebble Time 2 (emery)
    // hardware faults inside the SDK's __ieee754_sqrtf, even though
    // the same code runs fine in the emery emulator.
    const int32_t half_span = 19345;
    (void)bump_r;  // still read for future reference but unused here

    // Build the outside-(hex ∪ bumps) mask polygon. Outer screen rect
    // is CW; inner hole traces the visible shape CCW (visiting hex
    // vertices in reverse SVG order: 0, 5, 4, 3, 2, 1). At each V_i
    // the polygon traces the bump's outer arc — centered on outward
    // direction (i·60°), spanning ±half_span — as BUMP_ARC_SEGMENTS
    // chord segments. Implicit straight lines between consecutive
    // vertices' arcs form the hex edges with the bump cut off at each
    // end.
    s_outside_mask_points[0] =
        (GPoint){ bounds.origin.x, bounds.origin.y };
    s_outside_mask_points[1] =
        (GPoint){ bounds.origin.x + bounds.size.w, bounds.origin.y };
    s_outside_mask_points[2] =
        (GPoint){ bounds.origin.x + bounds.size.w,
                  bounds.origin.y + bounds.size.h };
    s_outside_mask_points[3] =
        (GPoint){ bounds.origin.x, bounds.origin.y + bounds.size.h };
    s_outside_mask_points[4] = s_outside_mask_points[0];

    static const int reverse_order[6] = { 0, 5, 4, 3, 2, 1 };
    const int32_t arc_step = (-2 * half_span) / BUMP_ARC_SEGMENTS;
    int idx = 5;
    for (int n = 0; n < 6; n++) {
      int i = reverse_order[n];
      GPoint center = bump_centers[i];
      int32_t outward = ((int32_t)i * TRIG_MAX_ANGLE) / 6;
      int32_t start_angle = outward + half_span;
      for (int k = 0; k < BUMP_ARC_POINTS; k++) {
        int32_t a = start_angle + (int32_t)k * arc_step;
        s_outside_mask_points[idx++] = (GPoint){
          .x = (int16_t)(center.x +
              (sin_lookup(a) * bump_r) / TRIG_MAX_RATIO),
          .y = (int16_t)(center.y -
              (cos_lookup(a) * bump_r) / TRIG_MAX_RATIO),
        };
      }
    }
    s_outside_mask_points[idx] = s_outside_mask_points[5];
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
#ifdef PBL_PLATFORM_EMERY
  if (s_stencil) {
    gbitmap_destroy(s_stencil);
    s_stencil = NULL;
  }
#endif
}

// AppMessage inbox handler — receives a Theme update from the Clay
// config UI, persists it, and re-renders. Each key carries an int
// 0..3 matching the ThemeColor enum values.
static void inbox_received_handler(DictionaryIterator *iter, void *context) {
  Tuple *t;
  if ((t = dict_find(iter, MESSAGE_KEY_BG)))     s_theme.bg     = (uint8_t)t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_LOGO)))   s_theme.logo   = (uint8_t)t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_HOUR)))   s_theme.hour   = (uint8_t)t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_MINUTE))) s_theme.minute = (uint8_t)t->value->int32;
  theme_save();
  if (s_face_layer) layer_mark_dirty(s_face_layer);
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

  app_message_register_inbox_received(inbox_received_handler);
  app_message_open(64, 64);
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
