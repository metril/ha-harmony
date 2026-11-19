#ifndef COLOUR_SCREENS_H
#define COLOUR_SCREENS_H

/* Reference coloured v2 screens (WP4): palette and free fills, derived ink, outline, coloured labels and tiles, the
 * active highlight and pressed state. Needs v2_screens.h first. The page's theme id is set through col_theme. */
static int col_theme;

#define COL_PAL(i) ((hp_color_t){HP_COLOR_PALETTE, (i)})
#define COL_RGB(v) ((hp_color_t){HP_COLOR_RGB, (v)})

static __attribute__((unused)) hp_page_t col_page(void) {
  hp_page_t p = v2page(7, 3, 6, "Colours", HP_PAGE_KIND_NORMAL);
  p.theme = (uint8_t)col_theme;
  V2ADD(p, HP_WIDGET_LABEL, 0, 0, 0, 2, 1, "Blue label", HP_ROLE_DEFAULT, HP_SHAPE_CARD, 0, 0, -1);
  V2ADD(p, HP_WIDGET_LABEL, 1, 2, 0, 1, 1, "Yellow", HP_ROLE_DEFAULT, HP_SHAPE_CARD, 0, 0, -1);
  V2ADD(p, HP_WIDGET_BUTTON, 2, 0, 1, 1, 1, "Red", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_POWER, 0, -1);
  V2ADD(p, HP_WIDGET_BUTTON, 3, 1, 1, 1, 1, "Green", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_PLAY, 0, -1);
  V2ADD(p, HP_WIDGET_BUTTON, 4, 2, 1, 1, 1, "Blue", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_TV, 0, -1);
  V2ADD(p, HP_WIDGET_BUTTON, 5, 0, 2, 1, 1, "Sun", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_BULB, 0, -1);
  V2ADD(p, HP_WIDGET_BUTTON, 6, 1, 2, 1, 1, "Ink", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_MUSIC, 0, -1);
  V2ADD(p, HP_WIDGET_BUTTON, 7, 2, 2, 1, 1, "Snow", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_FILM, 0, -1);
  V2ADD(p, HP_WIDGET_BUTTON, 8, 0, 3, 1, 1, "Active", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_TV, 0, 0);
  V2ADD(p, HP_WIDGET_BUTTON, 9, 1, 3, 1, 1, "Pressed", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_GAME, 0, -1);
  V2ADD(p, HP_WIDGET_BUTTON, 10, 2, 3, 1, 1, "Role", HP_ROLE_PRIMARY, HP_SHAPE_CARD, RA_ICON_MENU, 0, -1);
  V2ADD(p, HP_WIDGET_TILE, 11, 0, 4, 1, 1, "Lamp", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_BULB, 0, -1);
  V2ADD(p, HP_WIDGET_TILE, 12, 1, 4, 1, 1, "Plain", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_BULB, 0, -1);
  V2ADD(p, HP_WIDGET_TILE, 13, 2, 4, 1, 1, "Fan", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_FAN, 0, -1);
  V2ADD(p, HP_WIDGET_TILE, 14, 0, 5, 1, 1, "Temp", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_THERMO, 0, -1);
  V2ADD(p, HP_WIDGET_TILE, 15, 1, 5, 1, 1, "Door", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_BLINDS, 0, -1);
  V2ADD(p, HP_WIDGET_TILE, 16, 2, 5, 1, 1, "Light", HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_BULB, 0, -1);
  p.widgets[0].color_fill = COL_PAL(9);
  p.widgets[1].color_fill = COL_PAL(4);
  p.widgets[2].color_fill = COL_PAL(1);
  p.widgets[3].color_fill = COL_PAL(6);
  p.widgets[4].color_fill = COL_PAL(9);
  p.widgets[5].color_fill = COL_RGB(0xFFE0); /* light: the ink flips to dark */
  p.widgets[6].color_fill = COL_RGB(0x0841); /* near black: outline on a black ground */
  p.widgets[7].color_fill = COL_RGB(0xEF7D); /* near white: outline on a light ground */
  p.widgets[8].color_fill = COL_PAL(10);
  p.widgets[9].color_fill = COL_PAL(11);
  p.widgets[11].color_on = COL_PAL(11);
  p.widgets[13].color_fill = COL_PAL(7);
  p.widgets[14].color_fill = COL_PAL(3);
  p.widgets[15].color_fill = COL_PAL(1);
  p.widgets[16].color_on = COL_PAL(12);
  p.widgets[16].color_fill = COL_PAL(7);
  return p;
}

static __attribute__((unused)) void col_states(app_t *a) {
  ra_ui_tile_state(&a->ui, 7, 11, HP_TILE_ON, "On", 2);
  ra_ui_tile_state(&a->ui, 7, 12, HP_TILE_ON, "On", 2);
  ra_ui_tile_state(&a->ui, 7, 13, HP_TILE_OFF, "Off", 3);
  ra_ui_tile_state(&a->ui, 7, 14, HP_TILE_VALUE, "21.5 C", 6);
  ra_ui_tile_state(&a->ui, 7, 15, HP_TILE_UNAVAILABLE, "unavailable", 11);
  ra_ui_tile_state(&a->ui, 7, 16, HP_TILE_ON, "On", 2);
}

static __attribute__((unused)) void col_show(void) {
  hp_page_t p = col_page();
  ra_ui_show_page(&app.ui, &p);
  ra_ui_set_battery(&app.ui, 72, 0);
  col_states(&app);
  ra_ui_set_activity(&app.ui, 0, 1);
}
static __attribute__((unused)) void col_default(void) { col_theme = 0; col_show(); }
static __attribute__((unused)) void col_midnight(void) { col_theme = 1; col_show(); }
static __attribute__((unused)) void col_daylight(void) { col_theme = 2; col_show(); }
static __attribute__((unused)) void col_pressed(void) {
  col_theme = 0;
  col_show();
  ra_ui_press_at(&app.ui, 120, 200); /* "Pressed" */
}
static __attribute__((unused)) void col_stale(void) {
  col_theme = 2;
  col_show();
  ra_ui_set_link(&app.ui, 1);
}
#endif
