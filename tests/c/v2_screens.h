#ifndef V2_SCREENS_H
#define V2_SCREENS_H

/* Reference version 2 screens, shared by the render goldens (test_remote_render.c) and the PNG dump
 * (dump_remote_png.c). The including file defines `static app_t app;` first. Pages are built as models (the decoder is
 * covered by test_page.c); every screen is a function that puts the app in the state to hash / photograph. */
#include <string.h>

#include "remote/app/icons.h"

static hp_widget_t v2w(int kind, int id, int x, int y, int w, int h, const char *label, int role, int shape, int icon,
                       int icon_only, int bind) {
  hp_widget_t k;
  memset(&k, 0, sizeof k);
  k.kind = (uint8_t)kind;
  k.id = (uint8_t)id;
  k.x = (uint8_t)x;
  k.y = (uint8_t)y;
  k.w = (uint8_t)w;
  k.h = (uint8_t)h;
  k.role = (uint8_t)role;
  k.shape = (uint8_t)shape;
  k.icon = (uint8_t)icon;
  k.icon_only = (uint8_t)icon_only;
  k.has_bind = bind >= 0;
  k.bind = (uint8_t)(bind < 0 ? 0 : bind);
  if (label) snprintf(k.label, sizeof k.label, "%s", label);
  return k;
}
static hp_page_t v2page(int id, int cols, int rows, const char *title, int kind) {
  hp_page_t p;
  memset(&p, 0, sizeof p);
  p.version = 2;
  p.page_id = (uint8_t)id;
  p.cols = (uint8_t)cols;
  p.rows = (uint8_t)rows;
  p.total = 8;
  p.gen = 1;
  p.kind = (uint8_t)kind;
  if (title) snprintf(p.title, sizeof p.title, "%s", title);
  return p;
}
#define V2ADD(p, ...) ((p).widgets[(p).count++] = v2w(__VA_ARGS__))
#define B HP_WIDGET_BUTTON
#define T HP_WIDGET_TILE

/* Home: four big activity cards (32 px icons, bound to activities 0..3) and a danger pill. */
static __attribute__((unused)) hp_page_t v2_home(void) {
  hp_page_t p = v2page(0, 2, 3, "Activities", HP_PAGE_KIND_HOME);
  V2ADD(p, B, 0, 0, 0, 1, 1, "Watch TV", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_TV, 0, 0);
  V2ADD(p, B, 1, 1, 0, 1, 1, "Music", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_MUSIC, 0, 1);
  V2ADD(p, B, 2, 0, 1, 1, 1, "Games", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_GAME, 0, 2);
  V2ADD(p, B, 3, 1, 1, 1, 1, "Apple TV", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_FILM, 0, 3);
  V2ADD(p, B, 4, 0, 2, 2, 1, "Power Off", HP_ROLE_DANGER, HP_SHAPE_PILL, RA_ICON_POWER, 0, -1);
  return p;
}

/* An activity page: a header row, dense 3-column cards with 24 / 20 px icons, an icon-only strip, one of each role. */
static __attribute__((unused)) hp_page_t v2_activity(void) {
  hp_page_t p = v2page(1, 3, 6, "Watch TV", HP_PAGE_KIND_NORMAL);
  p.act = 1;
  V2ADD(p, B, 0, 0, 0, 3, 1, "BBC One HD", HP_ROLE_SECONDARY, HP_SHAPE_ROW, RA_ICON_TV, 0, -1);
  V2ADD(p, B, 1, 0, 1, 1, 1, "Guide", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_GUIDE, 0, -1);
  V2ADD(p, B, 2, 1, 1, 1, 1, "Menu", HP_ROLE_PRIMARY, HP_SHAPE_CARD, RA_ICON_MENU, 0, -1);
  V2ADD(p, B, 3, 2, 1, 1, 1, "Info", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_INFO, 0, -1);
  V2ADD(p, B, 4, 0, 2, 1, 1, "Netflix", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_APP, 0, -1);
  V2ADD(p, B, 5, 1, 2, 1, 1, "YouTube", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_APP, 0, -1);
  V2ADD(p, B, 6, 2, 2, 1, 1, "Prime", HP_ROLE_SECONDARY, HP_SHAPE_CARD, 99, 0, -1); /* unknown icon: the label still draws */
  V2ADD(p, B, 7, 0, 3, 1, 1, "Vol +", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_VOL_UP, 1, -1);
  V2ADD(p, B, 8, 1, 3, 1, 1, "Mute", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_MUTE, 1, -1);
  V2ADD(p, B, 9, 2, 3, 1, 1, "Ch +", HP_ROLE_DEFAULT, HP_SHAPE_MONOGRAM, 0, 0, -1);
  V2ADD(p, B, 10, 0, 4, 2, 1, "Pause", HP_ROLE_PRIMARY, HP_SHAPE_PILL, RA_ICON_PAUSE, 0, -1);
  V2ADD(p, B, 11, 2, 4, 1, 1, "More", HP_ROLE_NAV, HP_SHAPE_CARD, RA_ICON_CHEV_RIGHT, 0, 2);
  V2ADD(p, B, 12, 0, 5, 3, 1, "Power Off", HP_ROLE_DANGER, HP_SHAPE_PILL, RA_ICON_POWER, 0, -1);
  return p;
}

