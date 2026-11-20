#ifndef HH_BT_SC_H
#define HH_BT_SC_H
/* GATT Service Changed bookkeeping (no BTstack dependency). Each LE bond remembers, in the key store, the ATT
 * database hash its peer was last told about. The peer is sent one Service Changed indication (whole range) only
 * when that differs from the running database, and the new hash is stored once the indication is confirmed. A fresh
 * bond has just discovered the current database, so it is stored at bonding and never indicated. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BT_SC_SLOTS 8

typedef struct {
    int (*get)(uint32_t tag, uint8_t *v, uint32_t cap); /* bytes read, 0 = none */
    int (*put)(uint32_t tag, const uint8_t *v, uint32_t len);
} bt_sc_store_t;

/* FNV-1a over the compiled ATT database image. */
uint32_t bt_sc_hash(const uint8_t *db, size_t n);
/* Start of a run: the store and the hash of the running database; clears the per-connection state. */
void bt_sc_init(const bt_sc_store_t *store, uint32_t cur_hash);
/* The 4-byte Service Changed value: whole database range 0x0001..0xFFFF (little endian). */
void bt_sc_value(uint8_t out[4]);
/* A pairing just created this bond: record the current hash for it (no indication). */
void bt_sc_bond_fresh(const uint8_t addr[6], uint8_t addr_type);
/* The connection's peer is a known bond (identity address). True when an indication must now be sent. */
bool bt_sc_peer(uint16_t handle, const uint8_t addr[6], uint8_t addr_type);
/* The Service Changed CCCD was written or restored. True when an indication must now be sent. */
bool bt_sc_ccc(uint16_t handle, uint16_t value);
/* The indication was handed to the stack (at most once per connection). */
void bt_sc_sent(uint16_t handle);
/* The peer confirmed the indication: remember the current hash for its bond. */
void bt_sc_confirmed(uint16_t handle);
/* Connection ended. */
void bt_sc_drop(uint16_t handle);
/* Current CCCD value of the connection (0 = none). */
uint16_t bt_sc_ccc_get(uint16_t handle);
#endif
