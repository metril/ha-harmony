#ifndef HP_IO_RF_SPI_H
#define HP_IO_RF_SPI_H

/* hp_rf_transport over /dev/rfspi, the hub's CC2544 radio (docs/hardware/hub-rf-cc2544.md).
 * Every syscall goes through hp_rf_spi_ops_t, so each path is host-testable.
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
 * -HP_E_NOENT exactly once when the link goes from attached to lost (queued frames come first).
 * attached() is false until a link-up status or a data frame arrives, and false again on a
 * `10 <slot> 41 04 <status>` report with status & 0x40.
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
#define HP_RF_SPI_SLOT 1 /* the single paired remote (measured pipe table) */
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

typedef struct {
    const char *dev_path;        /* NULL = /dev/rfspi */
    uint8_t slot;                /* remote's slot; 0 = HP_RF_SPI_SLOT */
    const hp_rf_spi_ops_t *ops;  /* NULL = real syscalls */
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

/* Explicit-only radio commands. 0, -HP_E_INVAL, -HP_E_BUSY (write backoff), -HP_E_IO. */
int hp_rf_spi_set_pairing(hp_rf_transport *t, unsigned seconds); /* 10 FF 80 B2 01 00 <secs>, secs 1..255 */
int hp_rf_spi_unpair(hp_rf_transport *t, unsigned slot);         /* 10 FF 80 B2 03 <slot> 00, slot 1..15 */

#endif
