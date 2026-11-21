/* The remote app on page format v2: page store fill and "Receiving page k of N", home selection, new gen, re-render only
 * when the visible page is replaced, activity pages and power-off Home, local navigation (nav widgets, paged Up/Down,
 * Home / Favorites, Exit on BACK pages, the back chevron), and what is and is not forwarded to the hub. */
#include "remote_fake.h"

static fk_t fk;
static app_t app;
static fk_link_t lk;

static void start(void) {
  fk_init(&fk);
  memset(&lk, 0, sizeof lk);
  ra_ops_t ops = fk_ops(&fk);
  app_link_t l = fkl_link(&lk);
  CT_EQ(app_init(&app, &ops, &l, NULL), 0);
}

static void step(void) {
  fk.now += 40;
  app_step(&app, 0);
}
static void settle(void) {
  for (int i = 0; i < 3; i++) step();
}

/* ---- a tiny v2 encoder: header + widgets ---- */
typedef struct {
  uint8_t kind, id, x, y, w, h, role, shape, icon, only;
  int bind; /* -1 none */
  const char *label;
} ws_t;

static size_t enc(uint8_t *o, int id, int total, int gen, int kind, int back, int idx, int cnt, int act, const char *title,
                  int cols, int rows, const ws_t *w, int n) {
  size_t k = 0, tl = title ? strlen(title) : 0;
  o[k++] = 2; o[k++] = (uint8_t)id; o[k++] = (uint8_t)cols; o[k++] = (uint8_t)rows; o[k++] = (uint8_t)n;
  o[k++] = (uint8_t)gen; o[k++] = (uint8_t)total; o[k++] = (uint8_t)(kind | (back ? 4 : 0));
  o[k++] = (uint8_t)((idx << 4) | cnt); o[k++] = (uint8_t)act; o[k++] = (uint8_t)tl;
  if (tl) memcpy(o + k, title, tl);
  k += tl;
  for (int i = 0; i < n; i++) {
    uint8_t style = (uint8_t)(w[i].role | (w[i].shape << 3) | (w[i].only ? 0x20 : 0));
    uint8_t kb = (uint8_t)(w[i].kind | (style ? 0x10 : 0) | (w[i].icon ? 0x20 : 0) | (w[i].bind >= 0 ? 0x40 : 0));
    o[k++] = kb; o[k++] = w[i].id; o[k++] = (uint8_t)((w[i].x << 4) | w[i].y); o[k++] = (uint8_t)((w[i].w << 4) | w[i].h);
    if (style) o[k++] = style;
    if (w[i].icon) o[k++] = w[i].icon;
    if (w[i].bind >= 0) o[k++] = (uint8_t)w[i].bind;
    size_t ll = strlen(w[i].label);
    o[k++] = (uint8_t)ll;
    memcpy(o + k, w[i].label, ll);
    k += ll;
  }
  return k;
}

static void push(int id, int total, int gen, int kind, int back, int idx, int cnt, int act, const char *title, const ws_t *w,
                 int n) {
  uint8_t raw[HP_PAGE_MAX_BYTES];
  size_t len = enc(raw, id, total, gen, kind, back, idx, cnt, act, title, 2, 3, w, n);
  CT_CHECK(len <= HP_PAGE_MAX_BYTES);
  fkl_push_page(&lk, raw, len);
}
static void state(int activity, int power) {
  app_link_event_t *e = &lk.q[lk.nq++];
  memset(e, 0, sizeof *e);
  e->kind = APP_LINK_EV_STATE_UPDATE;
  e->activity = (uint8_t)activity;
  e->power = power != 0;
}
static void tilestate(int page, int tile, int st, const char *text) {
  app_link_event_t *e = &lk.q[lk.nq++];
  memset(e, 0, sizeof *e);
  e->kind = APP_LINK_EV_TILE_STATE;
  e->page = (uint8_t)page;
  e->tile = (uint8_t)tile;
  e->state = (uint8_t)st;
  snprintf(e->text, sizeof e->text, "%s", text);
}

