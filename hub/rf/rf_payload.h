#ifndef HP_RF_PAYLOAD_H
#define HP_RF_PAYLOAD_H

/* RF payload codec for the rf-capable messages (key_event, state_update,
 * page_push, battery), driven by the generated descriptors. Layout: fields
 * in schema order, big-endian; bool = 1 byte (0/1); enum = u8; str/bytes =
 * u8 length then that many bytes. Optional and json fields are not legal on
 * RF (-HP_E_INVAL). No allocation.
 *
 * A message is described by one hp_rf_val_t per schema field:
 *   u8/u16/u32/i32/bool/enum -> .u (i32 as its two's-complement bit pattern)
 *   str/bytes                -> .p/.len (str is not NUL-terminated on the wire)
 */

#include <stddef.h>
#include <stdint.h>
#include "proto/gen/harmony_proto.h"

typedef struct {
    uint32_t u;
    const uint8_t *p;
    size_t len;
} hp_rf_val_t;

/* Packs into out. -HP_E_TOOBIG: a str/bytes field > 255, out too small, or
 * total > HP_RF_MSG_MAX (15 fragments). -HP_E_INVAL: bad type/transport,
 * wrong nvals, out-of-range value. Never truncates. Returns 0, *out_len set. */
int hp_rf_pack(hp_msg_type_t t, const hp_rf_val_t *vals, size_t nvals, uint8_t *out, size_t cap,
               size_t *out_len);

/* Unpacks strictly (truncated, trailing bytes, bad bool/enum => -HP_E_INVAL).
 * vals must hold the message's field count; str/bytes .p point into `in`. */
int hp_rf_unpack(hp_msg_type_t t, const uint8_t *in, size_t len, hp_rf_val_t *vals, size_t nvals);

#endif
