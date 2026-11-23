#define _GNU_SOURCE
/* Bluetooth helper pure logic: HID report building, remote key diff, IPC framing. */
#include <errno.h>
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
#include "hub/bt/bt_pktq.h"
#include "hub/bt/bt_txq.h"
#include "hub/bt/bt_win.h"
#include "hub/bt/bt_leaddr.h"
#include "hub/bt/bt_trace.h"
#include "hci_dump.h"
#include <sys/stat.h>
#include "hub/bt/bt_ipc.h"
#include "hub/bt/bt_remote.h"
#include "hub/bt/bt_sc.h"
#include "hub/bt/hh_bt_gatt.h"

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


/* ---- HCI packet queue: EAGAIN packets are kept whole and in order, never dropped ---- */
static void test_pktq(void) {
    int sv[2];
    CT_EQ(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv), 0);
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    static bt_pktq_t q;
    uint8_t p[300];
    memset(p, 0xAB, sizeof p);
    int direct = 0; /* fill the socket until it says EAGAIN */
    for (;; direct++) {
        p[1] = (uint8_t)direct;
        if (write(sv[0], p, sizeof p) < 0) { CT_CHECK(errno == EAGAIN); break; }
        CT_CHECK(direct < 100000);
    }
    for (int i = 0; i < BT_PKTQ_SLOTS; i++) {
        p[1] = (uint8_t)(200 + i);
        CT_EQ(bt_pktq_push(&q, p, sizeof p), 0);
    }
    CT_EQ(bt_pktq_push(&q, p, sizeof p), -1); /* full */
    CT_EQ(bt_pktq_drain(&q, sv[0]), 0);
    CT_EQ(q.n, (unsigned)BT_PKTQ_SLOTS); /* still blocked: nothing lost, nothing sent */
    uint8_t rx[512];
    for (int i = 0; i < direct; i++) CT_CHECK(read(sv[1], rx, sizeof rx) == (ssize_t)sizeof p);
    CT_EQ(bt_pktq_drain(&q, sv[0]), 0); /* writable again */
    CT_EQ(q.n, 0u);
    for (int i = 0; i < BT_PKTQ_SLOTS; i++) { /* in order, whole */
        CT_CHECK(read(sv[1], rx, sizeof rx) == (ssize_t)sizeof p);
        CT_EQ(rx[1], 200 + i);
    }
    CT_EQ(bt_pktq_push(&q, p, BT_PKTQ_MAX + 1), -1);
    close(sv[0]);
    close(sv[1]);
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
/* ---- ATT database: Generic Attribute service, stable HID handles; Service Changed bookkeeping ---- */
static const uint8_t *att_find(uint16_t handle) {
    for (const uint8_t *p = profile_data + 1; p[0] || p[1]; p += p[0] | (p[1] << 8))
        if ((uint16_t)(p[4] | (p[5] << 8)) == handle) return p;
    return NULL;
}
static void test_att_db_gatt_service(void) {
    const uint8_t *svc = att_find(ATT_SERVICE_GATT_SERVICE_START_HANDLE);
    CT_CHECK(svc && svc[6] == 0x00 && svc[7] == 0x28 && svc[8] == 0x01 && svc[9] == 0x18); /* primary service 0x1801 */
    const uint8_t *decl = att_find(0x0037), *val = att_find(ATT_CHARACTERISTIC_GATT_SERVICE_CHANGED_01_VALUE_HANDLE);
    CT_CHECK(decl && decl[8] == 0x20 && decl[9] == 0x38 && decl[11] == 0x05 && decl[12] == 0x2a); /* indicate, 0x2A05 */
    CT_CHECK(val && val[6] == 0x05 && val[7] == 0x2a);
    const uint8_t *ccc = att_find(ATT_CHARACTERISTIC_GATT_SERVICE_CHANGED_01_CLIENT_CONFIGURATION_HANDLE);
    CT_CHECK(ccc && ccc[6] == 0x02 && ccc[7] == 0x29);
    CT_EQ(ATT_SERVICE_GATT_SERVICE_END_HANDLE, 0x0039);
    /* GAP: name + appearance; the HID handles did not move (a bonded host's cache stays valid) */
    CT_CHECK(att_find(0x0003) && att_find(0x0005));
    CT_EQ(ATT_CHARACTERISTIC_ORG_BLUETOOTH_CHARACTERISTIC_REPORT_01_VALUE_HANDLE, 0x0021);
    CT_EQ(ATT_CHARACTERISTIC_ORG_BLUETOOTH_CHARACTERISTIC_REPORT_MAP_01_VALUE_HANDLE, 0x002c);
    CT_EQ(ATT_CHARACTERISTIC_ORG_BLUETOOTH_CHARACTERISTIC_HID_CONTROL_POINT_01_VALUE_HANDLE, 0x0035);
}
static struct { uint32_t tag; uint8_t v[4]; } g_kv[8];
static int g_kvn;
static int kv_get(uint32_t tag, uint8_t *v, uint32_t cap) {
    for (int i = 0; i < g_kvn; i++)
        if (g_kv[i].tag == tag && cap >= 4) { memcpy(v, g_kv[i].v, 4); return 4; }
    return 0;
}
static int kv_put(uint32_t tag, const uint8_t *v, uint32_t len) {
    int i = 0;
    while (i < g_kvn && g_kv[i].tag != tag) i++;
    if (i == g_kvn) g_kvn++;
    g_kv[i].tag = tag;
    memcpy(g_kv[i].v, v, len < 4 ? len : 4);
    return 0;
}
static void kv_del(uint32_t tag) {
    for (int i = 0; i < g_kvn; i++)
        if (g_kv[i].tag == tag) { g_kv[i] = g_kv[--g_kvn]; return; }
}
static void test_service_changed(void) {
    static const bt_sc_store_t st = {kv_get, kv_put, kv_del};
    const uint8_t A[6] = {1, 2, 3, 4, 5, 6}, B[6] = {9, 8, 7, 6, 5, 4};
    uint8_t v[4];
    bt_sc_value(v);
    CT_EQ(v[0] | (v[1] << 8), 1);
    CT_EQ(v[2] | (v[3] << 8), 0xFFFF);
    uint32_t h = bt_sc_hash(profile_data, sizeof profile_data);
    g_kvn = 0;
    bt_sc_init(&st, h);
    /* a bond from before the record existed: indicated once, after its CCCD is enabled */
    CT_CHECK(!bt_sc_peer(0x40, A, 1));
    CT_CHECK(!bt_sc_ccc(0x40, 0));
    CT_CHECK(bt_sc_ccc(0x40, 2));
    bt_sc_sent(0x40);
    CT_CHECK(!bt_sc_ccc(0x40, 2)); /* never twice on a connection */
    bt_sc_drop(0x40);
    /* not confirmed (link lost): the next connection asks again */
    CT_CHECK(!bt_sc_ccc(0x41, 2));       /* the CCCD is restored before the peer is identified */
    CT_CHECK(bt_sc_peer(0x41, A, 1));
    bt_sc_sent(0x41);
    bt_sc_confirmed(0x41);               /* the handle value confirmation */
    bt_sc_drop(0x41);
    /* once per bond: later connections, and a helper restart (state kept in the store), stay quiet */
    CT_CHECK(!bt_sc_peer(0x42, A, 1) && !bt_sc_ccc(0x42, 2));
    bt_sc_drop(0x42);
    bt_sc_init(&st, h);
    CT_CHECK(!bt_sc_peer(0x43, A, 1) && !bt_sc_ccc(0x43, 2));
    bt_sc_drop(0x43);
    /* a fresh pairing has discovered the current database: no indication */
    bt_sc_bond_fresh(B, 1);
    CT_CHECK(!bt_sc_peer(0x44, B, 1) && !bt_sc_ccc(0x44, 2));
    bt_sc_drop(0x44);
    /* a new firmware database: every bond is told once more, and only once */
    bt_sc_init(&st, h + 1);
    CT_CHECK(!bt_sc_peer(0x45, B, 1) && bt_sc_ccc(0x45, 2));
    bt_sc_sent(0x45);
    bt_sc_confirmed(0x45);
    bt_sc_drop(0x45);
    CT_CHECK(!bt_sc_peer(0x46, B, 1) && !bt_sc_ccc(0x46, 2));
    CT_CHECK(bt_sc_peer(0x47, A, 1) == false && bt_sc_ccc(0x47, 2)); /* A has not been told about the new one */
    CT_EQ(bt_sc_ccc_get(0x47), 2);
    bt_sc_drop(0x46);
    bt_sc_drop(0x47);
    CT_EQ(bt_sc_ccc_get(0x47), 0);
    /* forgetting a bond deletes its record, so a later bond for the same address starts stale */
    int n0 = g_kvn;
    bt_sc_forget(B, 1);
    CT_EQ(g_kvn, n0 - 1);
    CT_CHECK(!bt_sc_peer(0x48, B, 1) && bt_sc_ccc(0x48, 2));
    bt_sc_drop(0x48);
}

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