static const ws_t BTN = {HP_WIDGET_BUTTON, 1, 0, 0, 2, 1, HP_ROLE_DEFAULT, HP_SHAPE_CARD, RA_ICON_TV, 0, 0, "Go"};
static ws_t one(int id, int role, int icon, int bind, const char *label) {
  ws_t w = BTN;
  w.id = (uint8_t)id;
  w.role = (uint8_t)role;
  w.icon = (uint8_t)icon;
  w.bind = bind;
  w.label = label;
  return w;
}

#define N 5 /* pages in the sets below: 0 activity page (act 1), 1 home, 2 favorites, 3..4 paged group 1/2 2/2 */
static void push_set(int gen, int order_rev) {
  ws_t act0[] = {one(1, HP_ROLE_DEFAULT, RA_ICON_TV, 0, "Watch"), one(2, HP_ROLE_NAV, RA_ICON_BACK, 1, "Back")};
  act0[1].y = 1;
  ws_t home[] = {one(1, HP_ROLE_DEFAULT, RA_ICON_TV, 0, "TV"), one(2, HP_ROLE_NAV, RA_ICON_CHEV_RIGHT, 3, "Scenes")};
  home[1].y = 1;
  ws_t fav[] = {one(1, HP_ROLE_DEFAULT, RA_ICON_STAR, -1, "Fav")};
  ws_t p3[] = {one(1, HP_ROLE_DEFAULT, 0, -1, "One")};
  ws_t p4[] = {one(1, HP_ROLE_DEFAULT, 0, -1, "Two")};
  int ids[N] = {0, 1, 2, 3, 4};
  if (order_rev)
    for (int i = 0; i < N / 2; i++) { int t = ids[i]; ids[i] = ids[N - 1 - i]; ids[N - 1 - i] = t; }
  for (int i = 0; i < N; i++) {
    switch (ids[i]) {
    case 0: push(0, N, gen, HP_PAGE_KIND_NORMAL, 1, 0, 0, 1, "Watch TV", act0, 2); break; /* BACK, act 1 */
    case 1: push(1, N, gen, HP_PAGE_KIND_HOME, 0, 0, 0, 0, "Home", home, 2); break;
    case 2: push(2, N, gen, HP_PAGE_KIND_FAVORITES, 0, 0, 0, 0, "Favorites", fav, 1); break;
    case 3: push(3, N, gen, HP_PAGE_KIND_NORMAL, 0, 1, 2, 0, "Scenes", p3, 1); break;
    default: push(4, N, gen, HP_PAGE_KIND_NORMAL, 0, 2, 2, 0, "Scenes", p4, 1); break;
    }
  }
}

static const char *msg_text(void) { return app.ui.msg ? lv_label_get_text(app.ui.msg) : ""; }
static const char *cap_text(void) { return app.ui.cap ? lv_label_get_text(app.ui.cap) : ""; }
static int shown(void) { return app.ui.page.version == 2 ? app.ui.page.page_id : -1; }

static void tap_widget(int id) {
  const hp_widget_t *w = NULL;
  for (unsigned i = 0; i < app.ui.page.count; i++)
    if (app.ui.page.widgets[i].id == id) w = &app.ui.page.widgets[i];
  CT_CHECK(w != NULL);
  int x1, y1, x2, y2;
  ra_ui_widget_rect(&app.ui.page, w, &x1, &y1, &x2, &y2);
  fk.now += 40;
  fk_touch(&fk, (x1 + x2) / 2, (y1 + y2) / 2, 1);
  app_step(&app, 0);
  fk.now += 40;
  fk_touch(&fk, (x1 + x2) / 2, (y1 + y2) / 2, 0);
  app_step(&app, 0);
}
static void press_key(int dev, int code) {
  fk.now += 40;
  fk_key(&fk, dev, code, 1);
  app_step(&app, 0);
  fk.now += 40;
  fk_key(&fk, dev, code, 0);
  app_step(&app, 0);
}
#define KEY_UP_ 103
#define KEY_DOWN_ 108
#define KEY_EXIT_ 174
#define KEY_HOME_ 364
#define KEY_FAV_ 102

