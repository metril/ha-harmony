/* Host-only tests for hub/io/hw_sinks.c: every syscall and the clock are injected.
 * argv[1] = path to the ir_bench binary, whose builder is the proven reference: the byte stream
 * must match its --out dump exactly, so the two cannot drift. Timings come from
 * backups/phase1/hub/20261116T234902Z/extracted/ir_codes.json (gitignored, so embedded). */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hub/hp_errors.h"
#include "hub/io/hw_sinks.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* Samsung PowerToggle, device 51886472 (GoVideoO1 32 Bit, 37900 Hz): one frame, 68 durations. */
static const uint16_t SAMSUNG[] = {
    4500, 4500, 560, 1690, 560, 1690, 560, 1690, 560, 560, 560, 560, 560, 560, 560, 560, 560, 560,
    560, 1690, 560, 1690, 560, 1690, 560, 560, 560, 560, 560, 560, 560, 560, 560, 560, 560, 560,
    560, 1690, 560, 560, 560, 560, 560, 560, 560, 560, 560, 560, 560, 560, 560, 1690, 560, 560,
    560, 1690, 560, 1690, 560, 1690, 560, 1690, 560, 1690, 560, 1690, 560, 46780};
/* Device 64694265 InputA1 (Toshiba 32 Bit, 38000 Hz). The extracted file has no "Audioengine"
 * device; this is the other-carrier IR code. */
static const uint16_t TOSHIBA[] = {
    8990, 4490, 568, 552, 568, 552, 568, 552, 568, 552, 568, 552, 568, 552, 568, 552, 568, 552,
    568, 1662, 568, 1662, 568, 1662, 568, 1662, 568, 1662, 568, 1662, 568, 1662, 568, 1662, 568,
    552, 568, 552, 568, 1662, 568, 1662, 568, 1662, 568, 552, 568, 1662, 568, 552, 568, 1662, 568,
    1662, 568, 552, 568, 552, 568, 552, 568, 1662, 568, 552, 568, 1662, 568, 40222};
/* Synthetic: no extracted code is outside 37.9/38 kHz, so 100 kHz (period 10000 ns -> N=10). */
static const uint16_t SYN100[] = {1000, 1000, 500, 20000};
#define LEN(a) (sizeof(a) / sizeof((a)[0]))

static uint32_t fnv(const uint8_t *b, size_t n) {
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *b++) * 16777619u;
    return h;
}

/* --- fake device ------------------------------------------------------------------------ */

enum { MAXLOG = 64, WCAP = 1 << 20 };
typedef struct {
    int open_errno;          /* nonzero: open fails with it */
    int closes, opens;
    int nlog;
    struct { char op; unsigned long req, arg; } log[MAXLOG]; /* o open, i ioctl, w write, c close */
    int wsteps[16], nsteps, step; /* write script: >0 limit, 0 = fail EIO, -1 = EINTR */
    unsigned long fail_req;          /* ioctl request that fails (0 = none) */
    uint8_t *wbuf;
    size_t wlen;
    uint64_t now;
} fake_t;

static void lg(fake_t *f, char op, unsigned long req, unsigned long arg) {
    if (f->nlog < MAXLOG) { f->log[f->nlog].op = op; f->log[f->nlog].req = req; f->log[f->nlog].arg = arg; f->nlog++; }
}
static int f_open(void *c, const char *p, int fl) {
    fake_t *f = c;
    (void)p; (void)fl;
    lg(f, 'o', 0, 0);
    if (f->open_errno) { errno = f->open_errno; return -1; }
    f->opens++;
    return 7;
}
static ssize_t f_write(void *c, int fd, const void *b, size_t n) {
    fake_t *f = c;
    (void)fd;
    int s = f->step < f->nsteps ? f->wsteps[f->step++] : (int)n;
    if (s < 0) { errno = EINTR; return -1; }
    if (s == 0) { errno = EIO; return -1; }
    if ((size_t)s < n) n = (size_t)s;
    lg(f, 'w', n, 0);
    if (f->wlen + n <= WCAP) memcpy(f->wbuf + f->wlen, b, n);
    f->wlen += n;
    return (ssize_t)n;
}
static int f_ioctl(void *c, int fd, unsigned long r, unsigned long a) {
    fake_t *f = c;
    (void)fd;
    lg(f, 'i', r, a);
    if (r == f->fail_req) { errno = EIO; return -1; }
    return 0;
}
static int f_close(void *c, int fd) { fake_t *f = c; (void)fd; lg(f, 'c', 0, 0); f->closes++; return 0; }
static uint64_t f_now(void *c) { return ((fake_t *)c)->now; }

