/* Widget colours and the page theme (WP4): palette / free fills with derived ink, the automatic outline, coloured labels
 * and tiles (state wins), the active highlight, pressed, per-page theme switching, theme id 7 with and without a custom
 * theme. Golden hashes in tests/vectors/remote_render_colors.json (HARMONY_REGEN_RENDER=1 rewrites it). */
#define main render_main_unused
#include "test_remote_render.c"
#undef main

#ifndef HP_COLORS_JSON
#error "HP_COLORS_JSON must be defined"
#endif

#include "colour_screens.h"

static const struct {
  const char *name;
  void (*fn)(void);
} CSC[] = {{"default", col_default}, {"midnight", col_midnight}, {"daylight", col_daylight},
           {"pressed", col_pressed}, {"stale_daylight", col_stale}};
#define NCSC ((int)(sizeof CSC / sizeof CSC[0]))

static uint64_t shot_fb(void) {
  lv_obj_invalidate(lv_screen_active());
  lv_obj_invalidate(lv_layer_top());
  refresh();
  return fb_hash();
}

static void test_col_goldens(void) {
  char *txt = slurp(HP_COLORS_JSON);
  cJSON *gold = txt ? cJSON_Parse(txt) : NULL;
  int regen = getenv("HARMONY_REGEN_RENDER") != NULL;
  if (!regen && !gold) {
    fprintf(stderr, "missing %s; regenerate with HARMONY_REGEN_RENDER=1\n", HP_COLORS_JSON);
    CT_CHECK(0);
    return;
  }
  FILE *out = regen ? fopen(HP_COLORS_JSON, "wb") : NULL;
  if (regen) fprintf(out, "{\n \"hash\": \"fnv1a-64 over the 240x320 RGB565 framebuffer; coloured reference screens\",\n \"screens\": {\n");
  for (int i = 0; i < NCSC; i++) {
    fresh_app();
    memset(fk.fb, 0xA5, sizeof fk.fb);
    CSC[i].fn();
    char hash[20];
    snprintf(hash, sizeof hash, "%016llx", (unsigned long long)shot_fb());
    if (regen) fprintf(out, "%s  \"%s\": \"%s\"", i ? ",\n" : "", CSC[i].name, hash);
    else {
      const cJSON *g = cJSON_GetObjectItem(cJSON_GetObjectItem(gold, "screens"), CSC[i].name);
      if (!g || strcmp(g->valuestring, hash)) fprintf(stderr, "%s: hash %s, golden %s\n", CSC[i].name, hash, g ? g->valuestring : "(none)");
      CT_CHECK(g && !strcmp(g->valuestring, hash));
    }
    app_destroy(&app);
  }
  if (regen) {
    fprintf(out, "\n }\n}\n");
    fclose(out);
  } else {
    CT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(gold, "screens")), NCSC);
  }
  cJSON_Delete(gold);
  free(txt);
}

static void cell(const hp_page_t *p, int id, int *x1, int *y1, int *x2, int *y2) {
  for (unsigned i = 0; i < p->count; i++)
    if (p->widgets[i].id == id) return ra_ui_widget_rect(p, &p->widgets[i], x1, y1, x2, y2);
  CT_CHECK(0);
}
static uint16_t c565(uint32_t rgb) { return ra_to565(rgb); }