static void test_fill_receiving_then_home(void) {
  start();
  /* pages arrive one per step so the screen can be inspected between them */
  ws_t a[] = {one(1, 0, 0, -1, "A")};
  push(0, 4, 5, HP_PAGE_KIND_NORMAL, 0, 0, 0, 0, "Zero", a, 1);
  step();
  CT_EQ(app.ui.page.page_id, RA_UI_PAGE_RECEIVING);
  CT_CHECK(!strcmp(msg_text(), "Receiving page"));
  CT_CHECK(!strcmp(cap_text(), "1 of 4"));
  push(2, 4, 5, HP_PAGE_KIND_NORMAL, 0, 0, 0, 0, "Two", a, 1); /* out of order is fine */
  step();
  CT_CHECK(!strcmp(cap_text(), "2 of 4"));
  CT_EQ(hp_store_count(&app.store), 2u);
  push(2, 4, 5, HP_PAGE_KIND_NORMAL, 0, 0, 0, 0, "Two", a, 1); /* an identical re-push counts once */
  push(3, 4, 5, HP_PAGE_KIND_HOME, 0, 0, 0, 0, "Home", a, 1);
  step();
  CT_CHECK(!strcmp(cap_text(), "3 of 4"));
  CT_EQ(app.ui.page.page_id, RA_UI_PAGE_RECEIVING);
  push(1, 4, 5, HP_PAGE_KIND_NORMAL, 0, 0, 0, 0, "One", a, 1);
  step();
  CT_EQ(shown(), 3); /* complete: the home page (kind home), not page 0 */
  CT_CHECK(!strcmp(lv_label_get_text(app.ui.title), "Home"));
  app_destroy(&app);
}

static void test_no_home_shows_page_zero_and_new_gen_clears(void) {
  start();
  ws_t a[] = {one(1, 0, 0, -1, "A")};
  push(1, 2, 9, HP_PAGE_KIND_NORMAL, 0, 0, 0, 0, "One", a, 1);
  push(0, 2, 9, HP_PAGE_KIND_NORMAL, 0, 0, 0, 0, "Zero", a, 1);
  settle();
  CT_EQ(shown(), 0);
  /* a push of another gen clears the store: back to receiving with the new total */
  push(1, 3, 10, HP_PAGE_KIND_NORMAL, 0, 0, 0, 0, "New one", a, 1);
  step();
  CT_EQ(hp_store_count(&app.store), 1u);
  CT_EQ(app.ui.page.page_id, RA_UI_PAGE_RECEIVING);
  CT_CHECK(!strcmp(cap_text(), "1 of 3"));
  push(2, 3, 10, HP_PAGE_KIND_NORMAL, 0, 0, 0, 0, "New two", a, 1);
  push(0, 3, 10, HP_PAGE_KIND_NORMAL, 0, 0, 0, 0, "New zero", a, 1);
  settle();
  CT_EQ(shown(), 0);
  CT_CHECK(!strcmp(lv_label_get_text(app.ui.title), "New zero"));
  app_destroy(&app);
}

