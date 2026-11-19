#define _GNU_SOURCE
/* Bluetooth helper pure logic: HID report building, remote key diff, IPC framing. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ctest_assert.h"
#include "hub/bt/bt_bonds.h"
#include "hub/bt/bt_hid.h"
#include "hub/bt/bt_hid_queue.h"
#include "hub/bt/bt_txq.h"
#include "hub/bt/bt_ipc.h"
#include "hub/bt/bt_remote.h"

static void test_keyboard_reports(void) {
    bt_hid_keys_t k;
    memset(&k, 0, sizeof k);
    uint8_t r[16];
    CT_EQ(bt_hid_apply(&k, 0x07, 0x04, true, r), 10); /* A down */
    CT_EQ(r[0], 0xA1);
    CT_EQ(r[1], 1);
    CT_EQ(r[4], 0x04);
    CT_EQ(bt_hid_apply(&k, 0x07, 0xE1, true, r), 10); /* left shift joins */
    CT_EQ(r[2], 0x02);
    CT_EQ(r[4], 0x04);
    CT_EQ(bt_hid_apply(&k, 0x07, 0x04, false, r), 10);
    CT_EQ(r[4], 0);
    CT_EQ(r[2], 0x02);
    CT_EQ(bt_hid_apply(&k, 0x07, 0xE1, false, r), 10);
    for (int i = 2; i < 10; i++) CT_EQ(r[i], 0); /* all released */
    CT_EQ(bt_hid_apply(&k, 0x07, 0, true, r), 0);
    CT_EQ(bt_hid_apply(&k, 0x08, 1, true, r), 0); /* unknown page */
}

static void test_six_key_rollover(void) {
    bt_hid_keys_t k;
    memset(&k, 0, sizeof k);
    uint8_t r[16];
    for (uint16_t u = 4; u < 11; u++) bt_hid_apply(&k, 0x07, u, true, r); /* 7 keys */
    int n = 0;
    for (int i = 4; i < 10; i++) n += r[i] != 0;
    CT_EQ(n, 6);
}

static void test_consumer(void) {
    bt_hid_keys_t k;
    memset(&k, 0, sizeof k);
    uint8_t r[16];
    CT_EQ(bt_hid_apply(&k, 0x0C, 0x00E9, true, r), 4); /* volume up */
    CT_EQ(r[1], 2);
    CT_EQ(r[2], 0xE9);
    CT_EQ(r[3], 0x00);
    CT_EQ(bt_hid_apply(&k, 0x0C, 0x00E9, false, r), 4);
    CT_EQ(r[2] | r[3], 0);
    CT_EQ(bt_hid_apply(&k, 0x0C, 0x0400, true, r), 0); /* beyond the descriptor's range */
}

static int s_events[8][3];
static int s_n;
static void emit(uint8_t page, uint16_t usage, bool down, void *ctx) {
    (void)ctx;
    s_events[s_n][0] = page;
    s_events[s_n][1] = usage;
    s_events[s_n][2] = down;
    s_n++;
}

static void test_remote_diff(void) {
    bt_rkey_set_t old = {.n = 0}, now = {.n = 0};
    now.act[now.n++] = (bt_rkey_t){0x0C, 0x42};
    bt_rkey_diff(&old, &now, emit, NULL);
    CT_EQ(s_n, 1);
    CT_EQ(s_events[0][2], 1);
    now.n = 0;
    now.act[now.n++] = (bt_rkey_t){0x07, 0x28};
    bt_rkey_diff(&old, &now, emit, NULL); /* one key replaced by another */
    CT_EQ(s_n, 3);
    CT_EQ(s_events[1][2], 0);
    CT_EQ(s_events[1][1], 0x42);
    CT_EQ(s_events[2][1], 0x28);
    now.n = 0;
    bt_rkey_diff(&old, &now, emit, NULL);
    CT_EQ(s_n, 4);
    CT_EQ(old.n, 0);
}

static void test_ipc_frames(void) {
    uint8_t buf[BT_IPC_HDR + 8], pl[5] = {3, 0x07, 0x00, 0x04, 1};
    size_t n = bt_ipc_frame(buf, BT_IPC_KEY, pl, sizeof pl);
    CT_EQ(n, BT_IPC_HDR + 5);
    uint8_t type;
    const uint8_t *p;
    size_t len;
    CT_EQ(bt_ipc_parse(buf, n - 1, &type, &p, &len), 0); /* incomplete */
    CT_EQ(bt_ipc_parse(buf, n, &type, &p, &len), n);
    CT_EQ(type, BT_IPC_KEY);
    CT_EQ(len, 5);
    CT_MEM_EQ(p, pl, 5);
    uint8_t bad[3] = {BT_IPC_PING, 0xFF, 0xFF};
    CT_EQ(bt_ipc_parse(bad, 3, &type, &p, &len), (size_t)-1);
}


