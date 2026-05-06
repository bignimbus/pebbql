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
static int    s_logo_radius;            // center to hexagon corner, display px

// Vertex circle geometry, matching the SVG (radius 8.82, centers at distance
// 40.68 from origin in a 100-viewbox).
static int    s_pdc_vertex_radius_px;
static int    s_pdc_vertex_dist_px;

// Notch dimensions in display px.
static int    s_hour_half_t;
static int    s_min_half_t;
static int    s_notch_full_radial;

#define LOGO_VIEWBOX        100
// Distance from logo center to hexagon corner, in viewbox permil. The
// SVG corners at (50, 6.9) → 43.1 from center (50, 50).
#define LOGO_VERTEX_PERMIL  431
// SVG vertex circles: r=8.82, centers at 40.68 from viewbox center (50, 50).
#define VERTEX_RADIUS_PERMIL  88
#define VERTEX_DIST_PERMIL   407

// Notch sizes in permil of target. Tangential cap of ~74 permil keeps the
// rect inside the vertex circle when the perim sits at a corner — beyond
// that the rect spills into the bg outside the logo. Radial cap of ~42
// permil keeps the rect inside the ring (which is ~4.2 viewbox units thick
// at the thinnest spot), so it never reaches the inner-triangle cutouts.
#define HOUR_HALF_T_PERMIL    70
#define MIN_HALF_T_PERMIL     55
#define NOTCH_FULL_R_PERMIL   35

// Glow-trigger threshold: the rect "expands" to fill the vertex circle only
// when the perim is essentially AT a vertex (~2 px). The minute hand moves
// ~6.7 px/min on chalk so this fires only at exact 10-min ticks; the hour
// hand moves ~0.56 px/min so it fires for a few minutes around each even
// hour. Anywhere else the notch reads as a plain rectangle.
#define VERTEX_NEAR_PX         2

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

// PDC recoloring: outer hexagon and vertex circles take the logo color;
// inner triangles take the bg color so they "cut out" of the hexagon by
// drawing the bg on top, replicating the SVG's evenodd fill rule via
// pure layering.
typedef struct { GColor logo; GColor bg; } RecolorCtx;

static bool recolor_cb(GDrawCommand *cmd, uint32_t index, void *ctx_) {
  RecolorCtx *ctx = ctx_;
  bool is_cutout = (index >= HEX_PDC_INNER_FIRST && index <= HEX_PDC_INNER_LAST);
  gdraw_command_set_fill_color(cmd, is_cutout ? ctx->bg : ctx->logo);
  return true;
}

static GPoint hex_vertex(int i) {
  int32_t angle = (i * TRIG_MAX_ANGLE) / 6;
  return (GPoint) {
    .x = s_logo_center.x + (int16_t)((sin_lookup(angle) * s_logo_radius) / TRIG_MAX_RATIO),
    .y = s_logo_center.y - (int16_t)((cos_lookup(angle) * s_logo_radius) / TRIG_MAX_RATIO),
  };
}

// Vertex circle centers sit slightly inward of the hexagon corners — the
// "bumps" in the hexagraph. Anchors the vertex glow to the PDC circles so
// it overlays them exactly.
static GPoint pdc_vertex_center(int i) {
  int32_t angle = (i * TRIG_MAX_ANGLE) / 6;
  return (GPoint) {
    .x = s_logo_center.x + (int16_t)((sin_lookup(angle) * s_pdc_vertex_dist_px) / TRIG_MAX_RATIO),
    .y = s_logo_center.y - (int16_t)((cos_lookup(angle) * s_pdc_vertex_dist_px) / TRIG_MAX_RATIO),
  };
}

// Position on hexagon perimeter at fraction = num/den, walking clockwise
// from the top vertex. Reports the side index (0..5) for orientation, plus
// the (t, den) along that side for proximity checks.
typedef struct {
  GPoint  pos;
  int     side;
  int32_t t;
  int32_t den;
} PerimResult;

static PerimResult perim_at(int32_t num, int32_t den) {
  int32_t scaled = num * 6;
  int side = scaled / den;
  if (side >= 6) side = 5;
  int32_t t = scaled - (int32_t)side * den;

  GPoint a = hex_vertex(side);
  GPoint b = hex_vertex((side + 1) % 6);
  return (PerimResult) {
    .pos = (GPoint) {
      .x = a.x + (int16_t)(((int32_t)(b.x - a.x) * t) / den),
      .y = a.y + (int16_t)(((int32_t)(b.y - a.y) * t) / den),
    },
    .side = side,
    .t    = t,
    .den  = den,
  };
}

// If perim is within `near_dist_px` of either endpoint of its side, returns
// the vertex index (0..5); else -1. Edge length == s_logo_radius for a
// regular hexagon, so along-edge distance scales linearly with t.
static int near_vertex(const PerimResult *p, int near_dist_px) {
  int32_t dist_to_start = (p->t * s_logo_radius) / p->den;
  if (dist_to_start < near_dist_px) return p->side;
  int32_t dist_to_end = ((p->den - p->t) * s_logo_radius) / p->den;
  if (dist_to_end < near_dist_px) return (p->side + 1) % 6;
  return -1;
}

