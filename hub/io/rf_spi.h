#ifndef HP_IO_RF_SPI_H
#define HP_IO_RF_SPI_H

/* hp_rf_transport over /dev/rfspi, the hub's CC2544 radio (docs/hardware/hub-rf-cc2544.md).
 * Every syscall goes through hp_rf_spi_ops_t, so each path is host-testable.
 *
 * ROLES. The same driver and link layer run on the hub (HP_RF_SPI_ROLE_HUB, the default) and on the
 * Elite remote (HP_RF_SPI_ROLE_REMOTE); the role picks codec, report id, default slot, write cap and
 * pairing behaviour. Remote role:
 *  - frames are 32 bytes both ways: `0x20, (slot & 0x0F) | powermode, 0x12, slot, inner[28]`; recv()
 *    accepts only such 0x20 reports (a 0x12 report is dropped), and the first one marks attached.
 *  - HARD 32-BYTE WRITE CAP. The remote's /dev/rfspi memcpys `count` bytes into a 32-byte kernel slab
 *    with NO check, so a longer write overflows kernel heap. Every write goes through one site that
 *    refuses n > 32 with -HP_E_TOOBIG before the syscall (plus a _Static_assert on the frame size).
 *  - `powermode`: the stock value of the power-mode byte was NOT recovered. The default 0x00 is
 *    UNVALIDATED; only the upper nibble is used. The remote app exposes it as a command-line option
 *    so it can be tried at the bench without a rebuild.
 *  - the slot is learned from the link-up report (measured: the hub is slot 1 on the remote, wpid 0x880B,
 *    status 0x07); a configured slot overrides it.
 *  - hp_rf_spi_set_pairing / hp_rf_spi_unpair are host-chip operations: -HP_E_INVAL, nothing written.
 *  - the init command `10 FF 80 00 00 01 00` is sent in both roles (measured on the remote's chip).
 *  - link-status `10 <slot> 41 04 <status>` is assumed to have the same shape as on the hub; this is
 *    UNVALIDATED on the remote.
 * READER. Received entries queue in kernel heap even with no opener, so the reader thread runs for
 * the whole time the device is open (from open() to destroy()) in both roles; do not stop it early.
 *
 * SLOT. `cfg.slot` != 0 fixes the slot (only that slot's reports are used, as before). With 0 the slot is
 * learned from the link-up report `10 NN 41 04 <status> <wpid>` (status 0x87 / wpid 0x4021 on the hub,
 * 0x07 / 0x880B on the remote): taken only while detached, so a re-link on another slot is picked up.
 * Until it is known data and TX statuses are dropped (debug line) and send() is -HP_E_NOENT with no write.
 *
 * SAFETY. Opens only /dev/rfspi (and /proc, for the preflight): never /dev/rffw, the chip-flash
 * programmer. Opening is refused, fail closed, while stock `hal` is alive OR when that cannot be
 * determined (-HP_E_HALBUSY; run `tools/deploy_hub.py quiesce`). Pairing / unpair commands are
 * written only by the explicit hp_rf_spi_set_pairing / hp_rf_spi_unpair calls.
 *
 * Frames. This transport carries bare 28-byte inner blocks (hub/rf/framing.h): send() wraps one in
 * the 30-byte hub-mode report `0x12, slot, inner[28]`; recv() returns the inner block of a received
 * 0x12 report (hp_rf_link_t with wrapped=false). A shorter send() is zero padded.
 *
 * Semantics (rf_pair.c's contract): claim() 0 once, then -HP_E_BUSY; send()/recv() are -HP_E_INVAL
 * before the claim; send() over 28 bytes is -HP_E_TOOBIG; recv() returns the length, 0 on timeout,
 * -HP_E_NOENT exactly once when the link goes from attached to lost, IN ORDER with the frames (a marker in
 * the receive queue: frames received before the loss come first, those of a re-attached link follow it).
 * attached() is false until a link-up status or a data frame arrives, and false again on a
 * `10 <slot> 41 04 <status>` report with status & 0x40.
 *
 * TX STATUS. A 32-byte `0x20 <slot> 0x42 <status> ...` report is the radio's TX-status for a frame
 * (measured on the hub; its 00 / 01 semantics are unknown, so they are only passed on). It is not
 * data and not queued for recv(): it is surfaced through hp_rf_transport.tx_status (hub role: only
 * for the configured slot; remote role: same shape, any slot, byte1 & 0x0F). A link-status change
 * (`10 <slot> 41 04 <status>`) is logged once per transition ("link up (slot 2, status 0x87, wpid
 * 4021)" / "link lost (status 0x47)").
 *
 * write() returns 0 (normal) or a positive received length for success, negative for error. After
 * a negative return (-EIO: the chip's ready line is stuck) writes are suppressed (-HP_E_BUSY, no
 * syscall) for 50 ms, doubling to 1 s while errors continue: the radio shares the NOR-flash SPI
 * bus. A single reader thread does read(fd, buf[128], 128) forever; destroy interrupts it with
 * SIGUSR1 (no SA_RESTART) and joins it. */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "hub/io/sinks.h"