static hp_hw_t *g_h; /* ~384 KiB: keep off the stack */
static fake_t F;
static hp_hw_ops_t OPS;

static void setup(void) {
    uint8_t *wb = F.wbuf;
    memset(&F, 0, sizeof F);
    F.wbuf = wb ? wb : malloc(WCAP);
    OPS = (hp_hw_ops_t){&F, f_open, f_write, f_ioctl, f_close, f_now};
}
static int open_hw(uint8_t ports) {
    hp_hw_cfg_t c = {NULL, ports, &OPS, NULL};
    int r = hp_hw_sinks_open(&c, g_h);
    F.nlog = 0; F.opens = F.closes = 0; /* forget the startup probe */
    return r;
}
static hp_ir_signal_t *mk(const uint16_t *us, size_t n, uint32_t hz) {
    static hp_ir_signal_t s;
    memset(&s, 0, sizeof s);
    memcpy(s.us, us, n * sizeof *us);
    s.once_n = (uint16_t)n;
    s.carrier_hz = hz;
    return &s;
}
static int send_ir(uint8_t mask, hp_ir_signal_t *s) { return g_h->sinks.ir->send(g_h->sinks.ir->self, mask, s); }

/* --- ir_bench reference ----------------------------------------------------------------- */

static const char *g_bench;
static int bench_dump(uint32_t hz, const uint16_t *us, size_t n, int repeat, uint8_t *out, size_t *len) {
    char path[] = "/tmp/hp_hw_test_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    close(fd);
    char *cmd = malloc(n * 7 + 512);
    int p = sprintf(cmd, "ASAN_OPTIONS=detect_leaks=0 %s --dry-run --carrier %u --repeat %d --out %s --durations ", g_bench, hz, repeat, path);
    for (size_t i = 0; i < n; i++) p += sprintf(cmd + p, i ? ",%u" : "%u", us[i]);
    strcat(cmd, " >/dev/null");
    int rc = system(cmd);
    free(cmd);
    FILE *f = fopen(path, "rb");
    *len = f ? fread(out, 1, HP_HW_RING_BYTES, f) : 0;
    if (f) fclose(f);
    unlink(path);
    return rc;
}

static void golden(const char *name, const uint16_t *us, size_t n, uint32_t hz, int repeat, uint32_t nbits,
                   uint32_t nbytes, uint32_t div, uint32_t Nn, uint32_t bit_ns, uint32_t hash) {
    static uint8_t mine[HP_HW_RING_BYTES], ref[HP_HW_RING_BYTES];
    hp_hw_wave_t w;
    size_t rl = 0;
    CHECK(hp_hw_ir_build(us, n, hz, (unsigned)repeat, mine, sizeof mine, &w) == 0);
    if (getenv("HW_PRINT")) printf("%s r=%d bits=%zu bytes=%zu div=0x%08x N=%u bit=%u hash=0x%08x\n", name, repeat, w.nbits, w.nbytes, w.divider, w.n, w.bit_ns, fnv(mine, w.nbytes));
    CHECK(w.nbits == nbits && w.nbytes == nbytes && w.divider == div && w.n == Nn && w.bit_ns == bit_ns);
    CHECK(fnv(mine, w.nbytes) == hash);
    CHECK(bench_dump(hz, us, n, repeat, ref, &rl) == 0);
    CHECK(rl == w.nbytes && memcmp(ref, mine, rl) == 0); /* identical to the proven ir_bench builder */
}