static void test_rerender_only_when_visible_page_replaced(void) {
  start();
  push_set(3, 0);
  settle();
  CT_EQ(shown(), 1); /* home */
  unsigned c0 = app.display.cycles;
  ws_t f[] = {one(1, 0, RA_ICON_STAR, -1, "Changed")};
  push(2, N, 3, HP_PAGE_KIND_FAVORITES, 0, 0, 0, 0, "Favorites", f, 1); /* not on screen */
  settle();
  CT_EQ(app.display.cycles, c0);
  CT_EQ(shown(), 1);
  ws_t h[] = {one(1, 0, RA_ICON_TV, 0, "TV2")};
  push(1, N, 3, HP_PAGE_KIND_HOME, 0, 0, 0, 0, "Home", h, 1); /* the visible page */
  settle();
  CT_CHECK(app.display.cycles > c0);
  CT_CHECK(!strcmp(lv_label_get_text(app.ui.lbl[0]), "TV2"));
  /* an identical re-push of everything changes nothing */
  c0 = app.display.cycles;
  push(1, N, 3, HP_PAGE_KIND_HOME, 0, 0, 0, 0, "Home", h, 1);
  settle();
  CT_EQ(app.display.cycles, c0);
  app_destroy(&app);
}

static void test_activity_page_and_power_off_home(void) {
  start();
  push_set(3, 1); /* reversed order */
  settle();
  CT_EQ(shown(), 1);
  state(0, 1); /* activity 0 starts: its page (act = 1) */
  settle();
  CT_EQ(shown(), 0);
  CT_CHECK(app.ui.act_on && app.ui.act_idx == 0);
  CT_CHECK(app.ui.active & 1u); /* "Watch" (bind 0) is teal */
  unsigned c0 = app.display.cycles;
  state(0, 1); /* no change: nothing happens */
  settle();
  CT_EQ(app.display.cycles, c0);
  state(1, 1); /* activity 1 has no page: stay, highlight moves */
  settle();
  CT_EQ(shown(), 0);
  CT_EQ(app.ui.active, 0u);
  state(0, 0); /* power off: Home */
  settle();
  CT_EQ(shown(), 1);
  CT_CHECK(!app.ui.act_on);
  app_destroy(&app);
}

static void test_tap_running_activity_returns_to_its_page(void) {
  start();
  push_set(3, 0);
  settle();
  state(0, 1);
  settle();
  CT_EQ(shown(), 0);
  press_key(0, KEY_HOME_);
  settle();
  CT_EQ(shown(), 1);
  int sent = lk.nsent;
  tap_widget(1); /* "TV" (bind 0) is the running activity: no state change comes back, so go there locally */
  CT_EQ(shown(), 0);
  CT_CHECK(lk.nsent > sent); /* the start is still sent */
  app_destroy(&app);
}

static void test_highlight_is_a_partial_redraw(void) {
  start();
  push_set(3, 0);
  settle();
  fk.now += 100;
  unsigned px0 = app.display.flush_px;
  state(0, 1); /* home has "TV" bound to 0, and activity 0 also moves to its page: use the state without page change */
  step();
  (void)px0;
  CT_EQ(shown(), 0);
  /* on the activity page "Watch" is bound to 0: turning power off then on again repaints only it */
  state(0, 1);
  settle();
  app_destroy(&app);
  start();
  ws_t only[] = {one(1, 0, RA_ICON_TV, 0, "TV"), one(2, 0, RA_ICON_MUSIC, 1, "Music")};
  only[1].y = 1;
  uint8_t raw[HP_PAGE_MAX_BYTES];
  size_t len = enc(raw, 0, 1, 1, HP_PAGE_KIND_HOME, 0, 0, 0, 0, "Home", 2, 3, only, 2);
  fkl_push_page(&lk, raw, len);
  settle();
  CT_EQ(shown(), 0);
  unsigned p0 = app.display.flush_px, c0 = app.display.cycles;
  state(1, 1);
  step();
  CT_EQ(app.display.cycles - c0, 1u);
  CT_CHECK(app.display.flush_px - p0 < 240u * 320u / 2); /* only the widget changed: a partial redraw */
  CT_CHECK(app.ui.active == 2u);
  app_destroy(&app);
}

