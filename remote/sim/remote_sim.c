/* Headless remote simulator: the remote end of the simulated RF link.
 *
 * usage: remote_sim --rf PATH (--press KEY | --hold KEY,MS | --dump-page | --dump-layout | --tap X,Y)
 *                   [--remote ID] [--action 0|1|2] [--timeout-ms N] [--settle-ms N]
 *                   [--hold-interval-ms N]
 *
 *   --rf PATH        Unix SOCK_SEQPACKET socket of `hub --sim --rf PATH` (required).
 *   --press KEY      Connect, send one key_event {remote, key, action} (remote
 *                    default 1, action default 0 = press), exit 0. KEY is a key name
 *                    (VolumeUp or volume_up, case-insensitive; the `keys:` table in
 *                    proto/schema.yaml) or a numeric logical code 0..255.
 *   --hold KEY,MS Connect, send key_event PRESS, then a HOLD every --hold-interval-ms
 *                    (default HP_IROUT_HOLD_INTERVAL_MS = 400, the real remote's period) for
 *                    MS milliseconds of holding, then RELEASE; exit 0. MS 0 = PRESS + RELEASE.
 *   --dump-page      Connect, print "remote_sim: attached" to stderr, wait for
 *                    the next page_push (up to --timeout-ms, default 5000),
 *                    print it as ONE JSON line on stdout
 *                      {"remote":R,"page":P,"data":"<base64>"}
 *                    and exit 0. Other hub->remote messages are ignored.
 *   --dump-layout    Wait for a page_push, decode it (remote/page/page.h), keep
 *                    receiving page_push/tile_update until the link is quiet for
 *                    --settle-ms (default 150), then print the LAST pushed page as
 *                    ONE JSON line {"remote","page","cols","rows","widgets":[
 *                    {"kind","id","x","y","w","h","label"[,"text"]}...]}
 *                    ("text" = live tile text from tile_update). Undecodable pages
 *                    are reported on stderr and ignored.
 *   --tap X,Y        As --dump-layout, then hit-test pixel X,Y (0..239, 0..319)
 *                    on that page. Button/tile hit: send key_event press then
 *                    release with key 128 + widget id, print {"hit":true,"id":N,
 *                    "key":K}. Miss (empty cell or label): send nothing, print
 *                    {"hit":false} and say so on stderr. Exit 0 either way.
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

#include "cJSON.h"
#include "hub/engine/irout.h"
#include "hub/hp_errors.h"
#include "hub/io/rf_pair.h"
#include "hub/net/ws.h"
#include "hub/parse_num.h"
#include "hub/rf/framing.h"
#include "hub/rf/rf_payload.h"
#include "remote/page/page.h"

#define SIM_RF_SLOT 1 /* the sim socket carries hub-mode (30 B) frames both ways */