// Draw a rectangle aligned to the local hexagon edge, with one long side
// flush against the perimeter (extending only inward). Fixed-point tangent
// + inward-normal vectors; edge length == s_logo_radius for a regular
// hexagon, so we sidestep sqrt.
static void draw_notch_rect(GContext *ctx, GPoint perim, int side,
                            int half_t, int full_r, GColor color) {
  GPoint a = hex_vertex(side);
  GPoint b = hex_vertex((side + 1) % 6);

  int32_t tx = ((int32_t)(b.x - a.x) * 256) / s_logo_radius;
  int32_t ty = ((int32_t)(b.y - a.y) * 256) / s_logo_radius;
  int32_t nx = -ty;
  int32_t ny = tx;
  // Flip if the normal happened to point outward (away from logo center).
  int32_t to_center_x = s_logo_center.x - perim.x;
  int32_t to_center_y = s_logo_center.y - perim.y;
  if (nx * to_center_x + ny * to_center_y < 0) {
    nx = -nx;
    ny = -ny;
  }

  GPoint corners[4] = {
    { (int16_t)(perim.x + ((tx * (-half_t)) / 256)),
      (int16_t)(perim.y + ((ty * (-half_t)) / 256)) },
    { (int16_t)(perim.x + ((tx *  half_t)  / 256)),
      (int16_t)(perim.y + ((ty *  half_t)  / 256)) },
    { (int16_t)(perim.x + ((tx *  half_t  + nx * full_r) / 256)),
      (int16_t)(perim.y + ((ty *  half_t  + ny * full_r) / 256)) },
    { (int16_t)(perim.x + ((tx * (-half_t) + nx * full_r) / 256)),
      (int16_t)(perim.y + ((ty * (-half_t) + ny * full_r) / 256)) },
  };

  GPathInfo info = { .num_points = 4, .points = corners };
  GPath *path = gpath_create(&info);
  if (path) {
    graphics_context_set_fill_color(ctx, color);
    gpath_draw_filled(ctx, path);
    gpath_destroy(path);
  }
}

static void draw_time_markers(GContext *ctx, struct tm *t, GColor bg) {
  int hour12 = t->tm_hour % 12;
  int minute = t->tm_min;

  // Hour notch creeps smoothly between hour positions as minutes pass:
  // fraction = (h*60 + m) / (12*60).
  PerimResult hour_p = perim_at(hour12 * 60 + minute, 720);
  PerimResult min_p  = perim_at(minute, 60);

  GColor hour_color   = color_notch(s_theme.hour,   s_theme.bg);
  GColor minute_color = color_notch(s_theme.minute, s_theme.bg);

  // Glow only when the perim is essentially AT a vertex — see comment on
  // VERTEX_NEAR_PX. Most of the time both hands read as rectangles.
  int hv = near_vertex(&hour_p, VERTEX_NEAR_PX);
  int mv = near_vertex(&min_p,  VERTEX_NEAR_PX);

  // Rectangles, layered hour-then-minute. A bg halo around the minute keeps
  // the two notches distinct when they overlap (especially on monochrome).
  draw_notch_rect(ctx, hour_p.pos, hour_p.side,
                  s_hour_half_t, s_notch_full_radial, hour_color);
  draw_notch_rect(ctx, min_p.pos, min_p.side,
                  s_min_half_t + 1, s_notch_full_radial + 1, bg);
  draw_notch_rect(ctx, min_p.pos, min_p.side,
                  s_min_half_t, s_notch_full_radial, minute_color);

  // Vertex glows: notch color fills the vertex circle bump when the hand
  // lands at an exact tick. Minute halo only when sharing a corner with
  // the hour, so isolated minute glows don't get a bg ring.
  if (hv >= 0) {
    graphics_context_set_fill_color(ctx, hour_color);
    graphics_fill_circle(ctx, pdc_vertex_center(hv), s_pdc_vertex_radius_px);
  }
  if (mv >= 0) {
    if (hv == mv) {
      graphics_context_set_fill_color(ctx, bg);
      graphics_fill_circle(ctx, pdc_vertex_center(mv), s_pdc_vertex_radius_px + 1);
    }
    graphics_context_set_fill_color(ctx, minute_color);
    graphics_fill_circle(ctx, pdc_vertex_center(mv), s_pdc_vertex_radius_px);
  }
}

static void face_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  GColor logo_color = color_logo(s_theme.logo, s_theme.bg);
  GColor bg         = color_bg(s_theme.bg);

  graphics_context_set_fill_color(ctx, bg);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  if (s_hexagraph) {
    RecolorCtx rc = { .logo = logo_color, .bg = bg };
    gdraw_command_list_iterate(
      gdraw_command_image_get_command_list(s_hexagraph), recolor_cb, &rc);
    gdraw_command_image_draw(ctx, s_hexagraph, s_logo_origin);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    draw_time_markers(ctx, t, bg);
  }
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
    s_logo_radius          = (target * LOGO_VERTEX_PERMIL)  / 1000;
    s_pdc_vertex_radius_px = (target * VERTEX_RADIUS_PERMIL) / 1000;
    s_pdc_vertex_dist_px   = (target * VERTEX_DIST_PERMIL)   / 1000;
    s_hour_half_t          = (target * HOUR_HALF_T_PERMIL)   / 1000;
    s_min_half_t           = (target * MIN_HALF_T_PERMIL)    / 1000;
    s_notch_full_radial    = (target * NOTCH_FULL_R_PERMIL)  / 1000;
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