static void test_nav_widget_switches_locally(void) {
  start();
  push_set(3, 0);
  settle();
  CT_EQ(shown(), 1);
  int sent = lk.nsent;
  tap_widget(2); /* nav to page 3 */
  CT_EQ(shown(), 3);
  CT_EQ(lk.nsent, sent); /* no key_event */
  tap_widget(1); /* an ordinary button */
  CT_EQ(lk.nsent, sent + 2);
  CT_EQ(lk.sent[sent].code, 128 + 1);
  app_destroy(&app);
}

static void test_paged_up_down_are_local(void) {
  start();
  push_set(3, 0);
  settle();
  tap_widget(2); /* home -> scenes 1/2 (page 3) */
  CT_EQ(shown(), 3);
  CT_CHECK(app.ui.seq && !strcmp(lv_label_get_text(app.ui.seq), "1/2"));
  int sent = lk.nsent;
  press_key(0, KEY_DOWN_);
  CT_EQ(shown(), 4);
  CT_CHECK(!strcmp(lv_label_get_text(app.ui.seq), "2/2"));
  press_key(0, KEY_DOWN_); /* at the end: stays, still not forwarded */
  CT_EQ(shown(), 4);
  press_key(0, KEY_UP_);
  CT_EQ(shown(), 3);
  press_key(0, KEY_UP_);
  CT_EQ(shown(), 3);
  CT_EQ(lk.nsent, sent); /* Up / Down never reached the hub: no press, no release */
  /* a held Down is not forwarded either */
  fk.now += 40;
  fk_key(&fk, 0, KEY_DOWN_, 1);
  app_step(&app, 0);
  fk.now += 700;
  app_step(&app, 0);
  fk.now += 40;
  fk_key(&fk, 0, KEY_DOWN_, 0);
  app_step(&app, 0);
  CT_EQ(lk.nsent, sent);
  /* on a page that is not paged Up goes to the hub */
  press_key(1, KEY_HOME_);
  CT_EQ(shown(), 1);
  int s2 = lk.nsent;
  press_key(0, KEY_UP_);
  CT_EQ(lk.nsent, s2 + 2);
  app_destroy(&app);
}

static void test_home_and_favorites_navigate_and_forward(void) {
  start();
  push_set(3, 0);
  settle();
  tap_widget(2);
  CT_EQ(shown(), 3);
  int sent = lk.nsent;
  press_key(1, KEY_FAV_);
  CT_EQ(shown(), 2);
  CT_EQ(lk.nsent, sent + 2); /* forwarded as well */
  press_key(1, KEY_HOME_);
  CT_EQ(shown(), 1);
  CT_EQ(lk.nsent, sent + 4);
  app_destroy(&app);
  /* no favorites page: the key still goes to the hub, the screen stays */
  start();
  ws_t a[] = {one(1, 0, 0, -1, "A")};
  push(0, 1, 2, HP_PAGE_KIND_HOME, 0, 0, 0, 0, "Home", a, 1);
  settle();
  sent = lk.nsent;
  press_key(1, KEY_FAV_);
  CT_EQ(shown(), 0);
  CT_EQ(lk.nsent, sent + 2);
  app_destroy(&app);
}

static void test_exit_on_back_page_goes_home_and_chevron(void) {
  start();
  push_set(3, 0);
  settle();
  state(0, 1);
  settle();
  CT_EQ(shown(), 0); /* the activity page is a BACK page */
  CT_CHECK(app.ui.back != NULL);
  int sent = lk.nsent;
  press_key(0, KEY_EXIT_);
  CT_EQ(shown(), 1);
  CT_EQ(lk.nsent, sent); /* not forwarded */
  CT_CHECK(app.ui.back == NULL);
  press_key(0, KEY_EXIT_); /* on Home (not a BACK page) it goes to the hub */
  CT_EQ(lk.nsent, sent + 2);
  /* the chevron: a tap in the bar's left corner */
  state(0, 1);
  state(0, 1);
  settle();
  state(0, 0);
  settle();
  CT_EQ(shown(), 1);
  tap_widget(2); /* to page 3 (no back) */
  state(0, 1);
  settle();
  CT_EQ(shown(), 0);
  sent = lk.nsent;
  fk.now += 40;
  fk_touch(&fk, 12, 12, 1);
  app_step(&app, 0);
  fk.now += 40;
  fk_touch(&fk, 12, 12, 0);
  app_step(&app, 0);
  CT_EQ(shown(), 1);
  CT_EQ(lk.nsent, sent);
  app_destroy(&app);
}

