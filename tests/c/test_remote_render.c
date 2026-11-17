/* Render every valid golden page into the memory framebuffer: structural checks plus a golden hash
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

/* RA_UI_BG (0x101418) as RGB565. */
#define BG565 ((uint16_t)(((0x10 >> 3) << 11) | ((0x14 >> 2) << 5) | (0x18 >> 3)))

static int in_box(const hp_page_t *p, int x, int y) {
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

static void render(const hp_page_t *p) {
  memset(fk.fb, 0xA5, sizeof fk.fb); /* sentinel: untouched pixels would show */
  ra_ui_show_page(&app.ui, p);
  lv_obj_invalidate(lv_screen_active());
  lv_refr_now(app.display.disp);
}

static void check_structure(const char *name, const hp_page_t *p) {
  /* nothing outside the widgets is anything but the background; the whole screen was painted */
  int stray = 0;
  for (int y = 0; y < 320; y++)
    for (int x = 0; x < 240; x++)
      if (!in_box(p, x, y) && px(x, y) != BG565) stray++;
  if (stray) fprintf(stderr, "%s: %d stray pixels outside widget boxes\n", name, stray);
  CT_EQ(stray, 0);
  for (unsigned i = 0; i < p->count; i++) {
    const hp_widget_t *w = &p->widgets[i];
    int x1, y1, x2, y2;
    ra_ui_widget_box(p, w, &x1, &y1, &x2, &y2);
    if (w->kind == HP_WIDGET_LABEL) {
      if (w->label[0] && !box_has_ink(p, w)) fprintf(stderr, "%s: label %u drew nothing\n", name, w->id);
      if (w->label[0]) CT_CHECK(box_has_ink(p, w));
    } else {
      int cx = (x1 + x2) / 2, cy = (y1 + y2) / 2;
      if (px(cx, cy) == BG565) fprintf(stderr, "%s: widget %u centre is background\n", name, w->id);
      CT_CHECK(px(cx, cy) != BG565);
    }
  }
}

static void test_pages(void) {
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
  FILE *out = NULL;
  if (regen) {
    out = fopen(HP_RENDER_JSON, "wb");
    CT_CHECK(out != NULL);
    fprintf(out, "{\n \"hash\": \"fnv1a-64 over the 240x320 RGB565 framebuffer (153600 bytes, little-endian)\",\n \"pages\": {\n");
  }
  const cJSON *v;
  int first = 1;
  cJSON_ArrayForEach(v, cJSON_GetObjectItem(pages, "valid")) {
    const char *name = cJSON_GetObjectItem(v, "name")->valuestring;
    uint8_t raw[300];
    size_t n = unhex(cJSON_GetObjectItem(v, "hex")->valuestring, raw, sizeof raw);
    hp_page_t p;
    CT_EQ(hp_page_decode(raw, n, &p), 0);

    fk_init(&fk);
    ra_ops_t ops = fk_ops(&fk);
    CT_EQ(app_init(&app, &ops, &app_link_null, NULL), 0);
    render(&p);
    check_structure(name, &p);
    char hash[20];
    snprintf(hash, sizeof hash, "%016llx", (unsigned long long)fnv1a(fk.fb, 480 * 320));
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
  if (regen) {
    fprintf(out, "\n }\n}\n");
    fclose(out);
    fprintf(stderr, "regenerated %s\n", HP_RENDER_JSON);
  } else if (gpages) {
    CT_EQ(cJSON_GetArraySize(gpages), cJSON_GetArraySize(cJSON_GetObjectItem(pages, "valid")));
  }
  cJSON_Delete(pages);
  cJSON_Delete(gold);
  free(pages_txt);
  free(gold_txt);
}

static void test_tile_text_and_builtin_pages(void) {
  fk_init(&fk);
  ra_ops_t ops = fk_ops(&fk);
  CT_EQ(app_init(&app, &ops, &app_link_null, NULL), 0);
  /* mixed-style page: a tile with live text */
  uint8_t raw[] = {1, 9, 4, 6, 1, 1, 7, 0x00, 0x22, 0};
  hp_page_t p;
  CT_EQ(hp_page_decode(raw, sizeof raw, &p), 0);
  render(&p);
  static uint8_t before[FK_FB_BYTES];
  memcpy(before, fk.fb, sizeof before);
  CT_EQ(ra_ui_tile_text(&app.ui, 9, 7, "21.5 C", 6), 0);
  lv_refr_now(app.display.disp);
  CT_CHECK(memcmp(before, fk.fb, sizeof before) != 0); /* the text drew */
  CT_CHECK(!strcmp(lv_label_get_text(app.ui.lbl[0]), "21.5 C"));
  CT_EQ(ra_ui_tile_text(&app.ui, 9, 99, "x", 1), -HP_E_NOENT); /* no such tile */
  CT_EQ(ra_ui_tile_text(&app.ui, 8, 7, "x", 1), -HP_E_NOENT);  /* not the page on screen */
  CT_CHECK(!strcmp(lv_label_get_text(app.ui.lbl[0]), "21.5 C"));

  /* built-in boot and fallback pages are real, drawn pages */
  ra_ui_show_boot(&app.ui);
  lv_refr_now(app.display.disp);
  CT_EQ(app.ui.page.page_id, RA_UI_PAGE_BOOT);
  CT_CHECK(box_has_ink(&app.ui.page, &app.ui.page.widgets[0]));
  ra_ui_show_fallback(&app.ui);
  lv_refr_now(app.display.disp);
  CT_EQ(app.ui.page.page_id, RA_UI_PAGE_FALLBACK);
  CT_CHECK(!strcmp(lv_label_get_text(app.ui.lbl[0]), "Hub unreachable"));
  CT_CHECK(box_has_ink(&app.ui.page, &app.ui.page.widgets[0]));
  app_destroy(&app);
}

int main(void) {
  CT_RUN(test_pages);
  CT_RUN(test_tile_text_and_builtin_pages);
  return CT_DONE();
}
