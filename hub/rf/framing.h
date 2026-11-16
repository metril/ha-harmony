#ifndef HP_RF_FRAMING_H
#define HP_RF_FRAMING_H

#include <stddef.h>
#include <stdint.h>
#include "proto/gen/harmony_proto.h"

/* Header (4 bytes): type u8, seq u8, frag u8 (index<<4 | total), len u8. */
#define HP_RF_HDR_LEN 4
#define HP_RF_MAX_FRAGS 15
#define HP_RF_FRAME_MAX 32
#define HP_RF_FRAG_PAYLOAD_MAX (HP_RF_FRAME_MAX - HP_RF_HDR_LEN)
#define HP_RF_MSG_MAX (HP_RF_MAX_FRAGS * HP_RF_FRAG_PAYLOAD_MAX)

#define HP_ASM_INCOMPLETE 0
#define HP_ASM_COMPLETE 1

typedef struct {
    uint8_t data[HP_RF_FRAME_MAX];
    size_t len;
} hp_rf_frame_t;

typedef struct {
    hp_msg_type_t type;
    uint8_t seq;
    size_t len;
    uint8_t payload[HP_RF_MSG_MAX];
} hp_rf_msg_t;

typedef struct {
    int active;
    uint8_t type;
    uint8_t seq;
    uint8_t total;
    uint16_t mask;
    uint8_t lens[HP_RF_MAX_FRAGS];
    uint8_t buf[HP_RF_MSG_MAX];
} hp_rf_asm_t;

/* frame_cap = total frame size (30 hub->remote, 32 remote->hub).
 * Returns 0, -HP_E_TOOBIG (needs >15 fragments or out_cap too small),
 * or -HP_E_INVAL (bad args / frame_cap). Never truncates. */
int hp_rf_split(hp_msg_type_t t, uint8_t seq, const uint8_t *payload, size_t len,
                size_t frame_cap, hp_rf_frame_t *out, size_t out_cap, size_t *out_n);

void hp_rf_asm_init(hp_rf_asm_t *a);

/* Returns HP_ASM_INCOMPLETE, HP_ASM_COMPLETE (out filled), or -HP_E_INVAL.
 * On error the assembler state is left unchanged. */
int hp_rf_asm_push(hp_rf_asm_t *a, const uint8_t *frame, size_t len, hp_rf_msg_t *out);

#endif