static void test_golden(void) {
    /* PowerToggle x3 is exactly the bench run that turned the TV on. */
    golden("samsung", SAMSUNG, LEN(SAMSUNG), 37900, 3, 147392, 18424, 0x0036f999, 12, 2199, 0x0a33af8d);
    golden("samsung", SAMSUNG, LEN(SAMSUNG), 37900, 1, 49152, 6144, 0x0036f999, 12, 2199, 0xaf77ed7d);
    golden("toshiba", TOSHIBA, LEN(TOSHIBA), 38000, 1, 49232, 6154, 0x0036d333, 12, 2193, 0xffdd5435);
    golden("syn100", SYN100, LEN(SYN100), 100000, 1, 22544, 2818, 0x00190000, 10, 1000, 0xa17ce287);
    /* The N table has five branches and the stock database only ever reaches N=12 (every extracted
     * code is 37.9/38 kHz). The rest are reachable only through hand-written `raw: {hz, us}` config
     * and the Flipper/Pronto/SmartIR importers, so they get golden vectors too. 455 kHz is Bang &
     * Olufsen's real carrier; 350/420 kHz stand in for the N=8 and N=4 branches. */
    golden("n8_350k", SYN100, LEN(SYN100), 350000, 1, 63040, 7880, 0x0008f333, 8, 358, 0xc9f2cf85);
    golden("n4_420k", SYN100, LEN(SYN100), 420000, 1, 37840, 4730, 0x000ee666, 4, 596, 0x2aba0871);
    /* N=2 quantises duty to 50 %: pulse = (33*2+99)/100 = 1 of 2 bits. Nothing nearer to 33 % exists
     * at this carrier, so a 455 kHz mark is a square wave by construction, not by a bug. */
    golden("n2_455k", SYN100, LEN(SYN100), 455000, 1, 20512, 2564, 0x001b7999, 2, 1099, 0xad385ff5);
}

/* Every boundary of the N table, from both sides. A wrong comparison here silently changes the
 * carrier of any imported code near the edge, and no stock code would ever catch it. */
static void test_carrier_branches(void) {
    static const struct { uint32_t hz, x, n; } B[] = {
        {444346, 2250, 2},  {444149, 2251, 4},   /* X < 2251  */
        {399921, 2500, 4},  {399761, 2501, 8},   /* X < 2501  */
        {333389, 2999, 8},  {333278, 3000, 10},  /* X < 3000  */
        { 83337, 11999, 10}, {83330, 12000, 12}, /* X < 12000 */
    };
    static uint8_t buf[HP_HW_RING_BYTES];
    hp_hw_wave_t w;
    for (size_t i = 0; i < LEN(B); i++) {
        CHECK(hp_hw_ir_build(SYN100, LEN(SYN100), B[i].hz, 1, buf, sizeof buf, &w) == 0);
        CHECK(w.period_ns == B[i].x && w.n == B[i].n);
        /* bit_ns is the carrier period split N ways, rounded up, and the divider tracks it. */
        CHECK(w.bit_ns == (B[i].x + B[i].n - 1) / B[i].n);
        CHECK(w.divider == (uint32_t)(((uint64_t)w.bit_ns << 16) / 40));
    }
    /* Outside the supported range nothing is built. */
    CHECK(hp_hw_ir_build(SYN100, LEN(SYN100), 8999, 1, buf, sizeof buf, &w) == -HP_E_INVAL);
    CHECK(hp_hw_ir_build(SYN100, LEN(SYN100), 500001, 1, buf, sizeof buf, &w) == -HP_E_INVAL);
}