/* A nav target that has not arrived (after the receiving screen gave way to the best page): a brief toast, the screen stays. */
static void test_missing_nav_target_toasts_and_stays(void) {
  start();
  push_set(3, 0);
  settle();
  CT_EQ(shown(), 1);
  uint8_t keep[HP_PAGE_MAX_BYTES];
  size_t klen = app.store.meta[3].len;
  memcpy(keep, app.store.raw[3], klen);
  memset(&app.store.meta[3], 0, sizeof app.store.meta[3]); /* page 3 never arrived (a lost page) */
  app.store.count--;
  tap_widget(2); /* nav to 3 */
  CT_EQ(shown(), 1); /* stays put, not the receiving screen */
  CT_CHECK(app.ui.toast != NULL);
  CT_CHECK(!strcmp(lv_label_get_text(lv_obj_get_child(app.ui.toast, 0)), "Page not received yet"));
  fkl_push_page(&lk, keep, klen);
  settle();
  CT_EQ(shown(), 1);
  tap_widget(2);
  CT_EQ(shown(), 3); /* now it is there */
  app_destroy(&app);
}

/* ---- review fixes: receiving timeout, dead touch, v1 <-> v2 ---- */

static const ws_t A1 = {HP_WIDGET_BUTTON, 1, 0, 0, 2, 1, 0, 0, 0, 0, -1, "A"};

static void test_incomplete_set_shows_best_page_after_10s(void) {
  start();
  ws_t a[] = {A1};
  push(2, 4, 5, HP_PAGE_KIND_NORMAL, 0, 0, 0, 0, "Two", a, 1);
  push(3, 4, 5, HP_PAGE_KIND_HOME, 0, 0, 0, 0, "Home", a, 1);
  settle();
  CT_EQ(app.ui.page.page_id, RA_UI_PAGE_RECEIVING);
  fk.now += 9000;
  step();
  CT_EQ(app.ui.page.page_id, RA_UI_PAGE_RECEIVING); /* not yet */
  fk.now += 1500;
  step();
  CT_EQ(shown(), 3); /* the home page, though the set is incomplete */
  tap_widget(1);
  /* a missing target toasts and stays; the set keeps filling in the background */
  push(1, 4, 5, HP_PAGE_KIND_NORMAL, 0, 0, 0, 0, "One", a, 1);
  settle();
  CT_EQ(shown(), 3);
  CT_EQ(hp_store_count(&app.store), 3u);
  push(0, 4, 5, HP_PAGE_KIND_NORMAL, 0, 0, 0, 0, "Zero", a, 1);
  settle();
  CT_EQ(shown(), 3); /* complete: the user is not moved */
  CT_CHECK(hp_store_complete(&app.store));
  app_destroy(&app);
  /* no home page: the lowest stored id */
  start();
  push(2, 4, 5, HP_PAGE_KIND_NORMAL, 0, 0, 0, 0, "Two", a, 1);
  push(1, 4, 5, HP_PAGE_KIND_NORMAL, 0, 0, 0, 0, "One", a, 1);
  settle();
  fk.now += 11000;
  step();
  CT_EQ(shown(), 1);
  app_destroy(&app);
}

