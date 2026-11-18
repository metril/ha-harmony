#include "remote/app/rf_link.h"

#include <string.h>

#include "hub/hp_errors.h"
#include "hub/rf/rf_payload.h"
#include "remote/app/icons.h"

static uint32_t t32(uint64_t now) { return (uint32_t)now; }

static app_link_event_t *ring_push(rf_link_t *l) {
  if (l->n == RF_LINK_RING) { /* the app drains 8 per step: only a flood gets here; keep the oldest */
    ra_logf(&l->sys, "rf_link: event queue full, dropping");
    return NULL;
  }
  app_link_event_t *e = &l->ring[(l->head + l->n++) % RF_LINK_RING];
  memset(e, 0, sizeof *e);
  return e;
}

static void raise_failed_ex(rf_link_t *l, bool fatal) {
  app_link_event_t *e = ring_push(l);
  if (e) {
    e->kind = APP_LINK_EV_FAILED;
    e->fatal = fatal;
  }
}
static void raise_failed(rf_link_t *l) { raise_failed_ex(l, false); }

static void deliver(void *ctx, const hp_rf_msg_t *m) {
  rf_link_t *l = ctx;
  hp_rf_val_t v[4];
  switch (m->type) {
  case HP_MSG_PAGE_PUSH:
    if (hp_rf_unpack(m->type, m->payload, m->len, v, 3) == 0 && v[2].len <= HP_PAGE_MAX_BYTES) {
      app_link_event_t *e = ring_push(l);
      if (!e) return;
      e->kind = APP_LINK_EV_PAGE_PUSH;
      e->page = (uint8_t)v[1].u;
      memcpy(e->data, v[2].p, v[2].len);
      e->len = v[2].len;
    } else {
      ra_logf(&l->sys, "rf_link: bad page_push dropped");
    }
    break;
  case HP_MSG_TILE_UPDATE:
    if (hp_rf_unpack(m->type, m->payload, m->len, v, 4) == 0 && v[3].len <= HP_PAGE_MAX_TEXT) {
      app_link_event_t *e = ring_push(l);
      if (!e) return;
      e->kind = APP_LINK_EV_TILE_UPDATE;
      e->page = (uint8_t)v[1].u;
      e->tile = (uint8_t)v[2].u;
      memcpy(e->text, v[3].p, v[3].len);
      e->text[v[3].len] = 0;
    } else {
      ra_logf(&l->sys, "rf_link: bad tile_update dropped");
    }
    break;
  case HP_MSG_TILE_STATE: {
    hp_rf_val_t t[5];
    if (hp_rf_unpack(m->type, m->payload, m->len, t, 5) == 0 && t[3].u <= HP_TILE_UNAVAILABLE && t[4].len <= HP_PAGE_MAX_TEXT) {
      app_link_event_t *e = ring_push(l);
      if (!e) return;
      e->kind = APP_LINK_EV_TILE_STATE;
      e->page = (uint8_t)t[1].u;
      e->tile = (uint8_t)t[2].u;
      e->state = (uint8_t)t[3].u;
      memcpy(e->text, t[4].p, t[4].len);
      e->text[t[4].len] = 0;
    } else {
      ra_logf(&l->sys, "rf_link: bad tile_state dropped");
    }
    break;
  }
  case HP_MSG_STATE_UPDATE:
    if (hp_rf_unpack(m->type, m->payload, m->len, v, 2) == 0) {
      app_link_event_t *e = ring_push(l);
      if (!e) return;
      e->kind = APP_LINK_EV_STATE_UPDATE;
      e->activity = (uint8_t)v[0].u;
      e->power = v[1].u != 0;
    } else {
      ra_logf(&l->sys, "rf_link: bad state_update dropped");
    }
    break;
  default: /* anything else from the hub is dropped */
    break;
  }
}

static int send_msg(rf_link_t *l, hp_msg_type_t t, const hp_rf_val_t *v, size_t nv, uint64_t now) {
  uint8_t payload[HP_RF_MSG_MAX];
  size_t plen;
  int rc = hp_rf_pack(t, v, nv, payload, sizeof payload, &plen);
  if (rc < 0) return rc;
  return hp_rf_link_send(&l->link, t, payload, plen, t32(now));
}

/* The link layer refuses to send while attached() is false, but a remote whose chip has not yet
 * reported link status (an unvalidated report) would then never speak first, and the hub treats a
 * remote as absent until it does. So the link gets a view of rf_spi without attached(): sends always
 * go out, and a silent hub shows up as a failed send after the retries, as designed. */