static void test_sequence(void) {
    setup();
    CHECK(open_hw(0) == 0);
    hp_ir_signal_t *s = mk(SAMSUNG, LEN(SAMSUNG), 37900);
    F.now = 1000;
    CHECK(send_ir(0, s) == 0); /* port_mask 0 -> default emitter */
    hp_hw_wave_t w;
    static uint8_t ref[HP_HW_RING_BYTES];
    CHECK(hp_hw_ir_build(s->us, s->once_n, 37900, 1, ref, sizeof ref, &w) == 0);
    const unsigned long exp[][2] = {{HP_I2S_DSIZE, 16}, {HP_I2S_MODE, 2}, {HP_I2S_VOLUME, 15},
                                    {HP_I2S_FREQ, 0x0036f999}};
    CHECK(F.log[0].op == 'o');
    for (int i = 0; i < 4; i++) CHECK(F.log[1 + i].op == 'i' && F.log[1 + i].req == exp[i][0] && F.log[1 + i].arg == exp[i][1]);
    CHECK(F.log[5].op == 'w' && F.log[5].req == w.nbytes);
    CHECK(F.log[6].op == 'i' && F.log[6].req == HP_I2S_START && F.log[6].arg == 0x10000);
    CHECK(F.nlog == 7 && F.closes == 0); /* send returns before DRAIN: it must not block */
    CHECK(F.wlen == w.nbytes && memcmp(F.wbuf, ref, w.nbytes) == 0);
    /* not yet reaped before the predicted end + margin; reaped after */
    F.now = 1000 + w.dur_ms + HP_HW_REAP_MARGIN_MS - 1;
    hp_hw_service(g_h);
    CHECK(F.nlog == 7);
    F.now++;
    hp_hw_service(g_h);
    CHECK(F.nlog == 9 && F.log[7].op == 'i' && F.log[7].req == HP_I2S_DRAIN && F.log[7].arg == 0 && F.log[8].op == 'c');
    CHECK(F.closes == 1 && F.opens == 1);
    CHECK(w.dur_ms == 109); /* one frame: 49152 bits x 2199 ns (the bench's 324.1 ms was 3 repeats) */
    /* a second send while one is pending drains+closes the first before opening */
    F.nlog = 0;
    CHECK(send_ir(0, s) == 0);
    F.nlog = 0; F.now += 5000;
    CHECK(send_ir(0, s) == 0);
    CHECK(F.log[0].op == 'i' && F.log[0].req == HP_I2S_DRAIN && F.log[1].op == 'c' && F.log[2].op == 'o');
    hp_hw_sinks_close(g_h);
    CHECK(F.log[F.nlog - 2].req == HP_I2S_DRAIN && F.log[F.nlog - 1].op == 'c');
}

static void test_ports(void) {
    setup();
    CHECK(open_hw(0) == 0);
    hp_ir_signal_t *s = mk(SYN100, LEN(SYN100), 100000);
    CHECK(send_ir(255, s) == 0); /* engine default mask -> emitter only */
    /* START bit 16 is the built-in emitter, proven on hardware 2026-11-17 (camera + TV). */
    CHECK(F.log[F.nlog - 1].arg == 0x10000);
    hp_hw_sinks_close(g_h);
    F.nlog = 0;
    CHECK(send_ir(2, s) == -HP_E_INVAL && F.nlog == 0); /* port 1 is not wired: never emit to nothing */
    setup();
    CHECK(open_hw(0x06) == 0); /* --ir-ports 6: bits 1 and 2 wired */
    CHECK(send_ir(0, s) == 0 && F.log[F.nlog - 1].arg == 0x60000);
    hp_hw_sinks_close(g_h);
    CHECK(send_ir(0x04, s) == 0 && F.log[F.nlog - 1].arg == 0x40000);
    hp_hw_sinks_close(g_h);
}