/* ---- window transport -> scan / advertising decision, failure line ---- */
static void test_window_transport(void) {
    /* no window: nothing discoverable, no pairing, page scan on (the prior state) */
    for (uint8_t tr = 0; tr <= 2; tr++) {
        CT_CHECK(!bt_win_classic_pairing(0, tr) && !bt_win_le_pairing(0, tr));
        CT_CHECK(bt_win_classic_connectable(0, tr, false));
        CT_CHECK(!bt_win_classic_pairing(2, tr) && !bt_win_le_pairing(2, tr)); /* a remote window opens no host pairing */
    }
    CT_CHECK(bt_win_classic_pairing(1, BT_PAIR_TR_ANY) && bt_win_le_pairing(1, BT_PAIR_TR_ANY)); /* auto: both */
    CT_CHECK(!bt_win_classic_pairing(1, BT_PAIR_TR_LE) && bt_win_le_pairing(1, BT_PAIR_TR_LE));   /* le: no classic */
    CT_CHECK(bt_win_classic_pairing(1, BT_PAIR_TR_CLASSIC) && !bt_win_le_pairing(1, BT_PAIR_TR_CLASSIC)); /* classic: no adv */
    CT_CHECK(!bt_win_classic_connectable(1, BT_PAIR_TR_LE, false)); /* LE-only, no classic bond: no page scan */
    CT_CHECK(bt_win_classic_connectable(1, BT_PAIR_TR_LE, true));   /* a bonded classic host may still reconnect */
    CT_CHECK(bt_win_classic_connectable(1, BT_PAIR_TR_CLASSIC, false) && bt_win_classic_connectable(1, BT_PAIR_TR_ANY, false));
}

