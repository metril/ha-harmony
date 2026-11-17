#ifndef HP_IO_SINKS_H
#define HP_IO_SINKS_H

/* Injectable hardware interfaces. Each is a vtable plus an opaque `self`
 * passed back to every call. All return 0 (or a byte count where noted) on
 * success and -HP_E_* on failure. Engine calls take an hp_sinks_t explicitly;
 * nothing is reached through a global. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hub/ir/encode.h"

typedef struct hp_ir_sink {
    void *self;
    /* Emit sig->us[0 .. sig->once_n) (mark/space, mark first) modulated at sig->carrier_hz on
     * the ports in port_mask (bit n = port n). The repeat part of sig is the caller's business;
     * the engine passes a signal whose once-part is exactly what must go out. Must not block. */
    int (*send)(void *self, uint8_t port_mask, const hp_ir_signal_t *sig);
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
    /* Optional (NULL = unknown): is a remote attached right now? */
    bool (*attached)(void *self);
    /* Optional (NULL = the transport has no TX status): pops one pending TX-status report. Returns 1
     * and fills *slot / *status, or 0 when none is pending. Never blocks. */
    int (*tx_status)(void *self, uint8_t *slot, uint8_t *status);
} hp_rf_transport;

typedef struct {
    hp_ir_sink *ir;
    hp_bt_sink *bt;
    hp_rf_transport *rf;
} hp_sinks_t;

#endif