static int usage(void) {
    fprintf(stderr, "usage: remote_sim --rf PATH (--press KEY | --hold KEY,MS | --dump-page | --dump-layout | --tap X,Y) "
                    "[--remote ID] [--action 0|1|2] [--timeout-ms N] [--settle-ms N] [--hold-interval-ms N]\n"
                    "KEY is a key name (VolumeUp or volume_up; keys: in proto/schema.yaml) or a logical code 0..255\n");
    return 2;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One received frame through unwrap + reassembly: 1 when it completed a new object. */
static int rx_complete(hp_rf_asm_t *as, const uint8_t *f, size_t n, hp_rf_msg_t *msg) {
    hp_rf_inner_t in, ack;
    uint8_t slot;
    if (hp_rf_unwrap_hub(f, n, &slot, &in) < 0) return 0;
    return hp_rf_asm_push(as, &in, msg, &ack) == HP_ASM_COMPLETE;
}

/* KEY is a numeric logical code 0..255 or a key name (VolumeUp / volume_up, case-insensitive). */
static int parse_key(const char *s, long *out) {
    if (!hp_parse_num(s, 0, 255, out)) return 0;
    const hp_hwkey_t *k = hp_hwkey_by_name(s);
    if (!k) return -1;
    *out = k->id;
    return 0;
}

static int send_key(hp_rf_transport *rf, uint8_t seq, long remote, long key, long action) {
    hp_rf_val_t v[3] = {{.u = (uint32_t)remote}, {.u = (uint32_t)key}, {.u = (uint32_t)action}};
    uint8_t payload[HP_RF_MSG_MAX];
    hp_rf_inner_t fr[HP_RF_MAX_FRAGS];
    size_t plen = 0, nf = 0;
    if (hp_rf_pack(HP_MSG_KEY_EVENT, v, 3, payload, sizeof payload, &plen) < 0 ||
        hp_rf_split(HP_MSG_KEY_EVENT, seq, payload, plen, fr, HP_RF_MAX_FRAGS, &nf) < 0) {
        fprintf(stderr, "remote_sim: cannot encode key_event\n");
        return 1;
    }
    for (size_t i = 0; i < nf; i++) {
        hp_rf_frame_t w;
        hp_rf_wrap_hub(SIM_RF_SLOT, &fr[i], &w); /* fire-and-forget: the sim link is reliable */
        if (rf->send(rf->self, w.data, w.len) < 0) {
            fprintf(stderr, "remote_sim: RF send failed\n");
            return 1;
        }
    }
    return 0;
}

static int press(hp_rf_transport *rf, long remote, long key, long action) {
    return send_key(rf, 1, remote, key, action);
}

static int hold(hp_rf_transport *rf, long remote, long key, long total_ms, long interval_ms) {
    uint8_t seq = 1;
    if (send_key(rf, seq++, remote, key, HP_KEY_ACTION_PRESS)) return 1;
    long start = now_ms();
    for (long next = interval_ms; next < total_ms; next += interval_ms) {
        long wait = start + next - now_ms();
        if (wait > 0) {
            struct timespec ts = {wait / 1000, (wait % 1000) * 1000000L};
            nanosleep(&ts, NULL);
        }
        if (send_key(rf, seq++, remote, key, HP_KEY_ACTION_HOLD)) return 1;
    }
    long wait = start + total_ms - now_ms();
    if (wait > 0) {
        struct timespec ts = {wait / 1000, (wait % 1000) * 1000000L};
        nanosleep(&ts, NULL);
    }
    return send_key(rf, seq, remote, key, HP_KEY_ACTION_RELEASE);
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
        if (!rx_complete(&as, fr, (size_t)n, &msg) || msg.type != HP_MSG_PAGE_PUSH) continue;
        hp_rf_val_t v[3];
        if (hp_rf_unpack(HP_MSG_PAGE_PUSH, msg.payload, msg.len, v, 3) < 0) continue;
        char b64[4 * ((255 + 2) / 3) + 1];
        hp_base64_encode(v[2].p, v[2].len, b64);
        printf("{\"remote\":%u,\"page\":%u,\"data\":\"%s\"}\n", (unsigned)v[0].u, (unsigned)v[1].u, b64);
        fflush(stdout);
        return 0;
    }
}

#define SIM_PAGES 8

static struct {
    hp_page_t pages[SIM_PAGES];
    int used[SIM_PAGES];
    int last;
    unsigned last_remote;
} st = {.last = -1};

static int on_page(const hp_rf_val_t *v) {
    hp_page_t p;
    int rc = hp_page_decode(v[2].p, v[2].len, &p);
    if (rc < 0) {
        fprintf(stderr, "remote_sim: rejected page_push (page %u): error %d\n", (unsigned)v[1].u, -rc);
        return 0;
    }
    int slot = -1;
    for (int i = 0; i < SIM_PAGES; i++)
        if (st.used[i] && st.pages[i].page_id == p.page_id) slot = i;
    for (int i = 0; slot < 0 && i < SIM_PAGES; i++)
        if (!st.used[i]) slot = i;
    if (slot < 0) slot = (st.last + 1) % SIM_PAGES; /* full: evict round-robin */
    st.pages[slot] = p;
    st.used[slot] = 1;
    st.last = slot;
    st.last_remote = (unsigned)v[0].u;
    return 1;
}