static void test_partial_set_nav_to_missing_page_toasts(void) {
  start();
  ws_t h[] = {one(1, HP_ROLE_NAV, RA_ICON_CHEV_RIGHT, 3, "Go")};
  push(0, 4, 5, HP_PAGE_KIND_HOME, 0, 0, 0, 0, "Home", h, 1);
  settle();
  fk.now += 11000;
  step();
  CT_EQ(shown(), 0);
  press_key(1, KEY_FAV_); /* wakes the screen if it dimmed */
  tap_widget(1); /* page 3 has not arrived */
  CT_EQ(shown(), 0);
  CT_CHECK(app.ui.toast != NULL);
  app_destroy(&app);
}

static void test_home_exit_chevron_work_from_receiving_and_error(void) {
  start();
  ws_t a[] = {A1};
  push(1, 4, 5, HP_PAGE_KIND_NORMAL, 0, 0, 0, 0, "One", a, 1);
  settle();
  CT_EQ(app.ui.page.page_id, RA_UI_PAGE_RECEIVING);
  int sent = lk.nsent;
  press_key(1, KEY_HOME_); /* Home on the receiving screen: the best page now */
  CT_EQ(shown(), 1);
  CT_EQ(lk.nsent, sent + 2); /* and still forwarded */
  app_destroy(&app);

  start();
  push_set(3, 0);
  settle();
  CT_EQ(shown(), 1);
  const int keys[3][2] = {{1, KEY_HOME_}, {0, KEY_EXIT_}, {-1, 0}};
  for (int k = 0; k < 3; k++) {
    ra_ui_test_fail_at = 3; /* the next page build runs out of memory */
    tap_widget(2);          /* home -> scenes */
    ra_ui_test_fail_at = -1;
    CT_EQ(app.ui.page.page_id, RA_UI_PAGE_ERROR);
    sent = lk.nsent;
    if (keys[k][0] >= 0) press_key(keys[k][0], keys[k][1]);
    else { /* a tap on the error screen */
      fk.now += 40; fk_touch(&fk, 120, 160, 1); app_step(&app, 0);
      fk.now += 40; fk_touch(&fk, 120, 160, 0); app_step(&app, 0);
    }
    CT_EQ(shown(), 1); /* the start page again */
  }
  /* the receiving screen of a new gen: the bar's left corner goes to the best page too */
  ws_t a2[] = {A1};
  push(0, 3, 9, HP_PAGE_KIND_NORMAL, 0, 0, 0, 0, "N0", a2, 1);
  settle();
  CT_EQ(app.ui.page.page_id, RA_UI_PAGE_RECEIVING);
  fk.now += 40; fk_touch(&fk, 12, 12, 1); app_step(&app, 0);
  fk.now += 40; fk_touch(&fk, 12, 12, 0); app_step(&app, 0);
  CT_EQ(shown(), 0);
  app_destroy(&app);
}

/* v1 pages, remote_info, then the v2 set with the SAME gen as one stored before: the v2 home must show, with paging. */
static void test_v1_then_identical_v2_set_switches_to_v2(void) {
  static const uint8_t V1[] = {0x01, 0x01, 0x04, 0x06, 0x01, 0x00, 0x01, 0x00, 0x11, 0x04, 'V', 'o', 'l', '+'};
  start();
  push_set(3, 0);
  settle();
  CT_EQ(shown(), 1);
  CT_CHECK(app.v2);
  fkl_push_page(&lk, V1, sizeof V1); /* an old-format Home Assistant pushes v1 pages */
  settle();
  CT_CHECK(!app.v2);
  CT_EQ(app.ui.page.page_id, 1);
  CT_EQ(app.ui.page.version, 1);
  push_set(3, 0); /* identical to what was stored before */
  settle();
  CT_CHECK(app.v2);
  CT_EQ(shown(), 1); /* the v2 home */
  tap_widget(2);
  CT_EQ(shown(), 3);
  CT_CHECK(app.ui.seq && !strcmp(lv_label_get_text(app.ui.seq), "1/2"));
  press_key(0, KEY_DOWN_);
  CT_EQ(shown(), 4); /* paging works */
  /* the reverse: a v1 page while a v2 set is stored replaces the display and leaves v2 mode (a v1-only HA needs that) */
  fkl_push_page(&lk, V1, sizeof V1);
  settle();
  CT_CHECK(!app.v2);
  CT_EQ(app.ui.page.version, 1);
  CT_EQ(hp_store_count(&app.store), 0u);
  /* ...and tile updates for it work the v1 way */
  app_destroy(&app);
}

