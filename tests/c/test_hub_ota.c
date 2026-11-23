#define _DEFAULT_SOURCE
/* Hub-side OTA upload state machine (hub/ota/hub_ota.c): staging, resume, busy, ENOSPC cleanup,
 * verify rejections, apply, power loss mid-apply. Host filesystem only; one mkdtemp dir per run. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ctest_assert.h"
#include "common/ota/ota_apply.h"
#include "hub/ota/hub_ota.h"

static char root[] = "/tmp/hub_ota_test_XXXXXX";
static uint8_t sk[64], pk[32];
static int sub_n;

static void hex(const uint8_t *d, char out[129]) {
    for (int i = 0; i < 64; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

typedef struct {
    char dir[256], hub[300], prev[300];
    uint8_t *img;
    size_t len;
    char sha[129];
} fx_t;

static void put_file(const char *p, const void *b, size_t n) {
    FILE *f = fopen(p, "wb");
    CT_CHECK(f != NULL);
    if (f) {
        fwrite(b, 1, n, f);
        fclose(f);
    }
}

static int exists(const char *p) { return access(p, F_OK) == 0; }

static long file_len(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    *n = 0;
    if (!f) return NULL;
    char *b = malloc(65536);
    *n = fread(b, 1, 65536, f);
    fclose(f);
    return b;
}

/* A directory with an installed "hub" binary and a signed image of `plen` payload bytes. */
static void fixture(fx_t *x, uint32_t build, int target, size_t plen) {
    snprintf(x->dir, sizeof x->dir, "%s/c%d", root, sub_n++);
    CT_EQ(mkdir(x->dir, 0755), 0);
    snprintf(x->hub, sizeof x->hub, "%s/hub", x->dir);
    snprintf(x->prev, sizeof x->prev, "%s/hub.prev", x->dir);
    put_file(x->hub, "OLD-BINARY", 10);
    chmod(x->hub, 0755);
    uint8_t *pl = malloc(plen);
    for (size_t i = 0; i < plen; i++) pl[i] = (uint8_t)(i * 31 + build);
    x->len = OTA_HDR_BYTES + plen;
    x->img = malloc(x->len);
    CT_EQ(ota_header_pack(x->img, target, build, pl, plen, sk), OTA_OK);
    memcpy(x->img + OTA_HDR_BYTES, pl, plen);
    free(pl);
    uint8_t d[64];
    ota_sha512_t c;
    ota_sha512_init(&c);
    ota_sha512_update(&c, x->img, x->len);
    ota_sha512_final(&c, d);
    hex(d, x->sha);
}

static void setup(hp_ota_t *o, fx_t *x) {
    hp_ota_init(o, x->dir, NULL);
    o->pubkey = pk;
    o->allow_dev = true; /* these builds carry the public dev key */
}

static int send_all(hp_ota_t *o, const void *own, const fx_t *x, size_t from, size_t step) {
    for (size_t off = from; off < x->len; off += step) {
        size_t n = x->len - off < step ? x->len - off : step;
        int e = hp_ota_chunk(o, own, (uint32_t)off, x->img + off, n);
        if (e) return e;
    }
    return 0;
}

/* Blocking helper: start the worker and wait for it. */
static int hp_ota_end(hp_ota_t *o, const void *owner, uint32_t *build_out) {
    int rc = hp_ota_end_start(o, owner);
    if (rc != HP_OTA_ASYNC) return rc;
    struct pollfd p = {hp_ota_done_fd(o), POLLIN, 0};
    while (poll(&p, 1, -1) < 0 && errno == EINTR) {}
    return hp_ota_end_done(o, build_out, NULL, NULL);
}

