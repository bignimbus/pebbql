#include <pebble.h>

// =====================================================================
// Theme: a single dark/light toggle, persisted as a uint8_t.
//
//   dark   = black bg, white hands w/ black border, white logo (mono)
//   light  = white bg, black hands w/ white border, black logo (mono)
//
// On color platforms the logo is always rhodamine; the toggle still
// drives bg + hand colors. On B&W (aplite, diorite) rhodamine has no
// rendering, so the logo collapses to whichever shade contrasts with bg.
// =====================================================================

typedef struct __attribute__((packed)) {
  uint8_t dark;
} Theme;

#define PERSIST_KEY_THEME 1

static Theme s_theme;

static void theme_load(void) {
  s_theme = (Theme) { .dark = 1 };
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

static GPoint s_logo_origin;
static GPoint s_logo_center;
static int    s_clock_radius;     // half the rendered logo size, in px

#define LOGO_VIEWBOX        100
#define LOGO_MARGIN          12

// Hand geometry. Lengths are fractions of s_clock_radius so platforms
// with different screen sizes look proportional. Tweak freely.
#define HOUR_LENGTH_NUM      65      // 65% of clock radius
#define HOUR_LENGTH_DEN     100
#define HOUR_HALF_WIDTH       6      // px

#define MIN_LENGTH_NUM       90
#define MIN_LENGTH_DEN      100
#define MIN_HALF_WIDTH        6      // px

#define HAND_BORDER_WIDTH     2      // stroke width for hand + pin outline

#define CENTER_PIN_RADIUS     7      // px

// PDC scaling: walk every command, scale points + circle radii.
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

// PDC layout (fixed by tools/hexagraph.normalized.svg):
//   [0]      outer hexagon          → logo color
//   [1..4]   inner-triangle cutouts → bg (replicates SVG evenodd by layering)
//   [5..10]  vertex circles         → logo color
#define HEX_PDC_INNER_FIRST   1
#define HEX_PDC_INNER_LAST    4

typedef struct { GColor logo; GColor bg; } RecolorCtx;

static bool recolor_cb(GDrawCommand *cmd, uint32_t index, void *ctx_) {
  RecolorCtx *c = ctx_;
  if (index >= HEX_PDC_INNER_FIRST && index <= HEX_PDC_INNER_LAST) {
    gdraw_command_set_fill_color(cmd, c->bg);
  } else {
    gdraw_command_set_fill_color(cmd, c->logo);
  }
  return true;
}

static int32_t hour_angle_at(int hour12, int minute) {
  return ((int32_t)(hour12 * 60 + minute) * TRIG_MAX_ANGLE) / (12 * 60);
}

static int32_t minute_angle_at(int minute) {
  return ((int32_t)minute * TRIG_MAX_ANGLE) / 60;
}

// Build a hand polygon: a rectangle from s_logo_center extending outward
// by `length`, half_width thick on each side, rotated to `angle`.
// Pebble angle convention: 0 = up (12 o'clock), increases CW.
static void hand_polygon(int32_t angle, int length, int half_width,
                         GPoint out[4]) {
  int32_t s = sin_lookup(angle);
  int32_t c = cos_lookup(angle);
  int16_t fx = (int16_t)((s * length) / TRIG_MAX_RATIO);
  int16_t fy = (int16_t)((-c * length) / TRIG_MAX_RATIO);
  int16_t sx = (int16_t)((c * half_width) / TRIG_MAX_RATIO);
  int16_t sy = (int16_t)((s * half_width) / TRIG_MAX_RATIO);
  out[0] = (GPoint){ s_logo_center.x - sx,      s_logo_center.y - sy      };
  out[1] = (GPoint){ s_logo_center.x + sx,      s_logo_center.y + sy      };
  out[2] = (GPoint){ s_logo_center.x + sx + fx, s_logo_center.y + sy + fy };
  out[3] = (GPoint){ s_logo_center.x - sx + fx, s_logo_center.y - sy + fy };
}

// Draw a hand as a "stadium" shape: rectangle body + filled circle at the
// tip for a rounded end. Border is two filled passes — wider in border
// color, narrower in fill color — which gives a clean continuous outline
// (no rect-outline meets circle-outline seam). The base is hidden by the
// center pin, so only the tip needs rounding.
static void draw_hand(GContext *ctx, int32_t angle, int length, int half_width,
                      GColor fill, GColor border) {
  GPoint tip = {
    s_logo_center.x + (int16_t)((sin_lookup(angle) * length) / TRIG_MAX_RATIO),
    s_logo_center.y - (int16_t)((cos_lookup(angle) * length) / TRIG_MAX_RATIO),
  };
  int outer_hw = half_width + HAND_BORDER_WIDTH;

  GPoint outer[4];
  hand_polygon(angle, length, outer_hw, outer);
  GPathInfo info_outer = { .num_points = 4, .points = outer };
  GPath *p_outer = gpath_create(&info_outer);
  graphics_context_set_fill_color(ctx, border);
  if (p_outer) {
    gpath_draw_filled(ctx, p_outer);
    gpath_destroy(p_outer);
  }
  graphics_fill_circle(ctx, tip, outer_hw);

  GPoint inner[4];
  hand_polygon(angle, length, half_width, inner);
  GPathInfo info_inner = { .num_points = 4, .points = inner };
  GPath *p_inner = gpath_create(&info_inner);
  graphics_context_set_fill_color(ctx, fill);
  if (p_inner) {
    gpath_draw_filled(ctx, p_inner);
    gpath_destroy(p_inner);
  }
  graphics_fill_circle(ctx, tip, half_width);
}

static void face_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  bool dark = s_theme.dark != 0;

  GColor bg     = dark ? GColorBlack : GColorWhite;
  GColor logo   = PBL_IF_COLOR_ELSE(GColorFashionMagenta,
                                    dark ? GColorWhite : GColorBlack);
  GColor fill   = dark ? GColorWhite : GColorBlack;
  GColor border = dark ? GColorBlack : GColorWhite;

  graphics_context_set_fill_color(ctx, bg);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  if (s_hexagraph) {
    GDrawCommandList *cmds = gdraw_command_image_get_command_list(s_hexagraph);
    RecolorCtx rc = { .logo = logo, .bg = bg };
    gdraw_command_list_iterate(cmds, recolor_cb, &rc);
    gdraw_command_image_draw(ctx, s_hexagraph, s_logo_origin);
  }

  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  int32_t ha = hour_angle_at(t->tm_hour % 12, t->tm_min);
  int32_t ma = minute_angle_at(t->tm_min);

  int hour_length = (s_clock_radius * HOUR_LENGTH_NUM) / HOUR_LENGTH_DEN;
  int min_length  = (s_clock_radius * MIN_LENGTH_NUM)  / MIN_LENGTH_DEN;

  // Minute first, then hour on top.
  draw_hand(ctx, ma, min_length,  MIN_HALF_WIDTH,  fill, border);
  draw_hand(ctx, ha, hour_length, HOUR_HALF_WIDTH, fill, border);

  // Pin: same two-pass construction as the hands for a uniform border.
  graphics_context_set_fill_color(ctx, border);
  graphics_fill_circle(ctx, s_logo_center, CENTER_PIN_RADIUS + HAND_BORDER_WIDTH);
  graphics_context_set_fill_color(ctx, fill);
  graphics_fill_circle(ctx, s_logo_center, CENTER_PIN_RADIUS);
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
    s_clock_radius = target / 2;
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

static void inbox_received_handler(DictionaryIterator *iter, void *context) {
  Tuple *t = dict_find(iter, MESSAGE_KEY_DARK);
  if (t) s_theme.dark = (uint8_t)t->value->int32;
  theme_save();
  if (s_face_layer) layer_mark_dirty(s_face_layer);
}

static void prv_init(void) {
  theme_load();

  s_window = window_create();
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
