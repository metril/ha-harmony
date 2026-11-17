#ifndef HP_REMOTE_RF_LINK_H
#define HP_REMOTE_RF_LINK_H

/* The remote's RF link: app_link_t over hp_rf_link_t on rf_spi (HP_RF_SPI_ROLE_REMOTE). Mirrors the
 * hub (hub/hub_handlers.c): same hp_rf_pack/unpack payloads, stop-and-wait objects with 3 retries.
 *
 * The RF module owns /dev/rfspi through rf_spi's own syscall seam; the app's allow-list (sys.h)
 * still refuses it. Receives page_push / tile_update / state_update into the app's events; send_key
 * packs key_event {remote, logical key, action}. A send that fails, or an object dropped after all
 * retries, raises APP_LINK_EV_FAILED (the fallback page). An idle link is silent: acks follow
 * objects, so nothing is expected while nobody presses a key.
 *
 * HEARTBEAT. The hub treats the remote as absent until it speaks, so a `battery` message goes out
 * once right after open and then every RF_LINK_BATTERY_MS while the app runs, plus once when the
 * link re-attaches after a loss. percent = the STC3115 soc (ra_battery_percent, the guard's own
 * read). millivolts is always 0: the schema has no "unknown" encoding and the app's allow-list has
 * no voltage node (nothing new is added), so 0 means unknown. If the soc read fails the last good
 * percent is sent; if there never was one the cycle is skipped and retried in RF_LINK_RETRY_MS.
 * A failed or refused send is retried in RF_LINK_RETRY_MS.
 *
 * POLLING. The hub's frames reach the remote only piggybacked on the remote's own transmissions, so
 * while awake (the app reports it through set_awake) the link sends a minimal poll frame whenever
 * nothing was transmitted for RF_LINK_POLL_MS while an object awaits its ack, and every
 * RF_LINK_IDLE_POLL_MS otherwise; none while asleep. Any frame heard from the hub raises
 * APP_LINK_EV_ALIVE. */
#include <stdbool.h>
#include <stdint.h>

#include "hub/io/rf_spi.h"
#include "hub/rf/link.h"
#include "remote/app/app.h"

#define RF_LINK_REMOTE_ID 1 /* key_event.remote, as remote_sim's default */
#define RF_LINK_BATTERY_MS (5 * 60 * 1000)
#define RF_LINK_RETRY_MS 5000
#define RF_LINK_POLL_MS HP_LINK_POLL_MS          /* poll gap while an object awaits its ack */
#define RF_LINK_IDLE_POLL_MS HP_LINK_IDLE_POLL_MS /* poll gap otherwise, while awake */
#define RF_LINK_RING 16

typedef struct {
  ra_sys_t sys; /* own instance: only the sysfs soc read and the clock are used */
  hp_rf_transport *tp; /* rf_spi */
  hp_rf_transport lt;  /* what the link sees: tp without attached() (see rf_link.c) */
  hp_rf_link_t link;
  uint8_t remote;
  app_link_event_t ring[RF_LINK_RING];
  unsigned head, n;
  uint32_t seen_failures, seen_rx;
  uint64_t next_batt_ms;
  int last_pct;
  bool was_attached, lost, dead_reported;
} rf_link_t;

/* No device access. Until rf_link_open() succeeds the link reports one APP_LINK_EV_FAILED. */
void rf_link_init(rf_link_t *l, const ra_ops_t *ops, uint8_t remote_id);
/* Opens rf_spi (cfg->role is forced to remote), claims it and starts the link. rf_spi's own
 * negative codes on failure (-HP_E_HALBUSY, ...). */
int rf_link_open(rf_link_t *l, const hp_rf_spi_cfg_t *cfg);
/* Stops the reader and closes the device exactly once; idempotent. */
void rf_link_close(rf_link_t *l);
app_link_t rf_link_app(rf_link_t *l);

#endif
