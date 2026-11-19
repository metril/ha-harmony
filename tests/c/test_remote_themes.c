/* Runtime themes: every standard screen under each preset and a custom wire theme (golden hashes in
 * tests/vectors/remote_render_themes.json; HARMONY_REGEN_RENDER=1 rewrites it), applying a theme to a built screen
 * equals building it under that theme, 1000 preset switches neither leak nor drift, bad wire input is refused,
 * and the helper maths for palette / free colours. */
#define main render_main_unused
#include "test_remote_render.c"
#undef main

#ifndef HP_THEME_RENDER_JSON
#error "HP_THEME_RENDER_JSON must be defined"
#endif

/* A custom theme: daylight with a purple accent, as the wire form (version, 13 tokens, 12 x fill/ink/pressed; RGB565 BE). */
static size_t custom_wire(uint8_t *b) {
  ra_theme_t t = ra_themes[RA_THEME_DAYLIGHT];
  t.acc = 0x8048C0;
  t.warm = 0x206028;
  const uint32_t tok[13] = {t.bg, t.s1, t.s2, t.s3, t.tx, t.tx2, t.tx3, t.acc, t.onacc, t.warm, t.onwarm, t.bad, t.onbad};
  size_t n = 0;
  b[n++] = RA_WIRE_VERSION;
  for (int i = 0; i < 13; i++) {
    uint16_t v = ra_to565(tok[i]);
    b[n++] = (uint8_t)(v >> 8);
    b[n++] = (uint8_t)v;
  }
  for (int i = 0; i < RA_PALETTE_N; i++) {
    const uint32_t c[3] = {t.palette[i].fill, t.palette[i].ink, t.palette[i].pressed};
    for (int k = 0; k < 3; k++) {
      uint16_t v = ra_to565(c[k]);
      b[n++] = (uint8_t)(v >> 8);
      b[n++] = (uint8_t)v;
    }
  }
  return n;
}

#define NTHEMES 4 /* midnight, daylight, high_contrast, custom */
static const char *const TNAME[NTHEMES] = {"midnight", "daylight", "high_contrast", "custom"};

static void apply_theme(int t) {
  if (t < 3) {
    CT_EQ(ra_theme_apply_preset((uint8_t)(t + 1)), 0);
  } else {
    uint8_t w[RA_WIRE_LEN];
    CT_EQ(custom_wire(w), (size_t)RA_WIRE_LEN);
    CT_EQ(ra_theme_apply_wire(w, sizeof w), 0);
  }
}

static uint64_t shot_hash(void) {
  lv_obj_invalidate(lv_screen_active());
  lv_obj_invalidate(lv_layer_top());
  refresh();
  return fb_hash();
}

static void test_theme_goldens(void) {
  char *txt = slurp(HP_THEME_RENDER_JSON);
  cJSON *gold = txt ? cJSON_Parse(txt) : NULL;
  int regen = getenv("HARMONY_REGEN_RENDER") != NULL;
  if (!regen && !gold) {
    fprintf(stderr, "missing %s; regenerate with HARMONY_REGEN_RENDER=1\n", HP_THEME_RENDER_JSON);
    CT_CHECK(0);
    return;
  }
  FILE *out = regen ? fopen(HP_THEME_RENDER_JSON, "wb") : NULL;
  if (regen) fprintf(out, "{\n \"hash\": \"fnv1a-64 over the 240x320 RGB565 framebuffer; key theme/screen\",\n \"themes\": {\n");
  int n = 0;
  for (int t = 0; t < NTHEMES; t++)
    for (int i = 0; i < NSCREENS; i++) {
      char key[64];
      snprintf(key, sizeof key, "%s/%s", TNAME[t], SCREENS[i].name);
      /* built under the theme */
      fresh_app();
      apply_theme(t);
      memset(fk.fb, 0xA5, sizeof fk.fb);
      SCREENS[i].setup();
      uint64_t built = shot_hash();
      app_destroy(&app);
      /* built under default, then switched */
      fresh_app();
      memset(fk.fb, 0xA5, sizeof fk.fb);
      SCREENS[i].setup();
      shot_hash();
      apply_theme(t);
      uint64_t switched = shot_hash();
      if (built != switched) fprintf(stderr, "%s: switching after build differs from building under the theme\n", key);
      CT_CHECK(built == switched);
      app_destroy(&app);
      char hash[20];
      snprintf(hash, sizeof hash, "%016llx", (unsigned long long)built);
      if (regen) {
        fprintf(out, "%s  \"%s\": \"%s\"", n ? ",\n" : "", key, hash);
      } else {
        const cJSON *g = cJSON_GetObjectItem(cJSON_GetObjectItem(gold, "themes"), key);
        if (!g || strcmp(g->valuestring, hash)) fprintf(stderr, "%s: render hash %s, golden %s\n", key, hash, g ? g->valuestring : "(none)");
        CT_CHECK(g && !strcmp(g->valuestring, hash));
      }
      n++;
    }
  if (regen) {
    fprintf(out, "\n }\n}\n");
    fclose(out);
  } else {
    CT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(gold, "themes")), n);
  }
  cJSON_Delete(gold);
  free(txt);
}