/* ---- outbound queue: a stuck socket never drops or splits a frame ---- */
static void test_txq(void) {
    int sv[2];
    CT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    static uint8_t store[1 << 20];
    bt_txq_t q = {store, 0, sizeof store};
    int sent = 0;
    uint8_t f[BT_IPC_HDR + 4];
    for (; sent < 20000; sent++) { /* far more than the socket buffer holds, nobody reads yet */
        uint8_t pl[4] = {(uint8_t)sent, (uint8_t)(sent >> 8), 0, 0};
        size_t n = bt_ipc_frame(f, BT_IPC_RKEY, pl, 4);
        if (bt_txq_push(&q, f, n) < 0) break;
        CT_EQ(bt_txq_drain(&q, sv[0]), 0);
    }
    CT_EQ(sent, 20000);
    CT_CHECK(q.n > 0); /* some of it is still queued */
    size_t cap_small = 4;
    static uint8_t tstore[8];
    bt_txq_t tiny = {tstore, 0, cap_small};
    CT_EQ(bt_txq_push(&tiny, f, sizeof f), -1); /* over the cap: the caller gives up */
    uint8_t rx[65536];
    size_t have = 0;
    int got = 0;
    while (got < sent) { /* the reader drains; the writer keeps draining on "writable" */
        ssize_t r = read(sv[1], rx + have, sizeof rx - have);
        if (r > 0) have += (size_t)r;
        bt_txq_drain(&q, sv[0]);
        size_t off = 0, used;
        uint8_t t;
        const uint8_t *p;
        size_t pn;
        while ((used = bt_ipc_parse(rx + off, have - off, &t, &p, &pn)) > 0 && used != (size_t)-1) {
            CT_EQ(p[0] | (p[1] << 8), got & 0xFFFF); /* in order, none lost */
            got++;
            off += used;
        }
        memmove(rx, rx + off, have - off);
        have -= off;
        if (r <= 0 && q.n == 0) break;
    }
    CT_EQ(got, sent);
    close(sv[0]);
    close(sv[1]);
}

static void test_backoff(void) {
    CT_EQ(bt_backoff_ms(1), 2000);
    CT_EQ(bt_backoff_ms(2), 4000);
    CT_EQ(bt_backoff_ms(8), 256000);
    CT_EQ(bt_backoff_ms(9), 300000);
    CT_EQ(bt_backoff_ms(255), 300000);
}

/* ---- bond file: compaction, truncated tail, .bak fallback ---- */
static size_t slurp(const char *path, uint8_t *out, size_t max) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    size_t n = fread(out, 1, max, fp);
    fclose(fp);
    return n;
}
static void put(FILE *fp, uint32_t tag, uint32_t len, const char *v, size_t have) {
    uint8_t h[8] = {(uint8_t)(tag >> 24), (uint8_t)(tag >> 16), (uint8_t)(tag >> 8), (uint8_t)tag,
                    (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
    fwrite(h, 1, 8, fp);
    fwrite(v, 1, have, fp);
}
static void test_bonds(void) {
    char dir[] = "/tmp/hhbtXXXXXX", path[64], bak[80];
    CT_CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/keys.tlv", dir);
    snprintf(bak, sizeof bak, "%s.bak", path);
    FILE *fp = fopen(path, "wb");
    uint8_t hdr[8] = "BTstack";
    fwrite(hdr, 1, 8, fp);
    put(fp, 1, 2, "ab", 2);
    put(fp, 1, 3, "xyz", 3); /* overrides the first */
    put(fp, 2, 2, "cd", 2);
    put(fp, 3, 0, "", 0);    /* deleted */
    put(fp, 4, 10, "abc", 3); /* truncated last record */
    fclose(fp);
    CT_EQ(bt_bonds_prepare(path), 1);
    uint8_t buf[256];
    CT_EQ(slurp(path, buf, sizeof buf), 8 + 11 + 10); /* header + tag1 + tag2: bad tail dropped only */
    CT_MEM_EQ(buf + 8 + 8, "xyz", 3);
    CT_CHECK(slurp(bak, buf, sizeof buf) > 0);        /* the previous file is kept */
    fp = fopen(path, "wb");                           /* main file destroyed */
    fputs("garbage!garbage", fp);
    fclose(fp);
    CT_EQ(bt_bonds_prepare(path), 1);
    CT_EQ(slurp(path, buf, sizeof buf), 8 + 11 + 10); /* restored from .bak */
    unlink(path); unlink(bak);
    char tmp[80];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    unlink(tmp);
    rmdir(dir);
    CT_EQ(bt_bonds_prepare(path), 0); /* nothing at all: nothing to do */
}

/* ---- hid queue: a full queue never drops the release of a press that went out ---- */
static int s_sent_n;
static uint8_t s_sent[64][16];
static int s_accept;
static uint8_t q_send(uint16_t cid, const uint8_t *r, uint16_t len) {
    (void)cid;
    if (!s_accept) return 1;
    memcpy(s_sent[s_sent_n++], r, len);
    return 0;
}
static void q_req(uint16_t cid) { (void)cid; }
static void q_log(const char *l) { (void)l; }
static void test_hidq_overflow(void) {
    hidq_init(q_send, q_req, q_log);
    uint8_t press[10] = {0xA1, 1, 0, 0, 4, 0, 0, 0, 0, 0}, rel[10] = {0xA1, 1};
    for (int i = 0; i < 30; i++) {
        hidq_send(7, press, 10, 0);
        hidq_send(7, rel, 10, 0);
    }
    s_accept = 1;
    hidq_service(7, 100);
    CT_CHECK(s_sent_n > 0 && s_sent_n <= HIDQ_DEPTH);
    CT_CHECK(hidq_is_release(s_sent[s_sent_n - 1], 10)); /* the last thing the host sees is a release */
}

int main(void) {
    CT_RUN(test_keyboard_reports);
    CT_RUN(test_six_key_rollover);
    CT_RUN(test_consumer);
    CT_RUN(test_remote_diff);
    CT_RUN(test_ipc_frames);
    CT_RUN(test_txq);
    CT_RUN(test_backoff);
    CT_RUN(test_bonds);
    CT_RUN(test_hidq_overflow);
    return CT_DONE();
}
