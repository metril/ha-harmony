/* Host tool: render the remote UI's reference screens through the real app into the memory
 * framebuffer and write them as PNGs (uncompressed deflate; no zlib needed).
 *   build/tests/c/dump_remote_png <outdir>
 * writes home, activity, device, tiles, unreachable, connecting, receiving, empty, lowbatt, pressed. */
#include <sys/stat.h>

#include "remote_fake.h"

static fk_t fk;
static __attribute__((unused)) int ct_unused(void) { return ct_failures; }
static app_t app;

static uint32_t crc_table[256];
static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c;
}
static void be32(uint8_t *o, uint32_t v) {
  o[0] = (uint8_t)(v >> 24); o[1] = (uint8_t)(v >> 16); o[2] = (uint8_t)(v >> 8); o[3] = (uint8_t)v;
}
static void chunk(FILE *f, const char *type, const uint8_t *d, size_t n) {
  uint8_t h[8];
  be32(h, (uint32_t)n);
  memcpy(h + 4, type, 4);
  fwrite(h, 1, 8, f);
  if (n) fwrite(d, 1, n, f);
  uint32_t c = crc32_update(0xFFFFFFFFu, (const uint8_t *)type, 4);
  if (n) c = crc32_update(c, d, n);
  uint8_t t[4];
  be32(t, c ^ 0xFFFFFFFFu);
  fwrite(t, 1, 4, f);
}

/* RGB565 framebuffer -> 8-bit RGB by shifting (the design tokens are exact, so #RRGGBB comes back unchanged). */
static int write_png(const char *path) {
  enum { W = 240, H = 320, ROW = 1 + W * 3 };
  static uint8_t raw[H * ROW];
  for (int y = 0; y < H; y++) {
    uint8_t *r = raw + (size_t)y * ROW;
    *r++ = 0;
    for (int x = 0; x < W; x++) {
      uint16_t v = (uint16_t)(fk.fb[y * 480 + x * 2] | (fk.fb[y * 480 + x * 2 + 1] << 8));
      *r++ = (uint8_t)(((v >> 11) & 31) << 3);
      *r++ = (uint8_t)(((v >> 5) & 63) << 2);
      *r++ = (uint8_t)((v & 31) << 3);
    }
  }
  size_t n = sizeof raw, nblk = (n + 65534) / 65535;
  static uint8_t z[sizeof raw + 16 + 5 * 8];
  size_t zo = 0;
  z[zo++] = 0x78;
  z[zo++] = 0x01;
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < n; i++) {
    a = (a + raw[i]) % 65521;
    b = (b + a) % 65521;
  }
  for (size_t k = 0, off = 0; k < nblk; k++) {
    size_t len = n - off > 65535 ? 65535 : n - off;
    z[zo++] = k + 1 == nblk;
    z[zo++] = (uint8_t)len; z[zo++] = (uint8_t)(len >> 8);
    z[zo++] = (uint8_t)~len; z[zo++] = (uint8_t)(~len >> 8);
    memcpy(z + zo, raw + off, len);
    zo += len;
    off += len;
  }
  be32(z + zo, (b << 16) | a);
  zo += 4;
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  uint8_t ihdr[13] = {0, 0, 0, W, 0, 0, 0x01, 0x40, 8, 2, 0, 0, 0};
  fwrite(sig, 1, 8, f);
  chunk(f, "IHDR", ihdr, sizeof ihdr);
  chunk(f, "IDAT", z, zo);
  chunk(f, "IEND", NULL, 0);
  return fclose(f);
}

static hp_widget_t wd(int kind, int id, int x, int y, int w, int h, const char *label, const char *text) {
  hp_widget_t k;
  memset(&k, 0, sizeof k);
  k.kind = (uint8_t)kind; k.id = (uint8_t)id; k.x = (uint8_t)x; k.y = (uint8_t)y; k.w = (uint8_t)w; k.h = (uint8_t)h;
  if (label) snprintf(k.label, sizeof k.label, "%s", label);
  if (text) snprintf(k.text, sizeof k.text, "%s", text);
  return k;
}

static hp_page_t page(int id, int cols, int rows) {
  hp_page_t p;
  memset(&p, 0, sizeof p);
  p.page_id = (uint8_t)id; p.cols = (uint8_t)cols; p.rows = (uint8_t)rows;
  return p;
}
#define ADD(p, ...) ((p).widgets[(p).count++] = wd(__VA_ARGS__))

