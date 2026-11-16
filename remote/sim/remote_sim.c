/* Headless remote simulator: the remote end of the simulated RF link.
 *
 * usage: remote_sim --rf PATH (--press KEY | --dump-page) [--remote ID]
 *                   [--action 0|1|2] [--timeout-ms N]
 *
 *   --rf PATH        Unix SOCK_SEQPACKET socket of `hub --sim --rf PATH` (required).
 *   --press KEY      Connect, send one key_event {remote, key, action} (remote
 *                    default 1, action default 0 = press), exit 0. KEY is a
 *                    key code 0..255.
 *   --dump-page      Connect, print "remote_sim: attached" to stderr, wait for
 *                    the next page_push (up to --timeout-ms, default 5000),
 *                    print it as ONE JSON line on stdout
 *                      {"remote":R,"page":P,"data":"<base64>"}
 *                    and exit 0. Other hub->remote messages are ignored.
 *   --remote ID, --action A, --timeout-ms N   as above (ID 0..255, A 0..2).
 *
 * Exit status: 0 ok; 1 link failure (cannot connect, refused because another
 * remote is attached, hub gone); 2 usage error; 3 timeout waiting for a page.
 * A one-line reason is written to stderr on failure. Single threaded; the
 * transport is claimed once (single-owner rule). */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hub/hp_errors.h"
#include "hub/io/rf_pair.h"
#include "hub/net/ws.h"
#include "hub/rf/framing.h"
#include "hub/rf/rf_payload.h"

#define REMOTE_RF_CAP 32 /* remote->hub total frame size */

static int usage(void) {
    fprintf(stderr, "usage: remote_sim --rf PATH (--press KEY | --dump-page) [--remote ID] "
                    "[--action 0|1|2] [--timeout-ms N]\n");
    return 2;
}

static int parse_num(const char *s, long lo, long hi, long *out) {
    char *end;
    long v = strtol(s, &end, 10);
    if (!*s || *end || v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int press(hp_rf_transport *rf, long remote, long key, long action) {
    hp_rf_val_t v[3] = {{.u = (uint32_t)remote}, {.u = (uint32_t)key}, {.u = (uint32_t)action}};
    uint8_t payload[HP_RF_MSG_MAX];
    hp_rf_frame_t fr[HP_RF_MAX_FRAGS];
    size_t plen = 0, nf = 0;
    if (hp_rf_pack(HP_MSG_KEY_EVENT, v, 3, payload, sizeof payload, &plen) < 0 ||
        hp_rf_split(HP_MSG_KEY_EVENT, 1, payload, plen, REMOTE_RF_CAP, fr, HP_RF_MAX_FRAGS, &nf) < 0) {
        fprintf(stderr, "remote_sim: cannot encode key_event\n");
        return 1;
    }
    for (size_t i = 0; i < nf; i++)
        if (rf->send(rf->self, fr[i].data, fr[i].len) < 0) {
            fprintf(stderr, "remote_sim: RF send failed\n");
            return 1;
        }
    return 0;
}

static int dump_page(hp_rf_transport *rf, long timeout_ms) {
    hp_rf_asm_t as;
    hp_rf_msg_t msg;
    uint8_t fr[HP_RF_FRAME_MAX + 1];
    hp_rf_asm_init(&as);
    long deadline = now_ms() + timeout_ms;
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0) {
            fprintf(stderr, "remote_sim: timed out waiting for page_push\n");
            return 3;
        }
        int n = rf->recv(rf->self, fr, sizeof fr, (int)left);
        if (n < 0) {
            fprintf(stderr, "remote_sim: hub disconnected\n");
            return 1;
        }
        if (n == 0 || n > HP_RF_FRAME_MAX) continue;
        if (hp_rf_asm_push(&as, fr, (size_t)n, &msg) != HP_ASM_COMPLETE || msg.type != HP_MSG_PAGE_PUSH)
            continue;
        hp_rf_val_t v[3];
        if (hp_rf_unpack(HP_MSG_PAGE_PUSH, msg.payload, msg.len, v, 3) < 0) continue;
        char b64[4 * ((255 + 2) / 3) + 1];
        hp_base64_encode(v[2].p, v[2].len, b64);
        printf("{\"remote\":%u,\"page\":%u,\"data\":\"%s\"}\n", (unsigned)v[0].u, (unsigned)v[1].u, b64);
        fflush(stdout);
        return 0;
    }
}

int main(int argc, char **argv) {
    const char *path = NULL;
    long key = -1, remote = 1, action = 0, timeout_ms = 5000;
    int dump = 0;
    for (int i = 1; i < argc; i++) {
        int more = i + 1 < argc;
        if (!strcmp(argv[i], "--rf") && more) path = argv[++i];
        else if (!strcmp(argv[i], "--press") && more) { if (parse_num(argv[++i], 0, 255, &key)) return usage(); }
        else if (!strcmp(argv[i], "--remote") && more) { if (parse_num(argv[++i], 0, 255, &remote)) return usage(); }
        else if (!strcmp(argv[i], "--action") && more) { if (parse_num(argv[++i], 0, 2, &action)) return usage(); }
        else if (!strcmp(argv[i], "--timeout-ms") && more) { if (parse_num(argv[++i], 1, 3600000, &timeout_ms)) return usage(); }
        else if (!strcmp(argv[i], "--dump-page")) dump = 1;
        else return usage();
    }
    if (!path || (key < 0) == !dump) return usage(); /* exactly one mode */

    hp_rf_transport *rf = NULL;
    if (hp_rf_pair_connect(path, &rf) < 0) {
        fprintf(stderr, "remote_sim: cannot connect to %s\n", path);
        return 1;
    }
    if (rf->claim(rf->self) < 0) return 1;
    int rc = hp_rf_pair_wait_attached(rf, 2000);
    if (rc < 0) {
        fprintf(stderr, rc == -HP_E_BUSY ? "remote_sim: hub refused: another remote is attached\n"
                                         : "remote_sim: no handshake from hub\n");
        hp_rf_pair_destroy(rf);
        return 1;
    }
    fprintf(stderr, "remote_sim: attached\n");
    fflush(stderr);
    rc = dump ? dump_page(rf, timeout_ms) : press(rf, remote, key, action);
    hp_rf_pair_destroy(rf);
    return rc;
}
