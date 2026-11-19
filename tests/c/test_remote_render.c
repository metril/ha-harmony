/* Render every valid golden page and every state screen into the memory framebuffer: structural checks plus a golden hash
 * per page (tests/vectors/remote_render.json). Regenerate deliberately with
 * HARMONY_REGEN_RENDER=1; it is never rewritten silently. */
#include "cJSON.h"
#include "hub/hp_errors.h"
#include "remote_fake.h"

#ifndef HP_PAGES_JSON
#error "HP_PAGES_JSON and HP_RENDER_JSON must be defined"
#endif

static fk_t fk;
static app_t app;

static size_t unhex(const char *s, uint8_t *out, size_t cap) {
  size_t n = strlen(s) / 2;
  if (n > cap) n = cap;
  for (size_t i = 0; i < n; i++) {
    unsigned b;
    sscanf(s + 2 * i, "%2x", &b);
    out[i] = (uint8_t)b;
  }
  return n;
}

static char *slurp(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  char *buf = malloc(1 << 17);
  size_t n = fread(buf, 1, (1 << 17) - 1, f);
  fclose(f);
  buf[n] = 0;
  return buf;
}

static uint64_t fnv1a(const uint8_t *p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

static uint16_t px(int x, int y) {
  const uint8_t *p = fk.fb + (size_t)y * 480 + (size_t)x * 2;
  return (uint16_t)(p[0] | (p[1] << 8));
}

#define RGB565(hex) ((uint16_t)(((((hex) >> 16) & 0xF8) << 8) | ((((hex) >> 8) & 0xFC) << 3) | (((hex) & 0xFF) >> 3)))
#define BG565 RGB565(RA_HEX_BG)

static int in_box(const hp_page_t *p, int x, int y) {
  if (y < HP_PAGE_BAR_H) return 1; /* the status bar is chrome, checked separately */
  if (app.ui.msg && y >= 140 && y < 196) return 1; /* state / empty-page message */
  for (unsigned i = 0; i < p->count; i++) {
    int x1, y1, x2, y2;
    ra_ui_widget_box(p, &p->widgets[i], &x1, &y1, &x2, &y2);
    if (x >= x1 && x <= x2 && y >= y1 && y <= y2) return 1;
  }
  return 0;
}

static int box_has_ink(const hp_page_t *p, const hp_widget_t *w) {
  int x1, y1, x2, y2;
  ra_ui_widget_box(p, w, &x1, &y1, &x2, &y2);
  for (int y = y1; y <= y2; y++)
    for (int x = x1; x <= x2; x++)
      if (px(x, y) != BG565) return 1;
  return 0;
}

static int rect_has_ink(int x1, int y1, int x2, int y2) {
  for (int y = y1; y <= y2; y++)
    for (int x = x1; x <= x2; x++)
      if (px(x, y) != BG565) return 1;
  return 0;
}

static void refresh(void) { lv_refr_now(app.display.disp); }

static void render(const hp_page_t *p) {
  memset(fk.fb, 0xA5, sizeof fk.fb); /* sentinel: untouched pixels would show */
  ra_ui_show_page(&app.ui, p);
  lv_obj_invalidate(lv_screen_active());
  refresh();
}

static void fresh_app(void) {
  fk_init(&fk);
  ra_ops_t ops = fk_ops(&fk);
  CT_EQ(app_init(&app, &ops, &app_link_null, NULL), 0);
}

static void check_structure(const char *name, const hp_page_t *p) {
  /* in the content area nothing outside the widget cells is anything but the background; the whole screen was painted */
  int stray = 0;
  for (int y = 0; y < 320; y++)
    for (int x = 0; x < 240; x++)
      if (!in_box(p, x, y) && px(x, y) != BG565) stray++;
  if (stray) fprintf(stderr, "%s: %d stray pixels outside widget boxes\n", name, stray);
  CT_EQ(stray, 0);
  for (unsigned i = 0; i < p->count; i++) {
    const hp_widget_t *w = &p->widgets[i];
    if ((int)i == app.ui.title_idx) continue; /* drawn in the bar, not in the grid */
    int x1, y1, x2, y2;
    ra_ui_widget_rect(p, w, &x1, &y1, &x2, &y2);
    if (w->kind == HP_WIDGET_LABEL) {
      if (w->label[0] && !box_has_ink(p, w)) fprintf(stderr, "%s: label %u drew nothing\n", name, w->id);
      if (w->label[0]) CT_CHECK(box_has_ink(p, w));
    } else {
      int cx = (x1 + x2) / 2, cy = (y1 + y2) / 2;
      if (px(x1 + 6, cy) == BG565 || px(cx, y1 + 1) == BG565) fprintf(stderr, "%s: widget %u surface missing\n", name, w->id);
      CT_CHECK(px(x1 + 6, cy) != BG565);
      /* margin: the strip between the cell edge and the drawn rect is background */
      int bx1, by1, bx2, by2;
      ra_ui_widget_box(p, w, &bx1, &by1, &bx2, &by2);
      if (x1 > bx1) CT_EQ(px(bx1, cy), BG565);
      (void)cx; (void)by1; (void)by2; (void)bx2; (void)y2;
    }
  }
}


/* ---- hand-built pages (title bar, pressed, truncation) ---- */

static hp_page_t home_page(void) { /* 1 col x 6 rows: title row + 3 activity buttons + a tile */
  hp_page_t p;
  memset(&p, 0, sizeof p);
  p.page_id = 20;
  p.cols = 1;
  p.rows = 6;
  p.count = 5;
  p.widgets[0] = (hp_widget_t){.kind = HP_WIDGET_LABEL, .id = 0, .x = 0, .y = 0, .w = 1, .h = 1, .label = "Activities"};
  p.widgets[1] = (hp_widget_t){.kind = HP_WIDGET_BUTTON, .id = 1, .x = 0, .y = 1, .w = 1, .h = 1, .label = "Watch TV"};
  p.widgets[2] = (hp_widget_t){.kind = HP_WIDGET_BUTTON, .id = 2, .x = 0, .y = 2, .w = 1, .h = 1, .label = "Play Games"};
  p.widgets[3] = (hp_widget_t){.kind = HP_WIDGET_BUTTON, .id = 3, .x = 0, .y = 3, .w = 1, .h = 1, .label = "Power Off"};
  p.widgets[4] = (hp_widget_t){.kind = HP_WIDGET_TILE, .id = 4, .x = 0, .y = 4, .w = 1, .h = 1, .text = "21.5 C"};
  return p;
}

/* A screen "case": a setup that puts the app in a state, then the framebuffer is hashed. */
typedef struct {
  const char *name;
  void (*setup)(void);
} screen_t;

static void sc_boot(void) { ra_ui_show_boot(&app.ui); }
static void sc_receiving(void) { ra_ui_show_receiving(&app.ui); }
static void sc_fallback(void) {
  ra_ui_set_link(&app.ui, 1);
  ra_ui_show_fallback(&app.ui);
}
static void sc_home(void) {
  hp_page_t p = home_page();
  ra_ui_show_page(&app.ui, &p);
  ra_ui_set_battery(&app.ui, 72, 0);
}
static void sc_home_pressed(void) {
  sc_home();
  ra_ui_press_at(&app.ui, 120, 60); /* "Watch TV" */
}
static void sc_home_lowbatt(void) {
  sc_home();
  ra_ui_set_battery(&app.ui, 8, 0);
  ra_ui_toast(&app.ui, "Battery 8%", "Place on the charger");
}
static void sc_home_charging_linkdown(void) {
  sc_home();
  ra_ui_set_battery(&app.ui, 100, 1);
  ra_ui_set_link(&app.ui, 1);
}
#include "v2_screens.h"

static const screen_t SCREENS[] = {
    {"boot", sc_boot},
    {"receiving", sc_receiving},
    {"unreachable", sc_fallback},
    {"home", sc_home},
    {"home_pressed", sc_home_pressed},
    {"home_lowbatt_toast", sc_home_lowbatt},
    {"home_charging_linkdown", sc_home_charging_linkdown},
    {"v2_home_active", sc2_home_active},
    {"v2_home_off", sc2_home_off},
    {"v2_home_pressed", sc2_pressed},
    {"v2_activity", sc2_activity},
    {"v2_tiles", sc2_tiles},
    {"v2_tiles_stale", sc2_tiles_stale},
    {"v2_paged_2_of_3", sc2_paged},
    {"v2_back_page", sc2_back},
    {"v2_receiving_12_of_40", sc2_receiving},
};
#define NSCREENS ((int)(sizeof SCREENS / sizeof SCREENS[0]))

static uint64_t fb_hash(void) { return fnv1a(fk.fb, 480 * 320); }

static void test_goldens(void) {
  char *pages_txt = slurp(HP_PAGES_JSON);
  CT_CHECK(pages_txt != NULL);
  cJSON *pages = cJSON_Parse(pages_txt);
  CT_CHECK(pages != NULL);
  int regen = getenv("HARMONY_REGEN_RENDER") != NULL;
  char *gold_txt = slurp(HP_RENDER_JSON);
  cJSON *gold = gold_txt ? cJSON_Parse(gold_txt) : NULL;
  if (!regen && !gold) {
    fprintf(stderr, "missing/invalid %s; regenerate with HARMONY_REGEN_RENDER=1\n", HP_RENDER_JSON);
    CT_CHECK(0);
    return;
  }
  cJSON *gpages = gold ? cJSON_GetObjectItem(gold, "pages") : NULL;
  cJSON *gscreens = gold ? cJSON_GetObjectItem(gold, "screens") : NULL;
  FILE *out = NULL;
  if (regen) {
    out = fopen(HP_RENDER_JSON, "wb");
    CT_CHECK(out != NULL);
    fprintf(out, "{\n \"hash\": \"fnv1a-64 over the 240x320 RGB565 framebuffer (153600 bytes, little-endian)\",\n \"pages\": {\n");
  }
  const cJSON *v;
  int first = 1, npages = 0;
  for (int set = 0; set < 2; set++)
  cJSON_ArrayForEach(v, cJSON_GetObjectItem(pages, set ? "valid_v2" : "valid")) {
    npages++;
    const char *name = cJSON_GetObjectItem(v, "name")->valuestring;
    uint8_t raw[300];
    size_t n = unhex(cJSON_GetObjectItem(v, "hex")->valuestring, raw, sizeof raw);
    hp_page_t p;
    CT_EQ(hp_page_decode(raw, n, &p), 0);
    fresh_app();
    render(&p);
    check_structure(name, &p);
    { /* pool headroom: even a 32-widget page leaves at least a third of LV_MEM_SIZE free */
      lv_mem_monitor_t mon;
      lv_mem_monitor(&mon);
      CT_CHECK(mon.free_size * 3 >= LV_MEM_SIZE);
    }
    char hash[20];
    snprintf(hash, sizeof hash, "%016llx", (unsigned long long)fb_hash());
    if (regen) {
      fprintf(out, "%s  \"%s\": \"%s\"", first ? "" : ",\n", name, hash);
      first = 0;
    } else {
      const cJSON *g = cJSON_GetObjectItem(gpages, name);
      if (!g || strcmp(g->valuestring, hash)) fprintf(stderr, "%s: render hash %s, golden %s\n", name, hash, g ? g->valuestring : "(none)");
      CT_CHECK(g && !strcmp(g->valuestring, hash));
    }
    app_destroy(&app);
  }
  if (regen) fprintf(out, "\n },\n \"screens\": {\n");
  for (int i = 0; i < NSCREENS; i++) {
    fresh_app();
    memset(fk.fb, 0xA5, sizeof fk.fb);
    SCREENS[i].setup();
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    refresh();
    char hash[20];
    snprintf(hash, sizeof hash, "%016llx", (unsigned long long)fb_hash());
    if (regen) {
      fprintf(out, "%s  \"%s\": \"%s\"", i ? ",\n" : "", SCREENS[i].name, hash);
    } else {
      const cJSON *g = cJSON_GetObjectItem(gscreens, SCREENS[i].name);
      if (!g || strcmp(g->valuestring, hash)) fprintf(stderr, "%s: render hash %s, golden %s\n", SCREENS[i].name, hash, g ? g->valuestring : "(none)");
      CT_CHECK(g && !strcmp(g->valuestring, hash));
    }
    app_destroy(&app);
  }
  if (regen) {
    fprintf(out, "\n }\n}\n");
    fclose(out);
    fprintf(stderr, "regenerated %s\n", HP_RENDER_JSON);
  } else if (gpages) {
    CT_EQ(cJSON_GetArraySize(gpages), npages);
    CT_EQ(cJSON_GetArraySize(gscreens), NSCREENS);
  }
  cJSON_Delete(pages);
  cJSON_Delete(gold);
  free(pages_txt);
  free(gold_txt);
}

static void test_tokens_are_rgb565_exact(void) {
  const uint32_t t[] = {RA_HEX_BG, RA_HEX_S1, RA_HEX_S2, RA_HEX_S3, RA_HEX_TX, RA_HEX_TX2, RA_HEX_TX3,
                               RA_HEX_ACC, RA_HEX_ONACC, RA_HEX_WARM, RA_HEX_ONWARM, RA_HEX_BAD, RA_HEX_ONBAD};
  for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
    CT_EQ((t[i] >> 16) & 7, 0); /* R and B keep 5 bits, G 6: nothing is lost on the panel */
    CT_EQ((t[i] >> 8) & 3, 0);
    CT_EQ(t[i] & 7, 0);
  }
  CT_EQ(RGB565(RA_HEX_BG), 0x10C4); /* matches the mockup's token table */
  CT_EQ(RGB565(RA_HEX_ACC), 0x2656);
  CT_EQ(RGB565(RA_HEX_BAD), 0xFAC9);
}