static void test_good_update(void) {
    fx_t x;
    hp_ota_t o;
    fixture(&x, 7, OTA_TARGET_HUB, 5000);
    setup(&o, &x);
    int own = 1;
    CT_EQ(hp_ota_begin(&o, &own, 1, (uint32_t)x.len, 7, x.sha), 0);
    CT_EQ(send_all(&o, &own, &x, 0, 1000), 0);
    uint32_t b = 0;
    CT_EQ(hp_ota_end(&o, &own, &b), 0);
    CT_EQ(b, 7);
    CT_CHECK(o.restart);
    size_t n = 0;
    char *got = slurp(x.hub, &n);
    CT_EQ(n, 5000);
    CT_MEM_EQ(got, x.img + OTA_HDR_BYTES, 5000);
    free(got);
    got = slurp(x.prev, &n);
    CT_CHECK(n == 10 && !memcmp(got, "OLD-BINARY", 10));
    free(got);
    char p[320];
    snprintf(p, sizeof p, "%s/ota-staged.img", x.dir);
    CT_CHECK(!exists(p));
    snprintf(p, sizeof p, "%s/ota-pending", x.dir);
    CT_CHECK(exists(p)); /* stays until the boot hook sees the health mark */
    got = slurp(p, &n);
    CT_CHECK(got && !strncmp(got, "build=7\nprev=0\n", 15)); /* what the hook needs to roll back */
    free(got);
    snprintf(p, sizeof p, "%s/ota-restart", x.dir);
    CT_CHECK(exists(p));
    snprintf(p, sizeof p, "%s/ota-build", x.dir);
    got = slurp(p, &n);
    CT_CHECK(got && atoi(got) == 7);
    free(got);
    /* the next process: build 7 is now the floor; restart request does not survive into it */
    hp_ota_t o2;
    hp_ota_init(&o2, x.dir, NULL);
    o2.allow_dev = true;
    CT_EQ(o2.min_build, 7);
    snprintf(p, sizeof p, "%s/ota-restart", x.dir);
    CT_CHECK(!exists(p));
    CT_EQ(hp_ota_begin(&o2, &own, 1, (uint32_t)x.len, 6, x.sha), HP_OTA_E_NOTREADY); /* unproven update pending */
    snprintf(p, sizeof p, "%s/ota-pending", x.dir);
    unlink(p); /* the hook's health mark */
    CT_EQ(hp_ota_begin(&o2, &own, 1, (uint32_t)x.len, 6, x.sha), HP_OTA_E_DOWNGRADE);
    CT_EQ(hp_ota_begin(&o2, &own, 1, (uint32_t)x.len, 7, x.sha), HP_OTA_E_DOWNGRADE); /* the same build as the running one */
    free(x.img);
}

/* The daemon's form: end_start returns at once, the worker signals a pollable fd, and until then nothing else
 * may touch the upload. */
static void test_async_end(void) {
    fx_t x;
    hp_ota_t o;
    fixture(&x, 9, OTA_TARGET_HUB, 3000);
    setup(&o, &x);
    int own = 1;
    CT_EQ(hp_ota_begin(&o, &own, 1, (uint32_t)x.len, 9, x.sha), 0);
    CT_EQ(send_all(&o, &own, &x, 0, 1000), 0);
    CT_EQ(hp_ota_done_fd(&o), -1);
    CT_EQ(hp_ota_end_start(&o, &own), HP_OTA_ASYNC);
    CT_CHECK(hp_ota_ending(&o));
    CT_EQ(hp_ota_begin(&o, &own, 1, (uint32_t)x.len, 9, x.sha), HP_OTA_E_BUSY);
    CT_EQ(hp_ota_abort(&o, &own), HP_OTA_E_BUSY);
    CT_EQ(hp_ota_end_start(&o, &own), HP_OTA_E_BUSY);
    struct pollfd p = {hp_ota_done_fd(&o), POLLIN, 0};
    CT_EQ(poll(&p, 1, 5000), 1);
    uint32_t b = 0;
    const void *who = NULL;
    bool alive = false;
    CT_EQ(hp_ota_end_done(&o, &b, &who, &alive), HP_OTA_OK);
    CT_EQ(b, 9);
    CT_CHECK(who == &own && alive && o.restart && !hp_ota_ending(&o) && !o.receiving);
    CT_EQ(hp_ota_end_done(&o, &b, NULL, NULL), HP_OTA_E_STATE); /* nothing finished */
    size_t n = 0;
    char *got = slurp(x.hub, &n);
    CT_EQ(n, 3000);
    free(got);
    free(x.img);

    /* a corrupted transfer fails in the worker: error code, staging file gone, upload not resumable */
    fixture(&x, 10, OTA_TARGET_HUB, 2000);
    hp_ota_t o2;
    setup(&o2, &x);
    char bad[129];
    memcpy(bad, x.sha, sizeof bad);
    bad[0] = bad[0] == '0' ? '1' : '0';
    CT_EQ(hp_ota_begin(&o2, &own, 1, (uint32_t)x.len, 10, bad), 0);
    CT_EQ(send_all(&o2, &own, &x, 0, 1000), 0);
    CT_EQ(hp_ota_end_start(&o2, &own), HP_OTA_ASYNC);
    p.fd = hp_ota_done_fd(&o2);
    CT_EQ(poll(&p, 1, 5000), 1);
    hp_ota_owner_gone(&o2, &own); /* the asking connection closed meanwhile */
    CT_EQ(hp_ota_end_done(&o2, &b, &who, &alive), HP_OTA_E_SHA);
    CT_CHECK(!alive && !o2.restart && !o2.receiving);
    char sp[320];
    snprintf(sp, sizeof sp, "%s/ota-staged.img", x.dir);
    CT_CHECK(!exists(sp));
    free(x.img);
}

