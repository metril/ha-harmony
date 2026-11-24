#ifndef HP_REMOTE_SYS_H
#define HP_REMOTE_SYS_H

/* Syscall seam for the remote app. Every device access goes through ra_sys_t, which enforces the
 * brick-safety allow-list before the ops table is reached (tests assert it over the syscall log).
 *
 * Allowed: open /dev/fb0 O_RDWR; /dev/input/event0 and event2 O_RDONLY; the backlight and key-LED
 * brightness nodes O_WRONLY; the three power nodes O_RDONLY; the boot-counter file named by
 * HARMONY_BOOTCOUNT_FILE (write); /sys/power/state O_WRONLY, only through ra_suspend (writes the msp430 `wakeup` seconds, then "mem"); wakeup_event O_RDONLY, only through ra_wake_event. ioctl: the two fb GETs,
 * plus FBIOBLANK with FB_BLANK_UNBLANK (ra_fb_unblank: the panel stays dark until it is issued) or FB_BLANK_POWERDOWN
 * (ra_fb_blank, before a suspend); never EVIOCGRAB, FBIOPUT_*, FBIOPAN, any other blank level. mmap: the fb fd only. exec: /sbin/poweroff, only from the battery guard.
 * Never: /dev/rfspi (Task 3 owns it via its own module), /dev/rffw, /dev/mspfw, /dev/watchdog,
 * /dev/mtd*, any CPU-frequency node (a frequency change can cut an I2C transfer and hang the remote), any kill or charger node. */
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define RA_PATH_FB "/dev/fb0"
#define RA_PATH_KEYS "/dev/input/event0"
#define RA_PATH_TOUCH "/dev/input/event2"
#define RA_PATH_MOTION "/dev/input/event1" /* LIS3DH accelerometer, INT1 on GPIO 108: one report per motion interrupt */
#define RA_PATH_BACKLIGHT "/sys/class/backlight/ap3156_backlight/brightness"
#define RA_PATH_LED "/sys/class/leds/tps6507x:white/brightness"
#define RA_PATH_SOC "/sys/bus/i2c/devices/1-0070/soc"
#define RA_PATH_POWERSRC "/sys/bus/i2c/devices/1-0048/tps6507x-juniper/powersource"
#define RA_PATH_BATTCHARGE "/sys/bus/i2c/devices/1-0048/tps6507x-juniper/battcharge"
#define RA_POWEROFF_PATH "/sbin/poweroff"
#define RA_PATH_WAKEUP "/sys/bus/i2c/devices/1-0034/wakeup"           /* msp430 RTC alarm: seconds from now; written only by ra_suspend */
#define RA_PATH_WAKEEVENT "/sys/bus/i2c/devices/1-0034/wakeup_event" /* what woke it: none|key|alarm|touch; read only by ra_wake_event */
#define RA_SUSPEND_WAKE_S 3600                                        /* hourly timed wake so the battery guard runs (upstream's value) */
#define RA_PATH_POWERSTATE "/sys/power/state"

typedef struct ra_ops {
  void *ctx;
  int (*open)(void *ctx, const char *path, int flags, int mode);
  int (*close)(void *ctx, int fd);
  ssize_t (*read)(void *ctx, int fd, void *buf, size_t n);
  ssize_t (*write)(void *ctx, int fd, const void *buf, size_t n);
  int (*ioctl)(void *ctx, int fd, unsigned long req, void *arg);
  void *(*mmap)(void *ctx, int fd, size_t len); /* shared RW mapping at offset 0; NULL on failure */
  int (*munmap)(void *ctx, void *p, size_t len);
  /* Wait up to timeout_ms for fds[0..n) to be readable; ready[i] set. Returns the number ready, 0 on
   * timeout, -1 on error (errno set; the caller retries EINTR). */
  int (*wait)(void *ctx, const int *fds, int n, unsigned timeout_ms, int *ready);
  uint64_t (*now_ms)(void *ctx); /* monotonic */
  int (*exec_poweroff)(void *ctx, const char *path); /* spawn without waiting; 0 on success */
  void (*log)(void *ctx, const char *msg);
  uint64_t (*wall_ms)(void *ctx); /* optional (NULL = now_ms). Wall clock: keeps counting through a suspend, unlike CLOCK_MONOTONIC */
} ra_ops_t;

typedef struct {
  ra_ops_t ops;
  const char *bootcount_path; /* NULL unless HARMONY_BOOTCOUNT_FILE was set */
  int fb_fd;                  /* -1 until /dev/fb0 is open */
} ra_sys_t;

extern const ra_ops_t ra_real_ops; /* real_ops.c */

void ra_sys_init(ra_sys_t *s, const ra_ops_t *ops, const char *bootcount_path);

/* Each returns -1 with errno = EPERM when the allow-list refuses. */
int ra_open_fb(ra_sys_t *s); /* O_RDWR */
int ra_open_input(ra_sys_t *s, const char *p); /* event0 / event2, O_RDONLY */
int ra_ioctl_fb(ra_sys_t *s, unsigned long req, void *arg);
/* FBIOBLANK(FB_BLANK_UNBLANK): the Elite's LCD shows nothing until this is issued (pan alone does nothing). */
int ra_fb_unblank(ra_sys_t *s);
/* FBIOBLANK(FB_BLANK_POWERDOWN) when blank, else UNBLANK. */
int ra_fb_blank(ra_sys_t *s, int blank);
void *ra_mmap_fb(ra_sys_t *s, size_t len);
ssize_t ra_read(ra_sys_t *s, int fd, void *buf, size_t n);
int ra_close(ra_sys_t *s, int fd);
/* Write a small ASCII string to one of the two writable sysfs nodes / read one of the power nodes. */
int ra_sysfs_write(ra_sys_t *s, const char *path, const char *val);
int ra_sysfs_read(ra_sys_t *s, const char *path, char *buf, size_t n); /* NUL-terminated, <0 on error */
int ra_write_bootcount(ra_sys_t *s);
/* Arm the msp430 timed wake (RA_SUSPEND_WAKE_S seconds; -1 and no suspend if that fails: a remote that sleeps with no alarm
 * never runs the battery guard), then write "mem" to /sys/power/state: returns when the kernel resumes (0), or -1 with errno. */
int ra_suspend(ra_sys_t *s);
/* Read the msp430 wakeup_event (what ended the last suspend) into buf, trimmed; -1 if unreadable. */
int ra_wake_event(ra_sys_t *s, char *buf, size_t n);
uint64_t ra_wall_ms(ra_sys_t *s);
int ra_poweroff(ra_sys_t *s); /* battery guard only */
void ra_logf(ra_sys_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
