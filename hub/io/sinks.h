#ifndef HP_IO_SINKS_H
#define HP_IO_SINKS_H

/* Injectable hardware interfaces. Each is a vtable plus an opaque `self`
 * passed back to every call. All return 0 (or a byte count where noted) on
 * success and -HP_E_* on failure. Engine calls take an hp_sinks_t explicitly;
 * nothing is reached through a global. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hp_ir_sink {
    void *self;
    /* Emit one Pronto code on the ports in port_mask (bit n = port n). */
    int (*send)(void *self, uint8_t port_mask, const uint16_t *pronto, size_t n);
} hp_ir_sink;

typedef struct hp_bt_sink {
    void *self;
    /* HID consumer-page usage key event. */
    int (*key)(void *self, uint16_t usage, bool down);
} hp_bt_sink;

typedef struct hp_rf_transport {
    void *self;
    int (*send)(void *self, const uint8_t *frame, size_t len);
    /* Returns frame length (>0), 0 on timeout, -HP_E_* on error. */
    int (*recv)(void *self, uint8_t *frame, size_t cap, int timeout_ms);
    /* Single-owner rule: first claim returns 0, any later one -HP_E_BUSY. */
    int (*claim)(void *self);
} hp_rf_transport;

typedef struct {
    hp_ir_sink *ir;
    hp_bt_sink *bt;
    hp_rf_transport *rf;
} hp_sinks_t;

#endif