static int g_gate[2];
static void gate_hook(void) { char c; (void)!read(g_gate[0], &c, 1); } /* the worker parks here, mid-apply */

/* While the worker is parked mid-apply the caller is free: end_start returned at once, the done fd is quiet, and
 * other calls (what a ping/other session would do) are served; the result arrives only after the worker is released. */
static void test_loop_free_while_worker_runs(void) {
    fx_t x;
    hp_ota_t o;
    fixture(&x, 11, OTA_TARGET_HUB, 2000);
    setup(&o, &x);
    CT_EQ(pipe(g_gate), 0);
    o.swap_hook = gate_hook;
    int own = 1;
    CT_EQ(hp_ota_begin(&o, &own, 1, (uint32_t)x.len, 11, x.sha), 0);
    CT_EQ(send_all(&o, &own, &x, 0, 1000), 0);
    CT_EQ(hp_ota_end_start(&o, &own), HP_OTA_ASYNC);
    struct pollfd p = {hp_ota_done_fd(&o), POLLIN, 0};
    CT_EQ(poll(&p, 1, 150), 0); /* still working: nothing to collect, and we were not blocked */
    for (int i = 0; i < 100; i++) CT_EQ(hp_ota_chunk(&o, &own, 0, x.img, 10), HP_OTA_E_BUSY); /* the loop keeps answering */
    CT_EQ(write(g_gate[1], "x", 1), 1);
    CT_EQ(poll(&p, 1, 5000), 1);
    uint32_t b = 0;
    CT_EQ(hp_ota_end_done(&o, &b, NULL, NULL), HP_OTA_OK);
    CT_EQ(b, 11);
    close(g_gate[0]);
    close(g_gate[1]);
    free(x.img);
}

/* Shutdown during an async end: finish_on_exit waits for the worker, so the process never exits between arming and
 * the rename, and the result is applied (restart set). */
static void test_finish_on_exit_joins_the_worker(void) {
    fx_t x;
    hp_ota_t o;
    fixture(&x, 12, OTA_TARGET_HUB, 2000);
    setup(&o, &x);
    int own = 1;
    CT_EQ(hp_ota_begin(&o, &own, 1, (uint32_t)x.len, 12, x.sha), 0);
    CT_EQ(send_all(&o, &own, &x, 0, 1000), 0);
    CT_EQ(hp_ota_end_start(&o, &own), HP_OTA_ASYNC);
    hp_ota_finish_on_exit(&o); /* SIGTERM path: no polling, just join */
    CT_CHECK(!hp_ota_ending(&o) && o.restart);
    size_t n = 0;
    char *got = slurp(x.hub, &n);
    CT_EQ(n, 2000); /* fully swapped, never half */
    free(got);
    char p[320];
    snprintf(p, sizeof p, "%s/ota-build", x.dir);
    CT_CHECK(exists(p));
    hp_ota_finish_on_exit(&o); /* idempotent */
    free(x.img);
}