static void test_title_bar(void) {
  fresh_app();
  hp_page_t p = home_page();
  render(&p);
  CT_EQ(app.ui.title_idx, 0);
  CT_CHECK(!strcmp(lv_label_get_text(app.ui.title), "Activities"));
  CT_CHECK(rect_has_ink(12, 6, 140, 26));  /* title drawn in the bar... */
  CT_EQ(px(100, 31), RGB565(RA_HEX_S2));   /* ...above a 1 px hairline */
  CT_CHECK(app.ui.obj[0] == NULL);         /* no grid widget was built for the title label */
  int x1, y1, x2, y2; /* the grid starts right under the bar: 5 rows of 57.6 px, first button at 40 */
  ra_ui_widget_rect(&app.ui.page, &app.ui.page.widgets[1], &x1, &y1, &x2, &y2);
  CT_EQ(y1, 40);
  CT_EQ(hp_page_row_shift(&app.ui.page), 1);
  CT_CHECK(hp_page_hit(&app.ui.page, 120, 33) == &app.ui.page.widgets[1]);
  /* a page without a full-width row-0 label keeps the app name */
  uint8_t raw[] = {1, 9, 4, 6, 1, 1, 7, 0x00, 0x22, 0};
  hp_page_t q;
  CT_EQ(hp_page_decode(raw, sizeof raw, &q), 0);
  render(&q);
  CT_EQ(app.ui.title_idx, -1);
  CT_CHECK(!strcmp(lv_label_get_text(app.ui.title), "Harmony"));
  app_destroy(&app);
}