static int lt_send(void *self, const uint8_t *f, size_t n) {
  rf_link_t *l = self;
  return l->tp->send(l->tp->self, f, n);
}
static int lt_recv(void *self, uint8_t *f, size_t cap, int ms) {
  rf_link_t *l = self;
  return l->tp->recv(l->tp->self, f, cap, ms);
}
static int lt_tx_status(void *self, uint8_t *slot, uint8_t *status) {
  rf_link_t *l = self;
  return l->tp->tx_status ? l->tp->tx_status(l->tp->self, slot, status) : 0;
}
static int lt_claim(void *self) {
  rf_link_t *l = self;
  return l->tp->claim(l->tp->self);
}

static int rf_send_key(void *ctx, uint8_t code, uint8_t action) {
  rf_link_t *l = ctx;
  if (!l->tp) return -HP_E_NOENT;
  hp_rf_val_t v[3] = {{.u = l->remote}, {.u = code}, {.u = action}};
  int rc = send_msg(l, HP_MSG_KEY_EVENT, v, 3, l->sys.ops.now_ms(l->sys.ops.ctx));
  if (rc < 0) raise_failed(l);
  return rc;
}

static void store_view(const rf_link_t *l, uint8_t *gen, uint8_t *pages) {
  bool have = l->store && l->store->have_gen && hp_store_count(l->store) > 0;
  *gen = have ? l->store->gen : 0;
  *pages = have ? (uint8_t)hp_store_count(l->store) : 0;
}

static void send_info(rf_link_t *l, uint64_t now) {
  uint8_t gen, pages;
  store_view(l, &gen, &pages);
  hp_rf_val_t v[6] = {{.u = l->remote}, {.u = RF_LINK_PAGE_FMT}, {.u = RF_LINK_CAPS}, {.u = RA_ICON_MAX},
                      {.u = gen}, {.u = pages}};
  if (send_msg(l, HP_MSG_REMOTE_INFO, v, 6, now) < 0) {
    l->next_info_ms = now + RF_LINK_RETRY_MS;
    return;
  }
  l->info_pending = false;
  l->info_sent_ms = now;
  l->told_gen = gen;
  l->told_pages = pages;
}

static void heartbeat(rf_link_t *l, uint64_t now) {
  if (!l->info_pending && now - l->info_sent_ms >= RF_LINK_BATTERY_MS) send_info(l, now); /* repeated: ahead of the battery */
  int pct = ra_battery_percent(&l->sys);
  if (pct < 0) pct = l->last_pct;
  if (pct < 0) { /* never had a reading: nothing honest to send */
    l->next_batt_ms = now + RF_LINK_RETRY_MS;
    return;
  }
  hp_rf_val_t v[3] = {{.u = l->remote}, {.u = (uint32_t)pct}, {.u = 0}};
  if (send_msg(l, HP_MSG_BATTERY, v, 3, now) < 0) {
    l->next_batt_ms = now + RF_LINK_RETRY_MS;
    return;
  }
  l->last_pct = pct;
  l->next_batt_ms = now + RF_LINK_BATTERY_MS;
}

static void service(rf_link_t *l, uint64_t now) {
  if (!l->tp) {
    if (!l->dead_reported) {
      l->dead_reported = true;
      raise_failed_ex(l, true); /* no radio at all: say so at once */
    }
    return;
  }
  int rc = hp_rf_link_pump(&l->link, t32(now));
  if (l->link.rx_frames != l->seen_rx) { /* something came from the hub */
    l->seen_rx = l->link.rx_frames;
    if (l->info_pending && l->next_info_ms == UINT64_MAX) l->next_info_ms = now;
    app_link_event_t *e = ring_push(l);
    if (e) e->kind = APP_LINK_EV_ALIVE;
  }
  if (rc == -HP_E_IO) ra_logf(&l->sys, "rf_link: link dead");
  bool att = l->tp->attached ? l->tp->attached(l->tp->self) : l->was_attached;
  if (att && l->lost) { /* re-attached after a loss: tell the hub we are here */
    l->lost = false;
    l->next_batt_ms = now;
    l->info_pending = true;
    l->next_info_ms = now;
  }
  if (!att && l->was_attached) l->lost = true;
  l->was_attached = att;
  if (l->store && !l->info_pending) { /* a finished (or emptied) page store the hub was not told about */
    uint8_t gen, pages;
    store_view(l, &gen, &pages);
    if ((gen != l->told_gen || pages != l->told_pages) && (pages == 0 || hp_store_complete(l->store))) {
      l->info_pending = true;
      l->next_info_ms = now;
    }
  }
  if (l->info_pending && now >= l->next_info_ms) send_info(l, now); /* first uplink after a link-up */
  if (now >= l->next_batt_ms) {
    heartbeat(l, now);
    hp_rf_link_pump(&l->link, t32(now));
  }
  uint32_t f = hp_rf_link_status(&l->link).failures;
  if (f != l->seen_failures) {
    l->seen_failures = f;
    l->info_pending = true; /* the hub went quiet: when it is heard again it may have forgotten us */
    l->next_info_ms = UINT64_MAX; /* held until a frame arrives (below): no traffic towards a silent hub */
    raise_failed(l);
  }
}