static hp_page_t home_page(void) {
  hp_page_t p = page(1, 1, 6);
  ADD(p, HP_WIDGET_LABEL, 0, 0, 0, 1, 1, "Activities", NULL);
  ADD(p, HP_WIDGET_BUTTON, 1, 0, 1, 1, 1, "Watch TV", NULL);
  ADD(p, HP_WIDGET_BUTTON, 2, 0, 2, 1, 1, "Listen to Music", NULL);
  ADD(p, HP_WIDGET_BUTTON, 3, 0, 3, 1, 1, "Play Games", NULL);
  ADD(p, HP_WIDGET_BUTTON, 4, 0, 4, 1, 1, "Watch a Movie", NULL);
  ADD(p, HP_WIDGET_BUTTON, 5, 0, 5, 1, 1, "Power Off", NULL);
  return p;
}
static hp_page_t activity_page(void) {
  hp_page_t p = page(2, 3, 6);
  static const char *n[] = {"Vol +", "Mute", "Ch +", "Vol -", "Guide", "Ch -", "Back", "Subtitles", "Info"};
  ADD(p, HP_WIDGET_LABEL, 0, 0, 0, 3, 1, "Watch TV", NULL);
  for (int i = 0; i < 9; i++) ADD(p, HP_WIDGET_BUTTON, 1 + i, i % 3, 1 + i / 3, 1, 1, n[i], NULL);
  ADD(p, HP_WIDGET_BUTTON, 10, 0, 4, 3, 1, "Pause", NULL);
  ADD(p, HP_WIDGET_TILE, 11, 0, 5, 3, 1, NULL, "Sky Q - Netflix");
  return p;
}
static hp_page_t device_page(void) {
  hp_page_t p = page(3, 1, 6);
  static const char *n[] = {"Power", "Input", "Menu", "Settings", "Sleep timer"};
  ADD(p, HP_WIDGET_LABEL, 0, 0, 0, 1, 1, "Living room TV", NULL);
  for (int i = 0; i < 5; i++) ADD(p, HP_WIDGET_BUTTON, 1 + i, 0, 1 + i, 1, 1, n[i], NULL);
  return p;
}
static hp_page_t tiles_page(void) {
  hp_page_t p = page(4, 2, 4);
  static const char *t[] = {"Lamp  On", "Ceiling  Off", "Blinds  60%", "21.5 C", "Fan  Low", "Door  Locked"};
  ADD(p, HP_WIDGET_LABEL, 0, 0, 0, 2, 1, "Living room", NULL);
  for (int i = 0; i < 6; i++) ADD(p, HP_WIDGET_TILE, 1 + i, i % 2, 1 + i / 2, 1, 1, NULL, t[i]);
  return p;
}
static hp_page_t empty_page(void) { return page(5, 4, 6); }

static void fresh(void) {
  fk_init(&fk);
  ra_ops_t ops = fk_ops(&fk);
  if (app_init(&app, &ops, &app_link_null, NULL) != 0) {
    fprintf(stderr, "app_init failed\n");
    exit(1);
  }
}

static void shot(const char *dir, const char *name) {
  char path[512];
  lv_obj_invalidate(lv_screen_active());
  lv_obj_invalidate(lv_layer_top());
  lv_refr_now(app.display.disp);
  snprintf(path, sizeof path, "%s/%s.png", dir, name);
  if (write_png(path) != 0) {
    fprintf(stderr, "cannot write %s\n", path);
    exit(1);
  }
  printf("%s\n", path);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <outdir>\n", argv[0]);
    return 2;
  }
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crc_table[i] = c;
  }
  mkdir(argv[1], 0777);
  const char *d = argv[1];
  hp_page_t p;
  fresh();
  p = home_page(); ra_ui_show_page(&app.ui, &p); ra_ui_set_battery(&app.ui, 72, 0); shot(d, "home");
  ra_ui_press_at(&app.ui, 120, 60); shot(d, "pressed");
  p = activity_page(); ra_ui_show_page(&app.ui, &p); shot(d, "activity");
  p = device_page(); ra_ui_show_page(&app.ui, &p); shot(d, "device");
  p = tiles_page(); ra_ui_show_page(&app.ui, &p); shot(d, "tiles");
  p = empty_page(); ra_ui_show_page(&app.ui, &p); shot(d, "empty");
  p = home_page(); ra_ui_show_page(&app.ui, &p); ra_ui_set_battery(&app.ui, 8, 0);
  ra_ui_toast(&app.ui, "Battery 8%", "Place on the charger"); shot(d, "lowbatt");
  ra_ui_toast_dismiss(&app.ui);
  ra_ui_set_link(&app.ui, 1); ra_ui_show_fallback(&app.ui); shot(d, "unreachable");
  ra_ui_set_link(&app.ui, 0); ra_ui_show_boot(&app.ui); shot(d, "connecting");
  ra_ui_show_receiving(&app.ui); shot(d, "receiving");
  app_destroy(&app);
  return 0;
}