static void test_link_and_battery(void) {
  fresh_app();
  hp_page_t p = home_page();
  render(&p);
  /* link up: three grey bars; lost: one short red bar, the taller two gone */
  CT_EQ(px(147, 20), RGB565(RA_HEX_TX2));
  CT_EQ(px(157, 12), RGB565(RA_HEX_TX2));
  ra_ui_set_link(&app.ui, 1);
  refresh();
  CT_EQ(px(147, 20), RGB565(RA_HEX_BAD));
  CT_EQ(px(157, 12), BG565);
  ra_ui_set_link(&app.ui, 0);
  refresh();
  CT_EQ(px(147, 20), RGB565(RA_HEX_TX2));
  CT_EQ(px(157, 12), RGB565(RA_HEX_TX2));
  /* battery unknown: outline only; known: percent and a fill */
  CT_CHECK(!rect_has_ink(160, 6, 205, 26));
  CT_EQ(px(214, 16), BG565);
  ra_ui_set_battery(&app.ui, 80, 0);
  refresh();
  CT_CHECK(!strcmp(lv_label_get_text(app.ui.pct), "80%"));
  CT_CHECK(rect_has_ink(164, 6, 205, 26));
  CT_EQ(px(214, 16), RGB565(RA_HEX_TX2));
  ra_ui_set_battery(&app.ui, 10, 0); /* <= 10%: red */
  refresh();
  CT_EQ(px(210, 16), RGB565(RA_HEX_BAD));
  ra_ui_set_battery(&app.ui, 10, 1); /* on a charger: teal, not red */
  refresh();
  CT_EQ(px(210, 16), RGB565(RA_HEX_ACC));
  ra_ui_set_battery(&app.ui, 100, 0);
  refresh();
  CT_EQ(px(226, 16), RGB565(RA_HEX_TX2));
  /* the bar survives a page change with the same indicators */
  ra_ui_set_link(&app.ui, 1);
  hp_page_t q = home_page();
  q.page_id = 21;
  ra_ui_show_page(&app.ui, &q);
  refresh();
  CT_EQ(px(147, 20), RGB565(RA_HEX_BAD));
  CT_CHECK(!strcmp(lv_label_get_text(app.ui.pct), "100%"));
  app_destroy(&app);
}