#define HP_RF_SPI_DEV "/dev/rfspi"
#define HP_RF_SPI_REMOTE_WRITE_CAP 32 /* the remote driver's kernel slab: see ROLES */
#define HP_RF_SPI_BACKOFF_MIN_MS 50
#define HP_RF_SPI_BACKOFF_MAX_MS 1000

typedef struct hp_rf_spi_ops {
    void *ctx;
    int (*open)(void *ctx, const char *path, int flags);
    ssize_t (*write)(void *ctx, int fd, const void *buf, size_t n);
    ssize_t (*read)(void *ctx, int fd, void *buf, size_t cap); /* blocks; -1/EINTR when signalled */
    int (*close)(void *ctx, int fd);
    /* 1 = stock hal alive, 0 = not, -1 = cannot tell. NULL counts as "cannot tell". */
    int (*hal_alive)(void *ctx);
    uint64_t (*now_ms)(void *ctx); /* monotonic */
    /* Wake the reader blocked in read(): the default is pthread_kill(t, SIGUSR1). */
    void (*interrupt)(void *ctx, pthread_t reader);
} hp_rf_spi_ops_t;

typedef enum { HP_RF_SPI_ROLE_HUB = 0, HP_RF_SPI_ROLE_REMOTE = 1 } hp_rf_spi_role_t;

typedef struct {
    const char *dev_path;        /* NULL = /dev/rfspi */
    uint8_t slot;                /* peer slot override; 0 = learn it from the link-up report (hub 1..15, remote 0..15) */
    const hp_rf_spi_ops_t *ops;  /* NULL = real syscalls */
    hp_rf_spi_role_t role;       /* default HP_RF_SPI_ROLE_HUB */
    uint8_t powermode;           /* remote role only: upper-nibble value OR'd into frame byte 1 */
    bool debug;                  /* log every write and read (ms since open, TX/RX, rc/len, hex) */
    void (*log)(void *ctx, const char *line); /* sink for debug and link-transition lines; NULL = stderr */
    void *log_ctx;
} hp_rf_spi_cfg_t;

/* The real syscalls (hal preflight walks /proc). Tests copy it and override fields. */
hp_rf_spi_ops_t hp_rf_spi_real_ops(void);

/* Preflight, open, init command, start the reader. 0, -HP_E_HALBUSY (hal alive or unknown, or
 * EBUSY/EACCES on open), -HP_E_NOENT, -HP_E_IO. */
int hp_rf_spi_open(const hp_rf_spi_cfg_t *cfg, hp_rf_transport **out);
/* Stops the reader (signal + join), closes the device, frees. NULL-safe. */
void hp_rf_spi_destroy(hp_rf_transport *t);
/* Readable when recv() has something (frame or link-loss) to return; -1 if t is not an rf_spi. */
int hp_rf_spi_wake_fd(const hp_rf_transport *t);
/* The peer slot in use (configured or learned), -1 while unknown or if t is not an rf_spi. */
int hp_rf_spi_slot(const hp_rf_transport *t);

/* Test seam: the guarded single write site, so a test can prove an over-long write never reaches
 * the syscall. Not for production use. */
int hp_rf_spi_write_for_test(hp_rf_transport *t, const uint8_t *buf, size_t n);

/* Explicit-only radio commands. 0, -HP_E_INVAL, -HP_E_BUSY (write backoff), -HP_E_IO. */
int hp_rf_spi_set_pairing(hp_rf_transport *t, unsigned seconds); /* 10 FF 80 B2 01 00 <secs>, secs 1..255 */
int hp_rf_spi_unpair(hp_rf_transport *t, unsigned slot);         /* 10 FF 80 B2 03 <slot> 00, slot 1..15 */

#endif