static void test_writes(void) {
    static uint8_t ref[HP_HW_RING_BYTES];
    hp_hw_wave_t w;
    hp_ir_signal_t *s = mk(SAMSUNG, LEN(SAMSUNG), 37900);
    CHECK(hp_hw_ir_build(s->us, s->once_n, 37900, 1, ref, sizeof ref, &w) == 0);
    /* short + partial + EINTR + a zero-progress-free sequence: all retried, stream intact */
    setup();
    F.wsteps[0] = 1000; F.wsteps[1] = -1; F.wsteps[2] = 17; F.wsteps[3] = -1; F.nsteps = 4;
    CHECK(open_hw(0) == 0);
    CHECK(send_ir(0, s) == 0);
    CHECK(F.wlen == w.nbytes && memcmp(F.wbuf, ref, w.nbytes) == 0);
    CHECK(F.log[F.nlog - 1].req == HP_I2S_START);
    hp_hw_sinks_close(g_h);
    /* write error mid-stream: error returned, fd closed, no START */
    setup();
    F.wsteps[0] = 500; F.wsteps[1] = 0; F.nsteps = 2;
    CHECK(open_hw(0) == 0);
    CHECK(send_ir(0, s) == -HP_E_IO);
    CHECK(F.closes == 1 && F.log[F.nlog - 1].op == 'c');
    for (int i = 0; i < F.nlog; i++) CHECK(!(F.log[i].op == 'i' && F.log[i].req == HP_I2S_START));
    /* a failed ioctl also closes */
    setup();
    F.fail_req = HP_I2S_FREQ;
    CHECK(open_hw(0) == 0);
    CHECK(send_ir(0, s) == -HP_E_IO && F.closes == 1 && F.wlen == 0);
    /* and a later send still works (no stuck pending state) */
    F.fail_req = 0; F.nlog = 0;
    CHECK(send_ir(0, s) == 0 && F.wlen == w.nbytes);
    hp_hw_sinks_close(g_h);
}

static void test_oversize(void) {
    setup();
    CHECK(open_hw(0) == 0);
    /* 512 x 65535 us = 33.5 s: far beyond the 384 KiB ring (about 6.9 s at 2199 ns/bit) */
    static uint16_t big[HP_IR_MAX_DUR];
    for (int i = 0; i < HP_IR_MAX_DUR; i++) big[i] = 65535;
    hp_ir_signal_t *s = mk(big, HP_IR_MAX_DUR, 37900);
    CHECK(send_ir(0, s) == -HP_E_TOOBIG);
    CHECK(F.nlog == 0 && F.wlen == 0);
    hp_ir_signal_t *bad = mk(SAMSUNG, 0, 37900);
    CHECK(send_ir(0, bad) == -HP_E_INVAL);
    s = mk(SYN100, LEN(SYN100), 0);
    CHECK(send_ir(0, s) == -HP_E_INVAL && F.nlog == 0);
}

static void test_open_errors(void) {
    setup();
    F.open_errno = EBUSY;
    CHECK(open_hw(0) == -HP_E_HALBUSY);
    setup();
    F.open_errno = EACCES;
    CHECK(open_hw(0) == -HP_E_HALBUSY);
    setup();
    F.open_errno = ENOENT;
    CHECK(open_hw(0) == -HP_E_NOENT);
    /* hal grabs the device after startup: send reports it, writes nothing */
    setup();
    CHECK(open_hw(0) == 0);
    F.open_errno = EBUSY;
    CHECK(send_ir(0, mk(SYN100, LEN(SYN100), 100000)) == -HP_E_HALBUSY && F.wlen == 0);
}

static void test_stubs(void) {
    setup();
    CHECK(open_hw(0) == 0);
    CHECK(g_h->sinks.bt->key(g_h->sinks.bt->self, 0, 12, 0xe9, true) == -HP_E_NOSYS);
    uint8_t b[4] = {0};
    CHECK(g_h->sinks.rf->send(g_h->sinks.rf->self, b, 4) == -HP_E_NOSYS);
    CHECK(g_h->sinks.rf->recv(g_h->sinks.rf->self, b, 4, 0) == -HP_E_NOSYS);
    CHECK(g_h->sinks.rf->claim(g_h->sinks.rf->self) == 0);
    CHECK(g_h->sinks.rf->claim(g_h->sinks.rf->self) == -HP_E_BUSY);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: test_hw_sinks IR_BENCH\n"); return 2; }
    g_bench = argv[1];
    g_h = calloc(1, sizeof *g_h);
    test_golden();
    test_carrier_branches();
    test_sequence();
    test_ports();
    test_writes();
    test_oversize();
    test_open_errors();
    test_stubs();
    free(F.wbuf);
    free(g_h);
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("hw_sinks: ok\n");
    return 0;
}