static void test_pressed_partial_redraw(void) {
  fresh_app();
  hp_page_t p = home_page();
  render(&p);
  int x1, y1, x2, y2; /* "Play Games" */
  ra_ui_widget_rect(&app.ui.page, &app.ui.page.widgets[2], &x1, &y1, &x2, &y2);
  /* sentinel the whole framebuffer: LVGL may only repaint the pressed widget */
  memset(fk.fb, 0xA5, sizeof fk.fb);
  ra_ui_press_at(&app.ui, 120, (y1 + y2) / 2);
  CT_EQ(app.ui.pressed, 2);
  refresh();
  int outside = 0, inside = 0;
  for (int y = 0; y < 320; y++)
    for (int x = 0; x < 240; x++) {
      int touched = px(x, y) != 0xA5A5;
      if (x >= x1 && x <= x2 && y >= y1 && y <= y2) inside += touched;
      else if (x >= x1 && x <= x2 + 1 && y >= y1 && y <= y2 + 1) continue; /* LVGL invalidates one px past the box */
      else outside += touched;
    }
  if (outside) {
    int bx1 = 999, by1 = 999, bx2 = -1, by2 = -1;
    for (int y = 0; y < 320; y++)
      for (int x = 0; x < 240; x++)
        if (px(x, y) != 0xA5A5 && !(x >= x1 && x <= x2 + 1 && y >= y1 && y <= y2 + 1)) {
          if (x < bx1) bx1 = x;
          if (x > bx2) bx2 = x;
          if (y < by1) by1 = y;
          if (y > by2) by2 = y;
        }
    fprintf(stderr, "pressed redraw touched %d px outside the widget: %d,%d..%d,%d vs %d,%d..%d,%d\n", outside, bx1, by1, bx2, by2, x1, y1, x2, y2);
  }
  CT_EQ(outside, 0);
  CT_CHECK(inside > 0);
  CT_EQ(px(x1 + 20, y1), RGB565(RA_HEX_TX));     /* 2 px white edge */
  CT_EQ(px(x1 + 20, y1 + 5), RGB565(RA_HEX_S3)); /* lighter fill */
  /* sliding to another widget repaints exactly those two */
  int a1, b1, a2, b2;
  ra_ui_widget_rect(&app.ui.page, &app.ui.page.widgets[1], &a1, &b1, &a2, &b2);
  memset(fk.fb, 0xA5, sizeof fk.fb);
  ra_ui_press_at(&app.ui, 120, (b1 + b2) / 2);
  CT_EQ(app.ui.pressed, 1);
  refresh();
  outside = 0;
  for (int y = 0; y < 320; y++)
    for (int x = 0; x < 240; x++) {
      int in1 = x >= x1 && x <= x2 + 1 && y >= y1 && y <= y2 + 1, in2 = x >= a1 && x <= a2 + 1 && y >= b1 && y <= b2 + 1;
      if (!in1 && !in2 && px(x, y) != 0xA5A5) outside++;
    }
  CT_EQ(outside, 0);
  /* the bar presses nothing */
  ra_ui_press_at(&app.ui, 120, 60);
  CT_EQ(app.ui.pressed, 1);
  ra_ui_press_at(&app.ui, 120, 10);
  CT_EQ(app.ui.pressed, -1);
  refresh();
  /* released: back to the idle picture */
  ra_ui_press_at(&app.ui, 120, (y1 + y2) / 2);
  ra_ui_release(&app.ui);
  CT_EQ(app.ui.pressed, -1);
  refresh();
  CT_EQ(px(x1 + 20, y1 + 5), RGB565(RA_HEX_S2));
  CT_EQ(px(x1 + 20, y1), RGB565(RA_HEX_S2));
  app_destroy(&app);
}

