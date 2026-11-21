#ifndef HP_IO_HW_SINKS_H
#define HP_IO_HW_SINKS_H

/* Real hub hardware backend behind hp_sinks_t. Only the IR sink is implemented this phase
 * (/dev/i2s, wire format bench-proven 2026-11-16, docs/hardware/hub-ir-i2s.md section 9);
 * BT returns -HP_E_NOSYS. The RF radio is opt-in via cfg.rf (hub/io/rf_spi.h: preflight, reader
 * thread); without it the RF sink is a -HP_E_NOSYS stub. Every syscall and the clock go through hp_hw_ops_t so
 * the whole translation is host-testable with no device.
 *
 * PORT MASK. The engine's port_mask (hp_ir_sink contract: bit n = port n) maps one-to-one onto
 * the hardware `ports` field: START arg = (uint32)ports << 16, so engine bit n = START bit 16+n.
 * Bench result (2026-11-17, camera + TV reaction as the oracle): **START bit 16, engine mask 0x01,
 * is the built-in emitter.** An earlier session concluded bit 17 from a single TV state change;
 * that was a false positive (the TV wakes unprompted) and the sink shipped with the wrong default,
 * which is why it emitted nothing on hardware. Bit 17 was then re-tested and did nothing.
 * cfg.ir_ports is the set of ports wired to emitters (default 0x01). The effective ports are
 *   port_mask == 0   -> all of cfg.ir_ports      (an unset/default mask still reaches bit 16)
 *   otherwise        -> port_mask & cfg.ir_ports (the engine default 255 therefore == 0x01)
 * and an empty result is rejected with -HP_E_INVAL: never emit to nothing.
 *
 * BLOCKING. hp_ir_sink.send must not block, but DRAIN blocks for the whole waveform (324 ms
 * for one Samsung frame). Resolution: send() does everything up to and including START, then
 * returns with the fd held open (never closed, like stock hal; closing frees the DMA ring while
 * IRQ 23 may still fire and hard-locks the hub). DRAIN happens at the start of the next
 * send / at hp_hw_sinks_close(). hub/engine/irout.c already books the line busy for the
 * waveform duration plus a guard and defers further keyed sends, so in practice the previous
 * DRAIN is already complete when the next send runs. In the paths where irout sends
 * back-to-back and "the sink paces" (timer queue full), the leading DRAIN blocks for at most
 * one waveform: a bounded, degraded-mode exception, never the normal path. No threads. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "hub/io/rf_spi.h"
#include "hub/io/sinks.h"

#define HP_HW_RING_BYTES (512u * 768u) /* driver ring: a larger stream is rejected, never split */
#define HP_HW_DEFAULT_IR_PORTS 0x01u   /* engine bit 0 == START bit 16 == built-in emitter */
#define HP_HW_DEFAULT_DEV "/dev/i2s"

#define HP_I2S_VOLUME 0x80044e20u
#define HP_I2S_FREQ 0x80044e21u
#define HP_I2S_DSIZE 0x80044e22u
#define HP_I2S_MODE 0x80044e23u
#define HP_I2S_START 0x80044e30u
#define HP_I2S_DRAIN 0x80044e31u

/* Syscall seam. Failures return -1 with errno set, as the real calls do. */
typedef struct hp_hw_ops {
    void *ctx;
    int (*open)(void *ctx, const char *path, int flags);
    ssize_t (*write)(void *ctx, int fd, const void *buf, size_t n);
    int (*ioctl)(void *ctx, int fd, unsigned long req, unsigned long arg);
    int (*close)(void *ctx, int fd);
    uint64_t (*now_ms)(void *ctx); /* monotonic */
} hp_hw_ops_t;

typedef struct {
    const char *dev_path;       /* NULL = /dev/i2s */
    uint8_t ir_ports;           /* emitter ports wired; 0 = HP_HW_DEFAULT_IR_PORTS */
    const hp_hw_ops_t *ops;     /* NULL = real syscalls + CLOCK_MONOTONIC */
    const hp_rf_spi_cfg_t *rf;  /* non-NULL = also open the CC2544 radio (hub/io/rf_spi.h); NULL = RF stays a stub */
} hp_hw_cfg_t;

/* Derived wire parameters for one waveform. */
typedef struct {
    uint32_t period_ns, n, pulse, bit_ns;
    uint32_t divider; /* FREQ arg, 16.16 fixed point: bit_ns/40 */
    size_t nbits, nbytes;
    uint32_t dur_ms;  /* predicted playback time, rounded up */
} hp_hw_wave_t;

typedef struct {
    hp_sinks_t sinks;
    hp_ir_sink ir;
    hp_bt_sink bt;
    hp_rf_transport rf;
    hp_hw_cfg_t cfg;
    hp_hw_ops_t ops;
    bool rf_claimed;
    hp_rf_transport *rfspi; /* the real radio when cfg.rf was given; sinks.rf then points at it */
    bool pending;          /* START issued, DRAIN owed */
    uint8_t flush_ports;   /* nonzero: a failed send left queued bytes; START+DRAIN them before the next use */
    bool configured;       /* DSIZE/MODE/VOLUME done on fd */
    int fd;                /* /dev/i2s, opened on first send and held for the daemon's life (like hal); -1 before */
    uint8_t buf[HP_HW_RING_BYTES];
} hp_hw_t;

/* Pure builder (shared by send, tests): mark/space microseconds (mark first) -> byte-pair-swapped
 * bitstream, `repeat` copies, 33 % duty. -HP_E_INVAL (bad carrier/args) or -HP_E_TOOBIG
 * (does not fit `cap`; nothing partial is usable). */
int hp_hw_ir_build(const uint16_t *us, size_t n, uint32_t carrier_hz, unsigned repeat,
                   uint8_t *buf, size_t cap, hp_hw_wave_t *w);

/* Probes the device (write-open + close) so a held device fails at startup. Returns 0,
 * -HP_E_HALBUSY (EBUSY/EACCES: stock hal holds /dev/i2s), or -HP_E_IO / -HP_E_NOENT. */
int hp_hw_sinks_open(const hp_hw_cfg_t *cfg, hp_hw_t *out);
void hp_hw_sinks_close(hp_hw_t *h); /* drains (bounded by the waveform); never closes the fd */

#endif