static void on_tile(const hp_rf_val_t *v) {
    for (int i = 0; i < SIM_PAGES; i++)
        if (st.used[i] && st.pages[i].page_id == v[1].u) {
            if (hp_page_set_tile_text(&st.pages[i], (uint8_t)v[2].u, (const char *)v[3].p, v[3].len) < 0)
                fprintf(stderr, "remote_sim: ignored tile_update (tile %u)\n", (unsigned)v[2].u);
            return;
        }
    fprintf(stderr, "remote_sim: tile_update for unknown page %u\n", (unsigned)v[1].u);
}

/* Receive until timeout_ms of quiet (or, with stop_on_page, until a page is stored).
 * 1 = a page arrived (stop_on_page), 0 = quiet/timeout, -1 = hub gone. */
static int pump(hp_rf_transport *rf, hp_rf_asm_t *as, long timeout_ms, int stop_on_page) {
    hp_rf_msg_t msg;
    uint8_t fr[HP_RF_FRAME_MAX + 1];
    long deadline = now_ms() + timeout_ms;
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0) return 0;
        int n = rf->recv(rf->self, fr, sizeof fr, (int)left);
        if (n < 0) return -1;
        if (n == 0 || n > HP_RF_FRAME_MAX) continue;
        if (!rx_complete(as, fr, (size_t)n, &msg)) continue;
        hp_rf_val_t v[4];
        if (msg.type == HP_MSG_PAGE_PUSH && hp_rf_unpack(HP_MSG_PAGE_PUSH, msg.payload, msg.len, v, 3) == 0) {
            if (on_page(v) && stop_on_page) return 1;
            if (!stop_on_page) deadline = now_ms() + timeout_ms;
        } else if (msg.type == HP_MSG_TILE_UPDATE &&
                   hp_rf_unpack(HP_MSG_TILE_UPDATE, msg.payload, msg.len, v, 4) == 0) {
            on_tile(v);
            if (!stop_on_page) deadline = now_ms() + timeout_ms;
        }
    }
}

/* Wait for a page, then let tile_updates settle. 0 ok, else an exit code. */
static int collect_page(hp_rf_transport *rf, long timeout_ms, long settle_ms) {
    hp_rf_asm_t as;
    hp_rf_asm_init(&as);
    int rc = pump(rf, &as, timeout_ms, 1);
    if (rc < 0) { fprintf(stderr, "remote_sim: hub disconnected\n"); return 1; }
    if (rc == 0) { fprintf(stderr, "remote_sim: timed out waiting for page_push\n"); return 3; }
    while ((rc = pump(rf, &as, settle_ms, 0)) == 1) {}
    if (rc < 0) { fprintf(stderr, "remote_sim: hub disconnected\n"); return 1; }
    return 0;
}

static int dump_layout(hp_rf_transport *rf, long timeout_ms, long settle_ms) {
    int rc = collect_page(rf, timeout_ms, settle_ms);
    if (rc) return rc;
    const hp_page_t *p = &st.pages[st.last];
    cJSON *o = cJSON_CreateObject(), *ws = cJSON_CreateArray();
    cJSON_AddNumberToObject(o, "remote", st.last_remote);
    cJSON_AddNumberToObject(o, "page", p->page_id);
    cJSON_AddNumberToObject(o, "cols", p->cols);
    cJSON_AddNumberToObject(o, "rows", p->rows);
    for (unsigned i = 0; i < p->count; i++) {
        const hp_widget_t *w = &p->widgets[i];
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "kind", w->kind == HP_WIDGET_BUTTON ? "button" : w->kind == HP_WIDGET_TILE ? "tile" : "label");
        cJSON_AddNumberToObject(j, "id", w->id);
        cJSON_AddNumberToObject(j, "x", w->x);
        cJSON_AddNumberToObject(j, "y", w->y);
        cJSON_AddNumberToObject(j, "w", w->w);
        cJSON_AddNumberToObject(j, "h", w->h);
        cJSON_AddStringToObject(j, "label", w->label);
        if (w->kind == HP_WIDGET_TILE) cJSON_AddStringToObject(j, "text", w->text);
        cJSON_AddItemToArray(ws, j);
    }
    cJSON_AddItemToObject(o, "widgets", ws);
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!txt) { fprintf(stderr, "remote_sim: out of memory\n"); return 1; }
    printf("%s\n", txt);
    fflush(stdout);
    free(txt);
    return 0;
}