static void test_truncation(void) {
  fresh_app();
  hp_page_t p;
  memset(&p, 0, sizeof p);
  p.page_id = 30;
  p.cols = 3;
  p.rows = 6;
  p.count = 2;
  p.widgets[0] = (hp_widget_t){.kind = HP_WIDGET_BUTTON, .id = 1, .x = 0, .y = 1, .w = 1, .h = 1,
                               .label = "Extraordinarily lengthy"};
  p.widgets[1] = (hp_widget_t){.kind = HP_WIDGET_TILE, .id = 2, .x = 1, .y = 1, .w = 1, .h = 1,
                               .text = "A very long tile value indeed"};
  render(&p);
  for (int i = 0; i < 2; i++) {
    int x1, y1, x2, y2;
    ra_ui_widget_rect(&p, &p.widgets[i], &x1, &y1, &x2, &y2);
    uint16_t fill = RGB565(i ? RA_HEX_S1 : RA_HEX_S2);
    int ink_cols = 0;
    /* text is clipped inside the 4 px padding: the left/right strips stay surface-coloured... */
    for (int x = x1 + 4; x <= x2 - 4; x++)
      for (int y = y1 + 4; y <= y2 - 4; y++)
        if (px(x, y) != fill) {
          ink_cols++;
          break;
        }
    CT_CHECK(ink_cols > 10);
    for (int y = y1 + 12; y <= y2 - 12; y++) {
      CT_EQ(px(x1 + 1, y), fill);
      CT_EQ(px(x2 - 1, y), fill);
    }
    /* ...and it is a single line: nothing near the top or bottom edge */
    CT_EQ(px((x1 + x2) / 2, y1 + 2), fill);
    CT_EQ(px((x1 + x2) / 2, y2 - 2), fill);
    CT_CHECK(lv_obj_get_height(app.ui.lbl[i]) <= 24);
  }
  app_destroy(&app);
}