/* HA entity tiles: on (warm), off, value, unavailable, and a small one. The states are applied by v2_tiles_states(). */
static __attribute__((unused)) hp_page_t v2_tiles(void) {
  hp_page_t p = v2page(2, 2, 3, "Living room", HP_PAGE_KIND_FAVORITES);
  V2ADD(p, T, 0, 0, 0, 1, 1, "Lamp", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_BULB, 0, -1);
  V2ADD(p, T, 1, 1, 0, 1, 1, "Ceiling", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_BULB, 0, -1);
  V2ADD(p, T, 2, 0, 1, 1, 1, "Temperature", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_THERMO, 0, -1);
  V2ADD(p, T, 3, 1, 1, 1, 1, "Fan", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_FAN, 0, -1);
  V2ADD(p, T, 4, 0, 2, 1, 1, "Blinds", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_BLINDS, 0, -1);
  V2ADD(p, T, 5, 1, 2, 1, 1, "", HP_ROLE_DEFAULT, HP_SHAPE_CARD, 0, 0, -1);
  return p;
}
static __attribute__((unused)) void v2_tiles_states(app_t *a) {
  ra_ui_tile_state(&a->ui, 2, 0, HP_TILE_ON, "On", 2);
  ra_ui_tile_state(&a->ui, 2, 1, HP_TILE_OFF, "Off", 3);
  ra_ui_tile_state(&a->ui, 2, 2, HP_TILE_VALUE, "21.5 C", 6);
  ra_ui_tile_state(&a->ui, 2, 3, HP_TILE_UNAVAILABLE, "unavailable", 11);
  ra_ui_tile_state(&a->ui, 2, 4, HP_TILE_VALUE, "60%", 3);
  ra_ui_tile_state(&a->ui, 2, 5, HP_TILE_VALUE, "Locked", 6);
}

/* Page 2 of a paged list (seq 2/3): five full-width rows. */
static __attribute__((unused)) hp_page_t v2_paged(void) {
  hp_page_t p = v2page(3, 1, 5, "Scenes", HP_PAGE_KIND_NORMAL);
  p.seq_idx = 2;
  p.seq_cnt = 3;
  static const char *n[] = {"Movie night", "Dinner", "Reading", "Bedtime", "All lights off"};
  for (int i = 0; i < 5; i++)
    V2ADD(p, B, i, 0, i, 1, 1, n[i], i == 4 ? HP_ROLE_DANGER : HP_ROLE_DEFAULT, HP_SHAPE_ROW, i == 4 ? RA_ICON_POWER : RA_ICON_SCENE, 0, -1);
  return p;
}

/* A BACK page: chevron in the bar, a device's buttons. */
static __attribute__((unused)) hp_page_t v2_back(void) {
  hp_page_t p = v2page(4, 2, 4, "Denon AVR", HP_PAGE_KIND_NORMAL);
  p.back = 1;
  V2ADD(p, B, 0, 0, 0, 2, 1, "Power", HP_ROLE_DANGER, HP_SHAPE_PILL, RA_ICON_POWER, 0, -1);
  V2ADD(p, B, 1, 0, 1, 1, 1, "Input", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_INPUT, 0, -1);
  V2ADD(p, B, 2, 1, 1, 1, 1, "Settings", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_SETTINGS, 0, -1);
  V2ADD(p, B, 3, 0, 2, 1, 1, "Vol -", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_VOL_DOWN, 0, -1);
  V2ADD(p, B, 4, 1, 2, 1, 1, "Vol +", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_VOL_UP, 0, -1);
  V2ADD(p, B, 5, 0, 3, 2, 1, "Back to activities", HP_ROLE_NAV, HP_SHAPE_ROW, RA_ICON_BACK, 0, 0);
  return p;
}

static __attribute__((unused)) void sc2_home_active(void) {
  hp_page_t p = v2_home();
  ra_ui_show_page(&app.ui, &p);
  ra_ui_set_battery(&app.ui, 72, 0);
  ra_ui_set_activity(&app.ui, 0, 1);
}
static __attribute__((unused)) void sc2_home_off(void) {
  hp_page_t p = v2_home();
  ra_ui_show_page(&app.ui, &p);
  ra_ui_set_battery(&app.ui, 72, 0);
}
static __attribute__((unused)) void sc2_activity(void) {
  hp_page_t p = v2_activity();
  ra_ui_show_page(&app.ui, &p);
  ra_ui_set_battery(&app.ui, 72, 0);
}
static __attribute__((unused)) void sc2_tiles(void) {
  hp_page_t p = v2_tiles();
  ra_ui_show_page(&app.ui, &p);
  ra_ui_set_battery(&app.ui, 72, 0);
  v2_tiles_states(&app);
}
static __attribute__((unused)) void sc2_tiles_stale(void) {
  sc2_tiles();
  ra_ui_set_link(&app.ui, 1);
}
static __attribute__((unused)) void sc2_paged(void) {
  hp_page_t p = v2_paged();
  ra_ui_show_page(&app.ui, &p);
  ra_ui_set_battery(&app.ui, 72, 0);
}
static __attribute__((unused)) void sc2_back(void) {
  hp_page_t p = v2_back();
  ra_ui_show_page(&app.ui, &p);
  ra_ui_set_battery(&app.ui, 72, 0);
}
static __attribute__((unused)) void sc2_receiving(void) {
  ra_ui_set_battery(&app.ui, 72, 0);
  ra_ui_show_receiving_n(&app.ui, 12, 40);
}
static __attribute__((unused)) void sc2_pressed(void) {
  sc2_home_active();
  ra_ui_press_at(&app.ui, 180, 60); /* "Music" */
}

#undef B
#undef T
#endif