/* Pixel-level rules under one preset. */
static void check_rules(int theme) {
  const ra_theme_t *t = &ra_themes[theme];
  col_theme = theme;
  hp_page_t p = col_page();
  fresh_app();
  col_show();
  refresh();
  int x1, y1, x2, y2;
  /* palette fill on a button: surface = the swatch fill, no outline */
  cell(&p, 2, &x1, &y1, &x2, &y2);
  CT_EQ(px(x1 + 6, (y1 + y2) / 2), c565(t->palette[0].fill));
  /* a free colour keeps its (RGB565) fill; bright yellow takes the dark ink */
  ra_swatch_t sw;
  int ol;
  ra_theme_free_colour(0xFFE0, &sw, &ol);
  cell(&p, 5, &x1, &y1, &x2, &y2);
  CT_EQ(px(x1 + 6, (y1 + y2) / 2), 0xFFE0);
  CT_EQ(sw.ink, theme == RA_THEME_DAYLIGHT ? t->tx : t->bg);
  /* near-black / near-white: the 1 px TX3 outline exactly where the fill is too close to the ground */
  ra_theme_free_colour(0x0841, &sw, &ol);
  cell(&p, 6, &x1, &y1, &x2, &y2);
  CT_EQ(px(x1, (y1 + y2) / 2) == c565(t->tx3), ol);
  if (theme == RA_THEME_MIDNIGHT) CT_EQ(ol, 1);
  ra_theme_free_colour(0xEF7D, &sw, &ol);
  cell(&p, 7, &x1, &y1, &x2, &y2);
  CT_EQ(px(x1, (y1 + y2) / 2) == c565(t->tx3), ol);
  if (theme == RA_THEME_DAYLIGHT) CT_EQ(ol, 1);
  /* active highlight: the fill stays, the 3 px border is the accent */
  cell(&p, 8, &x1, &y1, &x2, &y2);
  CT_EQ(px(x1 + 6, (y1 + y2) / 2), c565(t->palette[9].fill));
  CT_EQ(px(x1 + 1, (y1 + y2) / 2), c565(t->acc));
  CT_EQ(px(x1 + 2, (y1 + y2) / 2), c565(t->acc));
  /* tiles: on with color_on, on without (warm), off with a fill, value with a fill, unavailable ignores the fill */
  cell(&p, 11, &x1, &y1, &x2, &y2);
  CT_EQ(px(x1 + 6, (y1 + y2) / 2), c565(t->palette[10].fill));
  cell(&p, 12, &x1, &y1, &x2, &y2);
  CT_EQ(px(x1 + 6, (y1 + y2) / 2), c565(t->warm));
  cell(&p, 13, &x1, &y1, &x2, &y2);
  CT_EQ(px(x1 + 6, (y1 + y2) / 2), c565(t->palette[6].fill));
  cell(&p, 14, &x1, &y1, &x2, &y2);
  CT_EQ(px(x1 + 6, (y1 + y2) / 2), c565(t->palette[2].fill));
  cell(&p, 15, &x1, &y1, &x2, &y2);
  CT_EQ(px(x1 + 6, (y1 + y2) / 2), c565(t->s1));
  /* a role button without a colour keeps its default look */
  cell(&p, 10, &x1, &y1, &x2, &y2);
  CT_EQ(px(x1 + 6, (y1 + y2) / 2), c565(t->s1));
  /* stale: coloured off tiles fall back to the theme's state colours, an ON tile keeps its colour */
  ra_ui_set_link(&app.ui, 1);
  refresh();
  cell(&p, 13, &x1, &y1, &x2, &y2);
  CT_EQ(px(x1 + 6, (y1 + y2) / 2), c565(t->s1));
  cell(&p, 16, &x1, &y1, &x2, &y2);
  CT_EQ(px(x1 + 6, (y1 + y2) / 2), c565(t->palette[11].fill));
  ra_ui_set_link(&app.ui, 0);
  /* pressed: the swatch's pressed fill */
  ra_ui_press_at(&app.ui, 120, 200);
  refresh();
  cell(&p, 9, &x1, &y1, &x2, &y2);
  CT_EQ(px(x1 + 8, (y1 + y2) / 2), c565(t->palette[10].pressed));
  ra_ui_release(&app.ui);
  app_destroy(&app);
}

static void test_rules(void) {
  check_rules(RA_THEME_DEFAULT);
  check_rules(RA_THEME_MIDNIGHT);
  check_rules(RA_THEME_DAYLIGHT);
}

/* The active border is TX when the fill is the accent itself. */
static void test_active_on_accent(void) {
  fresh_app();
  const ra_theme_t *t = &ra_themes[0];
  col_theme = 0;
  hp_page_t p = col_page();
  p.widgets[8].color_fill = COL_RGB(c565(t->acc));
  render(&p);
  ra_ui_set_activity(&app.ui, 0, 1);
  refresh();
  int x1, y1, x2, y2;
  cell(&p, 8, &x1, &y1, &x2, &y2);
  CT_EQ(px(x1 + 6, (y1 + y2) / 2), c565(t->acc));
  CT_EQ(px(x1 + 1, (y1 + y2) / 2), c565(t->tx));
  app_destroy(&app);
}

/* A coloured label takes its colour when it reaches 4.5:1 on the ground, else TX. */
static void test_label_colour(void) {
  for (int th = 0; th < 3; th++) {
    const ra_theme_t *t = &ra_themes[th];
    col_theme = th;
    fresh_app();
    hp_page_t p = col_page();
    render(&p);
    for (int k = 0; k < 2; k++) {
      uint32_t f = t->palette[k ? 3 : 8].fill;
      uint32_t want = ra_meets(f, t->bg, 9, 2) ? f : t->tx;
      lv_color_t c = lv_obj_get_style_text_color(app.ui.lbl[k], 0);
      CT_EQ(lv_color_to_u32(c) & 0xFFFFFF, want);
    }
    app_destroy(&app);
  }
}

static uint64_t show_hash(int theme) {
  col_theme = theme;
  hp_page_t p = col_page();
  ra_ui_show_page(&app.ui, &p);
  col_states(&app);
  ra_ui_set_activity(&app.ui, 0, 1);
  return shot_fb();
}