static void test_no_leak_and_reversible(void) {
  fresh_app();
  sc2_home_active();
  ra_ui_toast(&app.ui, "Battery 8%", "Place on the charger");
  uint64_t base = shot_hash();
  for (int i = 1; i < RA_THEME_COUNT; i++) { /* warm up: every style property exists once */
    CT_EQ(ra_theme_apply_preset((uint8_t)i), 0);
    shot_hash();
  }
  CT_EQ(ra_theme_apply_preset(RA_THEME_DEFAULT), 0);
  CT_CHECK(shot_hash() == base);
  lv_mem_monitor_t a, b;
  lv_mem_monitor(&a);
  for (int i = 0; i < 1000; i++) {
    CT_EQ(ra_theme_apply_preset((uint8_t)(i % RA_THEME_COUNT)), 0);
    if (i % 97 == 0) shot_hash();
  }
  CT_EQ(ra_theme_apply_preset(RA_THEME_DEFAULT), 0);
  lv_mem_monitor(&b);
  CT_EQ(b.free_size, a.free_size);
  CT_CHECK(shot_hash() == base);
  app_destroy(&app);
}

static void test_bad_wire_keeps_theme(void) {
  fresh_app();
  CT_EQ(ra_theme_apply_preset(RA_THEME_DAYLIGHT), 0);
  uint8_t w[RA_WIRE_LEN];
  custom_wire(w);
  CT_CHECK(ra_theme_apply_wire(w, sizeof w - 1) < 0);
  CT_CHECK(ra_theme_apply_wire(NULL, sizeof w) < 0);
  w[0] = 9;
  CT_CHECK(ra_theme_apply_wire(w, sizeof w) < 0);
  CT_CHECK(ra_theme_apply_preset(200) < 0);
  CT_EQ(ra_theme_cur()->bg, ra_themes[RA_THEME_DAYLIGHT].bg);
  CT_EQ(ra_theme_cur()->acc, ra_themes[RA_THEME_DAYLIGHT].acc);
  app_destroy(&app);
}

static void test_helpers(void) {
  fresh_app();
  CT_CHECK(ra_theme_palette(0) == NULL && ra_theme_palette(RA_PALETTE_N + 1) == NULL);
  CT_EQ(ra_theme_palette(1)->fill, ra_themes[0].palette[0].fill);
  CT_EQ(ra_theme_palette(RA_PALETTE_N)->pressed, ra_themes[0].palette[RA_PALETTE_N - 1].pressed);
  ra_swatch_t s;
  int outline = -1;
  ra_theme_free_colour(ra_to565(0x20C8B0), &s, &outline); /* bright on the dark ground */
  CT_EQ(s.fill, 0x20C8B0);
  CT_EQ(s.ink, ra_ink(s.fill, ra_theme_cur()->tx, ra_theme_cur()->bg));
  CT_EQ(s.pressed, ra_pressed(s.fill, s.ink));
  CT_EQ(outline, 0);
  ra_theme_free_colour(ra_to565(ra_themes[0].bg), &s, &outline); /* the ground itself needs an outline */
  CT_EQ(outline, 1);
  /* a local swatch lands on a button; re-applying allocates nothing */
  hp_page_t p = home_page();
  render(&p);
  lv_obj_t *o = app.ui.obj[1], *l = app.ui.lbl[1];
  ra_theme_local(o, l, ra_theme_palette(1), 0);
  lv_mem_monitor_t a, b;
  lv_mem_monitor(&a);
  ra_theme_local(o, l, ra_theme_palette(2), 1);
  ra_theme_local(o, l, ra_theme_palette(1), 0);
  lv_mem_monitor(&b);
  CT_EQ(a.free_size, b.free_size);
  CT_EQ(lv_color_to_u32(lv_obj_get_style_bg_color(o, 0)) & 0xFFFFFF, ra_themes[0].palette[0].fill);
  app_destroy(&app);
}

int main(void) {
  CT_RUN(test_theme_goldens);
  CT_RUN(test_no_leak_and_reversible);
  CT_RUN(test_bad_wire_keeps_theme);
  CT_RUN(test_helpers);
  return CT_DONE();
}