static void test_state_screens(void) {
  fresh_app();
  hp_page_t p = home_page();
  /* boot: connecting */
  CT_EQ(app.ui.page.page_id, RA_UI_PAGE_BOOT);
  refresh();
  CT_CHECK(strstr(lv_label_get_text(app.ui.msg), "Connecting") != NULL);
  CT_CHECK(rect_has_ink(8, 148, 232, 170));
  CT_CHECK(rect_has_ink(12, 6, 140, 26)); /* the bar title is there on every screen */
  /* receiving config */
  ra_ui_show_receiving(&app.ui);
  refresh();
  CT_EQ(app.ui.page.page_id, RA_UI_PAGE_RECEIVING);
  CT_CHECK(strstr(lv_label_get_text(app.ui.msg), "Receiving config") != NULL);
  CT_CHECK(!app.ui.fallback);
  /* hub unreachable: red headline */
  ra_ui_show_fallback(&app.ui);
  refresh();
  CT_CHECK(app.ui.fallback);
  CT_CHECK(!strcmp(lv_label_get_text(app.ui.msg), "Hub unreachable"));
  int red = 0;
  for (int x = 8; x < 232; x++)
    for (int y = 148; y < 172; y++) red += px(x, y) == RGB565(RA_HEX_BAD);
  CT_CHECK(red > 50);
  /* empty page: a message instead of a blank grid */
  uint8_t raw[] = {1, 5, 4, 6, 0};
  hp_page_t e;
  CT_EQ(hp_page_decode(raw, sizeof raw, &e), 0);
  render(&e);
  CT_CHECK(app.ui.msg && strstr(lv_label_get_text(app.ui.msg), "No activities") != NULL);
  CT_CHECK(rect_has_ink(8, 148, 232, 170));
  /* a page with widgets has no such message */
  render(&p);
  CT_CHECK(app.ui.msg == NULL);
  /* a page that only has a title row counts as empty */
  hp_page_t t;
  memset(&t, 0, sizeof t);
  t.page_id = 6;
  t.cols = 1;
  t.rows = 1;
  t.count = 1;
  t.widgets[0] = (hp_widget_t){.kind = HP_WIDGET_LABEL, .id = 0, .w = 1, .h = 1, .label = "Empty"};
  render(&t);
  CT_CHECK(app.ui.msg != NULL);
  app_destroy(&app);
}