static void test_busy_resume_offsets(void) {
    fx_t x;
    hp_ota_t o;
    fixture(&x, 3, OTA_TARGET_HUB, 9000);
    setup(&o, &x);
    int a = 1, b = 2;
    CT_EQ(hp_ota_begin(&o, &a, 1, (uint32_t)x.len, 3, x.sha), 0);
    CT_EQ(hp_ota_begin(&o, &b, 1, (uint32_t)x.len, 3, x.sha), HP_OTA_E_BUSY); /* another live uploader */
    CT_EQ(hp_ota_begin(&o, &a, 1, (uint32_t)x.len + 1, 3, x.sha), HP_OTA_E_BUSY); /* same owner, other image */
    CT_EQ(hp_ota_chunk(&o, &b, 0, x.img, 10), HP_OTA_E_STATE);
    CT_EQ(hp_ota_abort(&o, &b), HP_OTA_E_BUSY);
    CT_EQ(hp_ota_chunk(&o, &a, 0, x.img, 4096), 0);
    CT_EQ(hp_ota_chunk(&o, &a, 0, x.img, 4096), 0); /* retransmit: accepted, offset unchanged */
    CT_EQ(o.offset, 4096);
    CT_EQ(hp_ota_chunk(&o, &a, 4097, x.img + 4097, 10), HP_OTA_E_OFFSET); /* gap */
    CT_EQ(hp_ota_chunk(&o, &a, 4000, x.img + 4000, 200), 0); /* overlap: only the new bytes count */
    CT_EQ(o.offset, 4200);
    CT_EQ(hp_ota_chunk(&o, &a, 4200, x.img, HP_OTA_CHUNK_MAX + 1), HP_OTA_E_CHUNK);
    CT_EQ(hp_ota_chunk(&o, &a, 4200, x.img, 0), HP_OTA_E_CHUNK);
    CT_EQ(hp_ota_end(&o, &a, NULL), HP_OTA_E_SIZE); /* truncated upload: refused, still resumable */
    /* the uploader's connection drops; another connection resumes where it stopped */
    hp_ota_owner_gone(&o, &a);
    CT_EQ(hp_ota_chunk(&o, &a, 4200, x.img + 4200, 10), HP_OTA_E_STATE);
    CT_EQ(hp_ota_begin(&o, &b, 1, (uint32_t)x.len, 3, x.sha), 0);
    CT_EQ(o.offset, 4200);
    CT_EQ(send_all(&o, &b, &x, 4200, 4096), 0);
    CT_EQ(hp_ota_end(&o, &b, NULL), 0);
    free(x.img);
    /* past the end of the declared size */
    fixture(&x, 4, OTA_TARGET_HUB, 100);
    setup(&o, &x);
    CT_EQ(hp_ota_begin(&o, &a, 1, (uint32_t)x.len, 4, x.sha), 0);
    CT_EQ(hp_ota_chunk(&o, &a, 0, x.img, x.len), 0);
    CT_EQ(hp_ota_chunk(&o, &a, (uint32_t)x.len, x.img, 1), HP_OTA_E_SIZE);
    /* a suspended upload of something else is replaced by a different begin */
    hp_ota_owner_gone(&o, &a);
    CT_EQ(hp_ota_begin(&o, &b, 1, (uint32_t)x.len, 5, x.sha), 0);
    CT_EQ(o.offset, 0);
    CT_EQ(hp_ota_abort(&o, &b), 0);
    CT_EQ(hp_ota_abort(&o, &b), 0); /* nothing in flight: fine */
    char p[320];
    snprintf(p, sizeof p, "%s/ota-staged.img", x.dir);
    CT_CHECK(!exists(p));
    free(x.img);
}

static void test_begin_validation(void) {
    fx_t x;
    hp_ota_t o;
    fixture(&x, 1, OTA_TARGET_HUB, 100);
    setup(&o, &x);
    int a = 1;
    CT_EQ(hp_ota_begin(&o, &a, 2, (uint32_t)x.len, 1, x.sha), HP_OTA_E_TARGET);
    CT_EQ(hp_ota_begin(&o, &a, 1, OTA_HDR_BYTES - 1, 1, x.sha), HP_OTA_E_SIZE);
    CT_EQ(hp_ota_begin(&o, &a, 1, OTA_HDR_BYTES + OTA_MAX_PAYLOAD + 1, 1, x.sha), HP_OTA_E_SIZE);
    CT_EQ(hp_ota_begin(&o, &a, 1, (uint32_t)x.len, 1, "nothex"), HP_OTA_E_ARG);
    hp_ota_t off;
    hp_ota_init(&off, NULL, NULL);
    CT_EQ(hp_ota_begin(&off, &a, 1, (uint32_t)x.len, 1, x.sha), HP_OTA_E_DISABLED);
    CT_EQ(hp_ota_chunk(&off, &a, 0, x.img, 1), HP_OTA_E_DISABLED);
    free(x.img);
}