static void test_tile_state_is_kept_for_pages_off_screen(void) {
  start();
  ws_t t[] = {{HP_WIDGET_TILE, 1, 0, 0, 2, 1, 0, 0, RA_ICON_BULB, 0, -1, "Lamp"}};
  push(0, 2, 4, HP_PAGE_KIND_HOME, 0, 0, 0, 0, "Home", t, 1);
  push(1, 2, 4, HP_PAGE_KIND_NORMAL, 0, 0, 0, 0, "Other", t, 1);
  settle();
  CT_EQ(shown(), 0);
  tilestate(1, 1, HP_TILE_ON, "On"); /* page 1 is not shown */
  tilestate(0, 1, HP_TILE_UNAVAILABLE, "x");
  settle();
  CT_EQ(app.ui.page.widgets[0].tstate, HP_TILE_UNAVAILABLE);
  CT_CHECK(!strcmp(lv_label_get_text(app.ui.lbl[0]), "n/a"));
  hp_page_t p;
  CT_EQ(hp_store_get(&app.store, 1, &p), 0);
  CT_EQ(p.widgets[0].tstate, HP_TILE_ON);
  CT_CHECK(!strcmp(p.widgets[0].text, "On"));
  /* a stale caption while the link is lost, gone when the hub speaks */
  fkl_push_kind(&lk, APP_LINK_EV_FAILED);
  step();
  CT_CHECK(!strcmp(lv_label_get_text(app.ui.wcap[0]), "stale"));
  fkl_push_kind(&lk, APP_LINK_EV_ALIVE);
  step();
  CT_CHECK(!strcmp(lv_label_get_text(app.ui.wcap[0]), "Lamp"));
  app_destroy(&app);
}

static void test_v1_page_still_works_and_is_not_stored(void) {
  start();
  static const uint8_t V1[] = {0x01, 0x01, 0x04, 0x06, 0x01, 0x00, 0x01, 0x00, 0x11, 0x04, 'V', 'o', 'l', '+'};
  fkl_push_page(&lk, V1, sizeof V1);
  settle();
  CT_EQ(app.ui.page.page_id, 1);
  CT_CHECK(!app.v2);
  CT_EQ(hp_store_count(&app.store), 0u);
  app_destroy(&app);
}

int main(void) {
  CT_RUN(test_fill_receiving_then_home);
  CT_RUN(test_no_home_shows_page_zero_and_new_gen_clears);
  CT_RUN(test_rerender_only_when_visible_page_replaced);
  CT_RUN(test_activity_page_and_power_off_home);
  CT_RUN(test_tap_running_activity_returns_to_its_page);
  CT_RUN(test_highlight_is_a_partial_redraw);
  CT_RUN(test_nav_widget_switches_locally);
  CT_RUN(test_paged_up_down_are_local);
  CT_RUN(test_home_and_favorites_navigate_and_forward);
  CT_RUN(test_exit_on_back_page_goes_home_and_chevron);
  CT_RUN(test_missing_nav_target_toasts_and_stays);
  CT_RUN(test_incomplete_set_shows_best_page_after_10s);
  CT_RUN(test_partial_set_nav_to_missing_page_toasts);
  CT_RUN(test_home_exit_chevron_work_from_receiving_and_error);
  CT_RUN(test_v1_then_identical_v2_set_switches_to_v2);
  CT_RUN(test_tile_state_is_kept_for_pages_off_screen);
  CT_RUN(test_v1_page_still_works_and_is_not_stored);
  return CT_DONE();
}