static int tap(hp_rf_transport *rf, long remote, long x, long y, long timeout_ms, long settle_ms) {
    int rc = collect_page(rf, timeout_ms, settle_ms);
    if (rc) return rc;
    const hp_widget_t *w = hp_page_hit(&st.pages[st.last], (int)x, (int)y);
    if (!w || w->kind == HP_WIDGET_LABEL) {
        fprintf(stderr, "remote_sim: tap %ld,%ld hit %s: no key_event sent\n", x, y, w ? "a label" : "nothing");
        printf("{\"hit\":false}\n");
        fflush(stdout);
        return 0;
    }
    long key = 128 + w->id;
    if (send_key(rf, 1, remote, key, 0) || send_key(rf, 2, remote, key, 1)) return 1;
    printf("{\"hit\":true,\"id\":%u,\"key\":%ld}\n", (unsigned)w->id, key);
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv) {
    const char *path = NULL;
    long hold_key = -1, hold_ms = 0, hold_iv = HP_IROUT_HOLD_INTERVAL_MS;
    long key = -1, remote = 1, action = 0, timeout_ms = 5000, settle_ms = 150, tx = -1, ty = -1;
    int dump = 0, layout = 0, do_tap = 0;
    for (int i = 1; i < argc; i++) {
        int more = i + 1 < argc;
        if (!strcmp(argv[i], "--rf") && more) path = argv[++i];
        else if (!strcmp(argv[i], "--press") && more) { if (parse_key(argv[++i], &key)) return usage(); }
        else if (!strcmp(argv[i], "--hold") && more) {
            char buf[32], *comma;
            snprintf(buf, sizeof buf, "%s", argv[++i]);
            if (!(comma = strchr(buf, ',')) || strlen(argv[i]) >= sizeof buf) return usage();
            *comma = 0;
            if (parse_key(buf, &hold_key) || hp_parse_num(comma + 1, 0, 600000, &hold_ms)) return usage();
        }
        else if (!strcmp(argv[i], "--hold-interval-ms") && more) { if (hp_parse_num(argv[++i], 1, 60000, &hold_iv)) return usage(); }
        else if (!strcmp(argv[i], "--remote") && more) { if (hp_parse_num(argv[++i], 0, 255, &remote)) return usage(); }
        else if (!strcmp(argv[i], "--action") && more) { if (hp_parse_num(argv[++i], 0, 2, &action)) return usage(); }
        else if (!strcmp(argv[i], "--timeout-ms") && more) { if (hp_parse_num(argv[++i], 1, 3600000, &timeout_ms)) return usage(); }
        else if (!strcmp(argv[i], "--settle-ms") && more) { if (hp_parse_num(argv[++i], 0, 60000, &settle_ms)) return usage(); }
        else if (!strcmp(argv[i], "--tap") && more) {
            char buf[32], *comma;
            snprintf(buf, sizeof buf, "%s", argv[++i]);
            if (!(comma = strchr(buf, ',')) || strlen(argv[i]) >= sizeof buf) return usage();
            *comma = 0;
            if (hp_parse_num(buf, 0, HP_PAGE_SCREEN_W - 1, &tx) || hp_parse_num(comma + 1, 0, HP_PAGE_SCREEN_H - 1, &ty)) return usage();
            do_tap = 1;
        }
        else if (!strcmp(argv[i], "--dump-layout")) layout = 1;
        else if (!strcmp(argv[i], "--dump-page")) dump = 1;
        else return usage();
    }
    if (!path || (key >= 0) + (hold_key >= 0) + dump + layout + do_tap != 1) return usage(); /* exactly one mode */

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
    rc = dump     ? dump_page(rf, timeout_ms)
         : layout ? dump_layout(rf, timeout_ms, settle_ms)
         : do_tap ? tap(rf, remote, tx, ty, timeout_ms, settle_ms)
         : hold_key >= 0 ? hold(rf, remote, hold_key, hold_ms, hold_iv)
                  : press(rf, remote, key, action);
    hp_rf_pair_destroy(rf);
    return rc;
}