static void test_low_battery_toast(void) {
  fresh_app();
  hp_page_t p = home_page();
  render(&p);
  ra_ui_toast(&app.ui, "Battery 8%", "Place on the charger");
  refresh();
  CT_CHECK(app.ui.toast != NULL);
  CT_EQ(px(20, 262), RGB565(RA_HEX_BAD)); /* 224x56 toast, 8 px from the bottom */
  CT_EQ(px(4, 262), BG565);               /* ...and 8 px from the sides */
  CT_EQ(px(100, 31), RGB565(RA_HEX_S2));  /* never covers the top bar */
  /* it survives a page change, and auto-dismisses after RA_TOAST_MS */
  hp_page_t q = home_page();
  q.page_id = 21;
  ra_ui_show_page(&app.ui, &q);
  refresh();
  CT_CHECK(app.ui.toast != NULL);
  fk.now += RA_TOAST_MS - 1;
  lv_timer_handler();
  CT_CHECK(app.ui.toast != NULL);
  fk.now += 2;
  lv_timer_handler();
  CT_CHECK(app.ui.toast == NULL);
  CT_CHECK(app.ui.toast_timer == NULL);
  refresh();
  CT_CHECK(px(20, 262) != RGB565(RA_HEX_BAD));
  /* a replacement or a dismiss on touch also clears it */
  ra_ui_toast(&app.ui, "one", NULL);
  ra_ui_toast(&app.ui, "two", NULL);
  CT_CHECK(app.ui.toast != NULL);
  ra_ui_toast_dismiss(&app.ui);
  CT_CHECK(app.ui.toast == NULL && app.ui.toast_timer == NULL);
  app_destroy(&app);
}

static void test_tile_text(void) {
  fresh_app();
  uint8_t raw[] = {1, 9, 4, 6, 1, 1, 7, 0x00, 0x22, 0};
  hp_page_t p;
  CT_EQ(hp_page_decode(raw, sizeof raw, &p), 0);
  render(&p);
  static uint8_t before[FK_FB_BYTES];
  memcpy(before, fk.fb, sizeof before);
  CT_EQ(ra_ui_tile_text(&app.ui, 9, 7, "21.5 C", 6), 0);
  refresh();
  CT_CHECK(memcmp(before, fk.fb, sizeof before) != 0); /* the text drew */
  CT_CHECK(!strcmp(lv_label_get_text(app.ui.lbl[0]), "21.5 C"));
  CT_EQ(ra_ui_tile_text(&app.ui, 9, 99, "x", 1), -HP_E_NOENT); /* no such tile */
  CT_EQ(ra_ui_tile_text(&app.ui, 8, 7, "x", 1), -HP_E_NOENT);  /* not the page on screen */
  CT_CHECK(!strcmp(lv_label_get_text(app.ui.lbl[0]), "21.5 C"));
  app_destroy(&app);
}

int main(void) {
  CT_RUN(test_tokens_are_rgb565_exact);
  CT_RUN(test_goldens);
  CT_RUN(test_title_bar);
  CT_RUN(test_link_and_battery);
  CT_RUN(test_pressed_partial_redraw);
  CT_RUN(test_truncation);
  CT_RUN(test_state_screens);
  CT_RUN(test_low_battery_toast);
  CT_RUN(test_tile_text);
  return CT_DONE();
}