static uint64_t tiny_disk(const char *dir) {
    (void)dir;
    return 100 * 1024;
}

static uint64_t unknown_disk(const char *dir) {
    (void)dir;
    return UINT64_MAX;
}

static long full_disk(int fd, const void *p, size_t n) {
    static int calls;
    if (++calls < 2) return (long)write(fd, p, n); /* first chunk lands, the next one hits ENOSPC */
    errno = ENOSPC;
    return -1;
}

static void test_space_and_enospc(void) {
    fx_t x;
    hp_ota_t o;
    char p[320];
    fixture(&x, 2, OTA_TARGET_HUB, 100000);
    setup(&o, &x);
    int a = 1;
    o.avail_fn = tiny_disk; /* statvfs says there is room for less than 2x the image */
    CT_EQ(hp_ota_begin(&o, &a, 1, (uint32_t)x.len, 2, x.sha), HP_OTA_E_NOSPACE);
    snprintf(p, sizeof p, "%s/ota-staged.img", x.dir);
    CT_CHECK(!exists(p));
    /* statvfs unknown (the hub's case on some mounts): begin proceeds, ENOSPC mid-write is clean */
    hp_ota_t o2;
    setup(&o2, &x);
    o2.avail_fn = unknown_disk;
    o2.write_fn = full_disk;
    CT_EQ(hp_ota_begin(&o2, &a, 1, (uint32_t)x.len, 2, x.sha), 0);
    CT_EQ(hp_ota_chunk(&o2, &a, 0, x.img, 4096), 0);
    CT_EQ(hp_ota_chunk(&o2, &a, 4096, x.img + 4096, 4096), HP_OTA_E_NOSPACE);
    CT_CHECK(!exists(p)); /* the partial file is gone */
    CT_CHECK(!o2.receiving);
    CT_EQ(hp_ota_chunk(&o2, &a, 4096, x.img + 4096, 10), HP_OTA_E_STATE);
    size_t n = 0;
    char *got = slurp(x.hub, &n);
    CT_CHECK(n == 10 && !memcmp(got, "OLD-BINARY", 10));
    free(got);
    free(x.img);
}

static void test_rejected_images_leave_binary_alone(void) {
    fx_t x;
    hp_ota_t o;
    char p[320];
    int a = 1;
    /* each case: a tampered/odd image, fully uploaded, refused at end; hub, hub.prev and staging untouched */
    for (int k = 0; k < 5; k++) {
        fixture(&x, 9, k == 2 ? OTA_TARGET_REMOTE : OTA_TARGET_HUB, 3000);
        if (k == 0) x.img[OTA_HDR_BYTES + 5] ^= 1;               /* payload bit flip: digest */
        if (k == 1) x.img[100] ^= 1;                             /* signature byte: bad signature */
        if (k == 3) x.img[0] = 'X';                              /* magic */
        if (k == 4) x.img[8] ^= 1;                               /* build id differs from the declared one */
        uint8_t d[64];
        ota_sha512_t c;
        ota_sha512_init(&c);
        ota_sha512_update(&c, x.img, x.len);
        ota_sha512_final(&c, d);
        hex(d, x.sha); /* the transfer digest matches what was sent: only the signed container is bad */
        setup(&o, &x);
        CT_EQ(hp_ota_begin(&o, &a, 1, (uint32_t)x.len, 9, x.sha), 0);
        CT_EQ(send_all(&o, &a, &x, 0, 2048), 0);
        int e = hp_ota_end(&o, &a, NULL);
        /* the remote-targeted image is refused as the wrong target, the rest as verify failures */
        CT_EQ(e, HP_OTA_E_VERIFY);
        CT_CHECK(o.verify_rc == (k == 0 ? OTA_E_DIGEST : k == 2 ? OTA_E_TARGET : k == 3 || k == 4 ? OTA_E_FORMAT : OTA_E_SIG));
        size_t n = 0;
        char *got = slurp(x.hub, &n);
        CT_CHECK(n == 10 && !memcmp(got, "OLD-BINARY", 10));
        free(got);
        CT_CHECK(!exists(x.prev));
        snprintf(p, sizeof p, "%s/ota-staged.img", x.dir);
        CT_CHECK(!exists(p));
        snprintf(p, sizeof p, "%s/ota-pending", x.dir);
        CT_CHECK(!exists(p)); /* a failed apply must not arm the hook's rollback */
        snprintf(p, sizeof p, "%s/ota-restart", x.dir);
        CT_CHECK(!exists(p));
        CT_CHECK(!o.restart && !o.receiving);
        free(x.img);
    }
    /* transport corruption: whole-image digest differs from begin.sha */
    fixture(&x, 9, OTA_TARGET_HUB, 3000);
    setup(&o, &x);
    CT_EQ(hp_ota_begin(&o, &a, 1, (uint32_t)x.len, 9, x.sha), 0);
    x.img[OTA_HDR_BYTES + 1] ^= 1;
    CT_EQ(send_all(&o, &a, &x, 0, 2048), 0);
    CT_EQ(hp_ota_end(&o, &a, NULL), HP_OTA_E_SHA);
    CT_CHECK(!exists(x.prev));
    /* declared build differs from the signed one */
    free(x.img);
    fixture(&x, 9, OTA_TARGET_HUB, 3000);
    setup(&o, &x);
    CT_EQ(hp_ota_begin(&o, &a, 1, (uint32_t)x.len, 10, x.sha), 0);
    CT_EQ(send_all(&o, &a, &x, 0, 2048), 0);
    CT_EQ(hp_ota_end(&o, &a, NULL), HP_OTA_E_VERIFY);
    CT_EQ(o.verify_rc, OTA_E_FORMAT);
    CT_CHECK(!exists(x.prev) && file_len(x.hub) == 10); /* refused BEFORE the swap */
    free(x.img);
}

