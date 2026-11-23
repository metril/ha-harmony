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
 * REMOTE_INFO. The first object after open, after every re-attach and after a send that failed all its retries (the
 * hub may have restarted and forgotten it) is remote_info {remote, 2, caps 15, highest icon id}, ahead of the battery
 * message; it is also repeated every RF_LINK_BATTERY_MS just ahead of the battery message (4 bytes; the hub only relays changes). A failed or refused send is
 * retried in RF_LINK_RETRY_MS. Receives tile_state like tile_update.
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
 * nothing was transmitted for RF_LINK_POLL_MS while an object awaits its ack or a partial downlink
 * object is being reassembled (a frame heard within the last second), and every
 * RF_LINK_IDLE_POLL_MS otherwise; none while asleep. Any frame heard from the hub raises
 * APP_LINK_EV_ALIVE. */
#include <stdbool.h>
#include <stdint.h>

#include "hub/io/rf_spi.h"
#include "hub/rf/link.h"
#include "remote/app/app.h"
#include "remote/page/store.h"

#define RF_LINK_REMOTE_ID 1 /* key_event.remote, as remote_sim's default */
/* remote_info (docs/plans/2026-11-18-page-format-v2.md): page format 2, caps bit 0 tile_state + bit 1 page store, bit 2 theme_color,
 * icons = the highest stock icon id the font holds. */
#define RF_LINK_PAGE_FMT 2
#define RF_LINK_CAP_TILE_STATE 0x01  /* bit 0 */
#define RF_LINK_CAP_PAGE_STORE 0x02   /* bit 1 */
#define RF_LINK_CAP_THEME_COLOR 0x04  /* bit 2: palette widget colours + preset themes */
#define RF_LINK_CAP_THEME_CUSTOM 0x08 /* bit 3: custom theme (theme_push) + free colours */
#define RF_LINK_CAPS (RF_LINK_CAP_TILE_STATE | RF_LINK_CAP_PAGE_STORE | RF_LINK_CAP_THEME_COLOR | RF_LINK_CAP_THEME_CUSTOM)
#define RF_LINK_BATTERY_MS (5 * 60 * 1000)
#define RF_LINK_RETRY_MS 5000
#define RF_LINK_POLL_MS HP_LINK_POLL_MS          /* poll gap while an object awaits its ack */
#define RF_LINK_IDLE_POLL_MS HP_LINK_IDLE_POLL_MS /* poll gap otherwise, while awake */
/* REJOIN. While the chip reports the link lost, the link writes the sync pulse (hp_rf_spi_sync) on events only: the
 * loss itself (once), a key, a tap, a display wake and a resume from suspend. No retry timer: nothing runs while the
 * link is up, the remote is asleep or nothing happens. */
#define RF_LINK_SYNC_COALESCE_MS 250 /* triggers closer than this share one pulse (resume + wake) */
#define RF_LINK_SYNC_QUERY_MS 5      /* the quick uplink (remote_info) follows the pulse after this, as stock's sync request */
#define RF_LINK_RING 16
/* Now Playing: now_playing (29) and cover_chunk (42) from the hub become APP_LINK_EV_NOW_PLAYING / _COVER; the hub only sends
 * them to a remote it heard awake, so nothing is asked for and no caps bit is needed. */
#define RA_NP_TEXT 48
#define RA_COVER_CHUNK_MAX 220

typedef struct {
  ra_sys_t sys; /* own instance: only the sysfs soc read and the clock are used */
  hp_rf_transport *tp; /* rf_spi */
  hp_rf_transport lt;  /* what the link sees: tp without attached() (see rf_link.c) */
  hp_rf_link_t link;
  uint8_t remote;
  app_link_event_t ring[RF_LINK_RING];
  unsigned head, n;
  uint32_t seen_failures, seen_rx;
  uint8_t seen_retries;
  bool logged_start; /* note_start: the last object announced as sending */
  uint8_t logged_seq, logged_type;
  uint64_t next_batt_ms, next_info_ms, info_sent_ms;
  bool info_pending;
  const hp_store_t *store; /* the page store reported in remote_info (gen, pages); NULL = none */
  uint8_t told_gen, told_pages; /* what the last remote_info said about it */
  int last_pct;
  bool was_attached, lost, dead_reported;
  bool awake;
  uint64_t last_sync_ms;
  bool synced_once;
  bool pulse_pending, ever_up; /* a pulse awaits its link-up (for the log); the link has been up since boot */
} rf_link_t;

/* No device access. Until rf_link_open() succeeds the link reports one APP_LINK_EV_FAILED. */
void rf_link_init(rf_link_t *l, const ra_ops_t *ops, uint8_t remote_id);
/* Opens rf_spi (cfg->role is forced to remote), claims it and starts the link. rf_spi's own
 * negative codes on failure (-HP_E_HALBUSY, ...). */
int rf_link_open(rf_link_t *l, const hp_rf_spi_cfg_t *cfg);
/* Stops the reader and closes the device exactly once; idempotent. */
void rf_link_close(rf_link_t *l);
/* The page store whose generation and page count go into remote_info. A completed or emptied store that no
 * longer matches what the hub was told is announced again (one message), so the hub's cache stays true. */
void rf_link_set_store(rf_link_t *l, const hp_store_t *store);
app_link_t rf_link_app(rf_link_t *l);

#endif
