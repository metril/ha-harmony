#ifndef HH_BT_SC_H
#define HH_BT_SC_H
/* GATT Service Changed bookkeeping (no BTstack dependency): has the ATT database changed since the last
 * run, and which connection has its Service Changed CCCD enabled and still needs the one indication. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BT_SC_SLOTS 8

/* FNV-1a over the compiled ATT database image. */
uint32_t bt_sc_hash(const uint8_t *db, size_t n);
/* stored/len: the value read from the store (len != 4: nothing stored). True when the database differs from the
 * stored hash, or there is none (first run of this firmware: a bonded peer may hold a stale cache). */
bool bt_sc_db_changed(uint32_t cur, const uint8_t *stored, size_t len);
/* The 4-byte Service Changed value: whole database range 0x0001..0xFFFF (little endian). */
void bt_sc_value(uint8_t out[4]);
/* Arms (or clears) the once-per-connection indication for this run. */
void bt_sc_set_changed(bool changed);
/* The CCCD was written or restored from the store. True when an indication must now be sent (once). */
bool bt_sc_ccc(uint16_t handle, uint16_t value);
/* The indication for this connection was sent. */
void bt_sc_sent(uint16_t handle);
/* Connection ended. */
void bt_sc_drop(uint16_t handle);
/* Current CCCD value of the connection (0 = none). */
uint16_t bt_sc_ccc_get(uint16_t handle);
#endif