/* page -> page: each page's preset applies, theme 0 brings the default back, same hashes as built fresh */
static void test_page_to_page(void) {
  uint64_t want[3];
  for (int th = 0; th < 3; th++) {
    fresh_app();
    want[th] = show_hash(th);
    app_destroy(&app);
  }
  fresh_app();
  int order[] = {1, 2, 0, 2, 1, 1, 0};
  for (unsigned i = 0; i < sizeof order / sizeof *order; i++) {
    CT_CHECK(show_hash(order[i]) == want[order[i]]);
    CT_EQ(ra_theme_cur()->id, order[i]);
  }
  /* system chrome follows: the connecting screen keeps the page's theme */
  show_hash(1);
  ra_ui_show_boot(&app.ui);
  CT_EQ(ra_theme_cur()->id, RA_THEME_MIDNIGHT);
  app_destroy(&app);
}

static size_t custom_wire_bytes(uint8_t *w) {
  ra_theme_t t = ra_themes[RA_THEME_DAYLIGHT];
  t.acc = 0x8048C0;
  const uint32_t tok[13] = {t.bg, t.s1, t.s2, t.s3, t.tx, t.tx2, t.tx3, t.acc, t.onacc, t.warm, t.onwarm, t.bad, t.onbad};
  size_t n = 0;
  w[n++] = RA_WIRE_VERSION;
  for (int i = 0; i < 13; i++) {
    uint16_t v = ra_to565(tok[i]);
    w[n++] = (uint8_t)(v >> 8);
    w[n++] = (uint8_t)v;
  }
  for (int i = 0; i < RA_PALETTE_N; i++) {
    const uint32_t c[3] = {t.palette[i].fill, t.palette[i].ink, t.palette[i].pressed};
    for (int k = 0; k < 3; k++) {
      uint16_t v = ra_to565(c[k]);
      w[n++] = (uint8_t)(v >> 8);
      w[n++] = (uint8_t)v;
    }
  }
  return n;
}

static void test_custom_and_unknown(void) {
  fresh_app();
  uint64_t def = show_hash(0);
  app_destroy(&app);
  /* 7 without a custom theme: the default preset, no crash */
  fresh_app();
  show_hash(2);
  CT_CHECK(show_hash(HP_PAGE_THEME_CUSTOM) == def);
  CT_EQ(ra_theme_cur()->id, RA_THEME_DEFAULT);
  /* unknown preset ids (4..6) fall back to the default too */
  show_hash(2);
  CT_CHECK(show_hash(5) == def);
  /* bad custom themes are refused */
  uint8_t w[RA_WIRE_LEN] = {0};
  CT_CHECK(ra_ui_set_custom_theme(w, sizeof w - 1) < 0);
  CT_CHECK(ra_ui_set_custom_theme(w, sizeof w) < 0); /* version 0 */
  CT_CHECK(ra_ui_set_custom_theme(NULL, 0) < 0);
  CT_EQ(custom_wire_bytes(w), (size_t)RA_WIRE_LEN);
  /* set while a page asks for 7: applies at once; later 7-pages use it, other pages leave it */
  show_hash(HP_PAGE_THEME_CUSTOM);
  uint64_t before = shot_fb();
  CT_EQ(ra_ui_set_custom_theme(w, sizeof w), 0);
  CT_EQ(ra_theme_cur()->id, RA_THEME_CUSTOM_ID);
  CT_CHECK(shot_fb() != before);
  show_hash(0);
  CT_EQ(ra_theme_cur()->id, RA_THEME_DEFAULT);
  show_hash(HP_PAGE_THEME_CUSTOM);
  CT_EQ(ra_theme_cur()->id, RA_THEME_CUSTOM_ID);
  CT_EQ(ra_theme_cur()->acc, ra_from565(ra_to565(0x8048C0)));
  app_destroy(&app);
}

static void test_no_leak(void) {
  fresh_app();
  show_hash(0);
  show_hash(1);
  show_hash(2);
  lv_mem_monitor_t a, b;
  lv_mem_monitor(&a);
  for (int i = 0; i < 60; i++) show_hash(i % 3);
  show_hash(2);
  lv_mem_monitor(&b);
  CT_EQ(a.free_size, b.free_size);
  app_destroy(&app);
}

int main(void) {
  CT_RUN(test_col_goldens);
  CT_RUN(test_rules);
  CT_RUN(test_active_on_accent);
  CT_RUN(test_label_colour);
  CT_RUN(test_page_to_page);
  CT_RUN(test_custom_and_unknown);
  CT_RUN(test_no_leak);
  return CT_DONE();
}