static int rf_poll(void *ctx, uint64_t now, app_link_event_t *ev) {
  rf_link_t *l = ctx;
  if (!l->n) service(l, now);
  if (!l->n) return 0;
  *ev = l->ring[l->head];
  l->head = (l->head + 1) % RF_LINK_RING;
  l->n--;
  return 1;
}

static int rf_get_fd(void *ctx) {
  rf_link_t *l = ctx;
  return l->tp ? hp_rf_spi_wake_fd(l->tp) : -1;
}

static void rf_set_awake(void *ctx, bool awake) {
  rf_link_t *l = ctx;
  if (l->tp) hp_rf_link_set_awake(&l->link, awake, t32(l->sys.ops.now_ms(l->sys.ops.ctx)));
}

static uint32_t rf_next_ms(void *ctx, uint64_t now) {
  rf_link_t *l = ctx;
  if (l->n || (!l->tp && !l->dead_reported)) return 0;
  if (!l->tp) return UINT32_MAX;
  uint64_t best = l->next_batt_ms > now ? l->next_batt_ms - now : 0;
  if (l->info_pending) {
    uint64_t d = l->next_info_ms > now ? l->next_info_ms - now : 0;
    if (d < best) best = d;
  }
  int r = hp_rf_link_next_timeout_ms(&l->link, t32(now)); /* the retry deadline always bounds the wait */
  if (r >= 0 && (uint64_t)r < best) best = (uint64_t)r;
  return best > UINT32_MAX ? UINT32_MAX : (uint32_t)best;
}

void rf_link_init(rf_link_t *l, const ra_ops_t *ops, uint8_t remote_id) {
  memset(l, 0, sizeof *l);
  ra_sys_init(&l->sys, ops, NULL);
  l->remote = remote_id;
  l->last_pct = -1;
}

int rf_link_open(rf_link_t *l, const hp_rf_spi_cfg_t *cfg) {
  hp_rf_spi_cfg_t c = *cfg;
  c.role = HP_RF_SPI_ROLE_REMOTE;
  hp_rf_transport *tp;
  int rc = hp_rf_spi_open(&c, &tp);
  if (rc < 0) return rc;
  if (tp->claim(tp->self) < 0) {
    hp_rf_spi_destroy(tp);
    return -HP_E_BUSY;
  }
  l->tp = tp;
  l->lt = (hp_rf_transport){l, lt_send, lt_recv, lt_claim, NULL, lt_tx_status, NULL};
  hp_rf_link_init(&l->link, &l->lt, false, 0, deliver, l);
  hp_rf_link_set_poll(&l->link, RF_LINK_POLL_MS, RF_LINK_IDLE_POLL_MS); /* active once the app reports awake */
  if (cfg->debug) hp_rf_link_set_log(&l->link, cfg->log ? cfg->log : hp_rf_log_stderr, cfg->log_ctx);
  l->next_batt_ms = 0; /* first heartbeat on the first poll */
  l->info_pending = true;
  l->next_info_ms = 0; /* remote_info goes out first */
  l->dead_reported = false;
  return 0;
}

void rf_link_set_store(rf_link_t *l, const hp_store_t *store) { l->store = store; }

void rf_link_close(rf_link_t *l) {
  if (!l->tp) return;
  hp_rf_spi_destroy(l->tp); /* signals and joins the reader, then closes the device */
  l->tp = NULL;
}

app_link_t rf_link_app(rf_link_t *l) {
  return (app_link_t){.ctx = l, .send_key = rf_send_key, .poll = rf_poll, .get_fd = rf_get_fd, .next_ms = rf_next_ms,
                      .set_awake = rf_set_awake};
}
