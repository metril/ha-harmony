#ifndef HP_IR_ENCODE_H
#define HP_IR_ENCODE_H

/* IR encoders: protocol code / raw / Pronto -> mark/space timings.
 * Pure functions, no allocation, no globals. Errors are -HP_E_*.
 * Durations that do not fit in uint16 microseconds are rejected with
 * -HP_E_TOOBIG (never split or truncated). */

#include <stddef.h>
#include <stdint.h>

#define HP_IR_MAX_DUR 512 /* once + repeat durations per signal */

typedef enum {
    HP_IR_NEC = 0,
    HP_IR_NECEXT,
    HP_IR_SAMSUNG32,
    HP_IR_RC5,
    HP_IR_RC6,
    HP_IR_SIRC12,
    HP_IR_SIRC15,
    HP_IR_SIRC20,
    HP_IR_PROTO_COUNT
} hp_ir_proto_t;

typedef enum { HP_IR_CODE_PROTO = 0, HP_IR_CODE_RAW, HP_IR_CODE_PRONTO } hp_ir_kind_t;

typedef struct {
    hp_ir_kind_t kind;
    union {
        struct { hp_ir_proto_t proto; uint32_t addr, cmd; } p;
        struct { uint32_t hz; const uint32_t *us; size_t n; } raw;
        struct { const uint16_t *words; size_t n; } pronto;
    } u;
} hp_ir_code_t;

typedef struct {
    uint32_t carrier_hz;
    uint16_t once_n, rep_n;
    /* Nominal start-to-start frame/repeat period in us (0 = unspecified: raw,
     * Pronto). The trailing gap in `us` is clamped to 65535 us, so a sink that
     * wants exact spacing idles for period_us minus the emitted duration. */
    uint32_t period_us;
    /* once-sequence then repeat-sequence; alternating mark/space, mark first */
    uint16_t us[HP_IR_MAX_DUR];
} hp_ir_signal_t;

/* toggle: RC5/RC6 toggle bit (nonzero = 1); ignored by other protocols.
 * On failure *out is zeroed. */
int hp_ir_encode(const hp_ir_code_t *code, uint8_t toggle, hp_ir_signal_t *out);

/* Parse "0000 006D ..." (1-4 hex digits per word, whitespace separated).
 * -HP_E_INVAL on bad text/empty, -HP_E_TOOBIG if more than cap words. */
int hp_ir_parse_pronto(const char *s, uint16_t *words, size_t cap, size_t *n);

const char *hp_ir_proto_name(hp_ir_proto_t p);                 /* NULL if invalid */
int hp_ir_proto_from_name(const char *name, hp_ir_proto_t *p); /* 0 / -HP_E_NOENT */

#endif