static void die_mid_apply(void) { _exit(9); }

static void test_power_loss_mid_apply(void) {
    fx_t x;
    int a = 1;
    fixture(&x, 6, OTA_TARGET_HUB, 8000);
    pid_t pid = fork();
    if (pid == 0) { /* "power cut" between verify and the swap */
        hp_ota_t o;
        setup(&o, &x);
        hp_ota_begin(&o, &a, 1, (uint32_t)x.len, 6, x.sha);
        send_all(&o, &a, &x, 0, 4096);
        ota_apply_test_hook = die_mid_apply;
        hp_ota_end(&o, &a, NULL);
        _exit(0);
    }
    int st = 0;
    CT_EQ(waitpid(pid, &st, 0), pid);
    CT_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 9);
    /* the installed binary is whole and executable; the next start sweeps the leftovers */
    size_t n = 0;
    char *got = slurp(x.hub, &n);
    CT_CHECK(n == 10 && !memcmp(got, "OLD-BINARY", 10));
    free(got);
    struct stat s;
    CT_EQ(stat(x.hub, &s), 0);
    CT_CHECK(s.st_mode & 0100);
    char p[320];
    snprintf(p, sizeof p, "%s/ota-pending", x.dir);
    CT_CHECK(!exists(p)); /* cut during verify/copy: the marker is not armed yet, so nothing can roll back */
    CT_CHECK(!exists(x.prev));
    hp_ota_t o;
    setup(&o, &x);
    snprintf(p, sizeof p, "%s/ota-staged.img", x.dir);
    CT_CHECK(!exists(p));
    /* and a retry from scratch works */
    CT_EQ(hp_ota_begin(&o, &a, 1, (uint32_t)x.len, 6, x.sha), 0);
    CT_EQ(send_all(&o, &a, &x, 0, 4096), 0);
    CT_EQ(hp_ota_end(&o, &a, NULL), 0);
    free(x.img);
}

static void die_at_swap(void) { _exit(9); }

static void test_power_loss_between_link_and_rename(void) {
    fx_t x;
    int a = 1;
    fixture(&x, 6, OTA_TARGET_HUB, 8000);
    put_file(x.prev, "OLDER-PREV", 10); /* an earlier update's prev: must not outlive a cut as a rollback source */
    pid_t pid = fork();
    if (pid == 0) {
        hp_ota_t o;
        setup(&o, &x);
        o.swap_hook = die_at_swap;
        hp_ota_begin(&o, &a, 1, (uint32_t)x.len, 6, x.sha);
        send_all(&o, &a, &x, 0, 4096);
        hp_ota_end(&o, &a, NULL);
        _exit(0);
    }
    int st = 0;
    CT_EQ(waitpid(pid, &st, 0), pid);
    CT_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 9);
    struct stat h, p;
    CT_EQ(stat(x.hub, &h), 0);
    CT_EQ(stat(x.prev, &p), 0);
    CT_CHECK(h.st_ino == p.st_ino); /* hub.prev IS the running binary: the hook's rollback is a no-op, never N-2 */
    size_t n = 0;
    char *got = slurp(x.hub, &n);
    CT_CHECK(n == 10 && !memcmp(got, "OLD-BINARY", 10));
    free(got);
    char bp[320];
    snprintf(bp, sizeof bp, "%s/ota-build", x.dir);
    CT_CHECK(!exists(bp)); /* the old binary stays the build floor: ota-build is written only after the rename */
    free(x.img);
}