static void test_pair_fail_line(void) {
    char l[120];
    bt_pair_fail_line(l, sizeof l, "le", "AA:BB:CC:DD:EE:FF", "SMP reason", 0x05);
    CT_CHECK(!strcmp(l, "pairing failed: transport le, peer AA:BB:CC:DD:EE:FF, SMP reason 0x05"));
    bt_pair_fail_line(l, sizeof l, "classic", "11:22:33:44:55:66", "HCI status", 0x18);
    CT_CHECK(!strcmp(l, "pairing failed: transport classic, peer 11:22:33:44:55:66, HCI status 0x18"));
    CT_CHECK(!strchr(l, '\n'));
}

/* the refused / sent-after-waiting lines are debug level: the default log stays silent */
static int s_log_n, s_dbg_n;
static void c_log(const char *l) { (void)l; s_log_n++; }
static void c_dbg(const char *l) { (void)l; s_dbg_n++; }
static void test_hidq_log_levels(void) {
    uint8_t rel[10] = {0xA1, 1};
    s_sent_n = 0;
    hidq_init(q_send, q_req, c_log);
    hidq_set_debug_log(c_dbg);
    s_accept = 0;
    hidq_send(9, rel, 10, 0); /* refused, queued */
    s_accept = 1;
    hidq_service(9, 1); /* sent after waiting */
    CT_EQ(s_log_n, 0);
    CT_CHECK(s_dbg_n >= 2);
    hidq_set_debug_log(NULL); /* default: dropped, no crash */
    s_accept = 0;
    hidq_send(9, rel, 10, 2);
    s_accept = 1;
    hidq_service(9, 3);
    CT_EQ(s_log_n, 0);
}

/* ---- LE static random address: valid, persisted, reused ---- */
static bool rnd_fixed(uint8_t *b, size_t n) {
    static const uint8_t v[6] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC}; /* top bits not 11: generate forces them */
    memcpy(b, v, n);
    return true;
}
static bool rnd_zero(uint8_t *b, size_t n) { memset(b, 0, n); return true; }
static bool rnd_fail(uint8_t *b, size_t n) { (void)b; (void)n; return false; }

static void test_leaddr(void) {
    uint8_t a[6], b[6];
    CT_CHECK(bt_leaddr_generate(a, NULL) && bt_leaddr_valid(a)); /* real urandom: top two bits 11 */
    CT_EQ(a[0] & 0xC0, 0xC0);
    CT_CHECK(bt_leaddr_generate(a, rnd_fixed) && a[0] == 0xD2 && a[5] == 0xBC);
    uint8_t bad1[6] = {0x3F, 1, 2, 3, 4, 5}, bad2[6] = {0xC0, 0, 0, 0, 0, 0}, bad3[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    CT_CHECK(!bt_leaddr_valid(bad1) && !bt_leaddr_valid(bad2) && !bt_leaddr_valid(bad3));
    CT_CHECK(!bt_leaddr_generate(a, rnd_zero)); /* an all-zero random part is never accepted, 0xC0 forced or not */
    CT_CHECK(!bt_leaddr_generate(a, rnd_fail));

    char dir[] = "/tmp/hh_leaddr_XXXXXX", path[80];
    CT_CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/le_addr", dir);
    CT_EQ(bt_leaddr_load_or_create(path, a, NULL), 1); /* created */
    CT_CHECK(bt_leaddr_valid(a));
    CT_EQ(bt_leaddr_load_or_create(path, b, rnd_fixed), 0); /* reused, never regenerated */
    CT_CHECK(!memcmp(a, b, 6));
    FILE *fp = fopen(path, "w"); /* a damaged file is replaced by a valid address */
    fputs("xx", fp);
    fclose(fp);
    CT_EQ(bt_leaddr_load_or_create(path, b, rnd_fixed), 1);
    CT_CHECK(bt_leaddr_valid(b) && b[0] == 0xD2);
    CT_EQ(bt_leaddr_load_or_create(path, a, NULL), 0);
    CT_CHECK(!memcmp(a, b, 6));
    char tmp[90];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    CT_CHECK(access(tmp, F_OK) != 0); /* the atomic write leaves no temp file */
    unlink(path);
    CT_EQ(bt_leaddr_load_or_create("/nonexistent_dir_hh/le_addr", a, NULL), -1); /* cannot persist: not to be used */
    rmdir(dir);
}

/* ---- advertising flags / data refresh when the window changes while advertising ---- */
static void test_adv_action(void) {
    CT_EQ(bt_win_adv_flags(true), 0x06);
    CT_EQ(bt_win_adv_flags(false), 0x04);
    CT_EQ(bt_win_adv_action(false, true, 0, 0x06), BT_ADV_START);
    CT_EQ(bt_win_adv_action(true, false, 0x06, 0x04), BT_ADV_STOP);
    CT_EQ(bt_win_adv_action(true, true, 0x04, 0x06), BT_ADV_REFRESH); /* a window opened under reconnect advertising */
    CT_EQ(bt_win_adv_action(true, true, 0x06, 0x04), BT_ADV_REFRESH); /* the window closed, a reconnect keeps advertising */
    CT_EQ(bt_win_adv_action(true, true, 0x06, 0x06), BT_ADV_NONE);
    CT_EQ(bt_win_adv_action(false, false, 0x06, 0x04), BT_ADV_NONE);
}

/* ---- HCI trace: line format and the size cap ---- */
static void test_trace(void) {
    char l[256];
    uint8_t pkt[4] = {0x0E, 0x04, 0x01, 0x0A};
    size_t n = bt_trace_format(l, sizeof l, 12, 1, 4, pkt, 4);
    CT_CHECK(n == strlen(l) && !strcmp(l, "12 < type=4 len=4 0e 04 01 0a"));
    uint8_t big[100] = {0};
    bt_trace_format(l, sizeof l, 1, 0, 2, big, 100);
    CT_CHECK(strstr(l, "...") && strlen(l) < 200);

    /* key material never reaches the trace */
    uint8_t lk[26] = {0x18, 24, 1, 2, 3, 4, 5, 6, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 5};
    bt_trace_format(l, sizeof l, 0, 1, 4, lk, 26);
    CT_CHECK(!strstr(l, "aa") && strstr(l, "05 06 **"));
    uint8_t cmd[35] = {0x19, 0x20, 28, 0x40, 0, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB};
    bt_trace_format(l, sizeof l, 0, 0, 1, cmd, 35); /* LE Start Encryption */
    CT_CHECK(!strstr(l, "bb") && strstr(l, "40 00 **"));
    uint8_t lkr[25] = {0x0B, 0x04, 22, 1, 2, 3, 4, 5, 6, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    bt_trace_format(l, sizeof l, 0, 0, 1, lkr, 25);
    CT_CHECK(!strstr(l, "cc") && strstr(l, "06 **"));
    uint8_t ltk[23] = {0x1A, 0x20, 18, 0x40, 0, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD};
    bt_trace_format(l, sizeof l, 0, 0, 1, ltk, 23);
    CT_CHECK(!strstr(l, "dd") && strstr(l, "40 00 **"));
    uint8_t enc[35] = {0x17, 0x20, 32, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
    bt_trace_format(l, sizeof l, 0, 0, 1, enc, 35);
    CT_CHECK(!strstr(l, "ee") && strstr(l, "17 20 20 **"));
    uint8_t encr[22] = {0x0E, 20, 1, 0x17, 0x20, 0, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99};
    bt_trace_format(l, sizeof l, 0, 1, 4, encr, 22);
    CT_CHECK(!strstr(l, "99") && strstr(l, "20 00 **"));
    /* SMP: Encryption Info (key), Identity Info, Signing Info on CID 6 are redacted; Pairing Request (0x01) is not */
    uint8_t smp[26] = {0x40, 0x20, 21, 0, 17, 0, 6, 0, 0x06, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77};
    for (int i = 0; i < 3; i++) {
        smp[8] = i == 0 ? 0x06 : i == 1 ? 0x08 : 0x0A;
        bt_trace_format(l, sizeof l, 0, 1, 2, smp, 26);
        CT_CHECK(!strstr(l, "77") && strstr(l, "06 00 0"));
    }
    smp[8] = 0x01;
    bt_trace_format(l, sizeof l, 0, 1, 2, smp, 26);
    CT_CHECK(strstr(l, "77") && !strstr(l, "**"));

    char dir[] = "/tmp/hh_trace_XXXXXX";
    CT_CHECK(mkdtemp(dir) != NULL);
    static char path[80];
    snprintf(path, sizeof path, "%s/t.log", dir);
    CT_EQ(bt_trace_start(path, 2000), 0);
    for (int i = 0; i < 200; i++) hci_dump_packet(1, 1, pkt, 4);
    bt_trace_stop();
    struct stat st;
    CT_EQ(stat(path, &st), 0);
    CT_CHECK(st.st_size > 0 && st.st_size <= 2000); /* capped: the file restarted instead of growing */
    CT_EQ(st.st_mode & 0777, 0600);
    unlink(path);
    rmdir(dir);
}

/* a file that cannot be read is neither replaced nor used */
static void test_leaddr_unreadable(void) {
    char dir[] = "/tmp/hh_leaddr2_XXXXXX", p[90];
    CT_CHECK(mkdtemp(dir) != NULL);
    uint8_t a[6];
    snprintf(p, sizeof p, "%s/le_addr", dir);
    CT_CHECK(mkdir(p, 0700) == 0); /* open() works, read() fails with EISDIR */
    CT_EQ(bt_leaddr_load_or_create(p, a, NULL), -1);
    struct stat st;
    CT_CHECK(stat(p, &st) == 0 && S_ISDIR(st.st_mode)); /* untouched */
    rmdir(p);
    char nd[100];
    snprintf(nd, sizeof nd, "%s/x", dir);
    FILE *fp = fopen(nd, "w"); /* a path through a regular file: ENOTDIR, not ENOENT */
    fclose(fp);
    snprintf(p, sizeof p, "%s/x/le_addr", dir);
    CT_EQ(bt_leaddr_load_or_create(p, a, NULL), -1);
    unlink(nd);
    rmdir(dir);
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
    CT_RUN(test_att_db_gatt_service);
    CT_RUN(test_service_changed);
    CT_RUN(test_hidq_overflow);
    CT_RUN(test_window_transport);
    CT_RUN(test_pair_fail_line);
    CT_RUN(test_hidq_log_levels);
    CT_RUN(test_leaddr);
    CT_RUN(test_pktq);
    CT_RUN(test_adv_action);
    CT_RUN(test_trace);
    CT_RUN(test_leaddr_unreadable);
    return CT_DONE();
}