static void test_unwritable_marker_aborts_the_swap(void) {
    fx_t x;
    hp_ota_t o;
    int a = 1;
    char p[340];
    fixture(&x, 6, OTA_TARGET_HUB, 3000);
    setup(&o, &x);
    snprintf(p, sizeof p, "%s/ota-pending.tmp", x.dir);
    CT_EQ(mkdir(p, 0755), 0); /* the marker cannot be written: no rollback net, so no swap */
    CT_EQ(hp_ota_begin(&o, &a, 1, (uint32_t)x.len, 6, x.sha), 0);
    CT_EQ(send_all(&o, &a, &x, 0, 4096), 0);
    CT_EQ(hp_ota_end(&o, &a, NULL), HP_OTA_E_IO);
    size_t n = 0;
    char *got = slurp(x.hub, &n);
    CT_CHECK(n == 10 && !memcmp(got, "OLD-BINARY", 10)); /* nothing changed */
    free(got);
    snprintf(p, sizeof p, "%s/ota-pending", x.dir);
    CT_CHECK(!exists(p) && !o.restart);
    snprintf(p, sizeof p, "%s/hub.new", x.dir);
    CT_CHECK(!exists(p));
    free(x.img);
}

static void test_dev_key_and_health_gate(void) {
    fx_t x;
    hp_ota_t o;
    int a = 1;
    char p[320];
    fixture(&x, 3, OTA_TARGET_HUB, 100);
    setup(&o, &x);
    o.key_dev = true;
    o.allow_dev = false;
    CT_EQ(hp_ota_begin(&o, &a, 1, (uint32_t)x.len, 3, x.sha), HP_OTA_E_DEVKEY);
    o.allow_dev = true;
    snprintf(p, sizeof p, "%s/healthy", x.dir);
    o.health_file = p;
    CT_EQ(hp_ota_begin(&o, &a, 1, (uint32_t)x.len, 3, x.sha), HP_OTA_E_NOTREADY); /* the hook has not marked this boot healthy */
    put_file(p, "1", 1);
    CT_EQ(hp_ota_begin(&o, &a, 1, (uint32_t)x.len, 3, x.sha), 0);
    free(x.img);
}

static void test_rollback_info(void) {
    fx_t x;
    hp_ota_t o;
    fixture(&x, 1, OTA_TARGET_HUB, 10);
    setup(&o, &x);
    uint32_t b = 0;
    char why[96];
    CT_EQ(hp_ota_rollback_info(&o, &b, why, sizeof why), 0);
    char p[320];
    snprintf(p, sizeof p, "%s/ota-rollback", x.dir);
    put_file(p, "build=12\nreason=boot_unclean\nat=up 55s\n", 38);
    CT_EQ(hp_ota_rollback_info(&o, &b, why, sizeof why), 1);
    CT_EQ(b, 12);
    CT_CHECK(!strcmp(why, "boot_unclean"));
    free(x.img);
}

static uint32_t zero_build(void) { return 0; }

int main(void) {
    hp_build_id_hook = zero_build; /* the tests pick their own build ids */
    if (!mkdtemp(root)) return 1;
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i + 1);
    ota_keypair_from_seed(seed, pk, sk);
    test_good_update();
    test_async_end();
    test_finish_on_exit_joins_the_worker();
    test_loop_free_while_worker_runs();
    test_busy_resume_offsets();
    test_begin_validation();
    test_space_and_enospc();
    test_rejected_images_leave_binary_alone();
    test_power_loss_mid_apply();
    test_power_loss_between_link_and_rename();
    test_unwritable_marker_aborts_the_swap();
    test_dev_key_and_health_gate();
    test_rollback_info();
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    if (system(cmd) != 0) return 1;
    return CT_DONE();
}
