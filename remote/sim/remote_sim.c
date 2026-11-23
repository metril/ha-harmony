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
 *   --info CAPS      (opt-in) first announce remote_info {remote, page_fmt 2, caps CAPS (default 3: tile_state +
 *                    page store), icons 50} the way the real remote does after link-up, then do the mode. The
 *                    hub caches it and relays it on ws; tile_update commands then arrive as tile_state.
 *   --stored GEN,PAGES  with --info: the remote_info `gen` / `pages` fields, i.e. the v2 page set this simulated
 *                    remote claims to hold (default 0,0 = none). The sim keeps no store of its own.
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
#include "hub/rf/link.h"
#include "hub/rf/rf_payload.h"
#include "remote/page/page.h"
#include "remote/page/store.h"

#define SIM_RF_SLOT 1 /* the sim socket carries hub-mode (30 B) frames both ways */

static int usage(void) {
    fprintf(stderr, "usage: remote_sim --rf PATH (--press KEY | --hold KEY,MS | --dump-page | --dump-layout | --tap X,Y) "
                    "[--remote ID] [--action 0|1|2] [--timeout-ms N] [--settle-ms N] [--hold-interval-ms N] [--info CAPS] [--stored GEN,PAGES]\n"
                    "KEY is a key name (VolumeUp or volume_up; keys: in proto/schema.yaml) or a logical code 0..255\n");
    return 2;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The simulated remote speaks the same object link as the hub (acks, retries, duplicate
 * suppression); completed objects land in a small ring that the callers drain. */
#define SIM_INBOX 8
static hp_rf_link_t g_link;
static hp_rf_msg_t g_inbox[SIM_INBOX];
static unsigned g_in_head, g_in_n;

static void on_deliver(void *ctx, const hp_rf_msg_t *m) {
    (void)ctx;
    if (g_in_n == SIM_INBOX) return; /* callers drain every pump; a flood past that is dropped */
    g_inbox[(g_in_head + g_in_n++) % SIM_INBOX] = *m;
}

/* KEY is a numeric logical code 0..255 or a key name (VolumeUp / volume_up, case-insensitive). */
static int parse_key(const char *s, long *out) {
    if (!hp_parse_num(s, 0, 255, out)) return 0;
    const hp_hwkey_t *k = hp_hwkey_by_name(s);
    if (!k) return -1;
    *out = k->id;
    return 0;
}

static void nap_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* Pumps the link once. 0 ok, -1 hub gone. */
static int link_step(void) { return hp_rf_link_pump(&g_link, (uint32_t)now_ms()) < 0 ? -1 : 0; }

/* Pops one received object. */
static int inbox_pop(hp_rf_msg_t *m) {
    if (!g_in_n) return 0;
    *m = g_inbox[g_in_head];
    g_in_head = (g_in_head + 1) % SIM_INBOX;
    g_in_n--;
    return 1;
}

/* Sends one key_event and waits (bounded by the link's retries) for the hub's ack. 0 ok, 1 on failure. */
static int send_key(long remote, long key, long action) {
    hp_rf_val_t v[3] = {{.u = (uint32_t)remote}, {.u = (uint32_t)key}, {.u = (uint32_t)action}};
    uint8_t payload[HP_RF_MSG_MAX];
    size_t plen = 0;
    if (hp_rf_pack(HP_MSG_KEY_EVENT, v, 3, payload, sizeof payload, &plen) < 0) {
        fprintf(stderr, "remote_sim: cannot encode key_event\n");
        return 1;
    }
    uint32_t fails = hp_rf_link_status(&g_link).failures;
    if (hp_rf_link_send(&g_link, HP_MSG_KEY_EVENT, payload, plen, (uint32_t)now_ms()) < 0) {
        fprintf(stderr, "remote_sim: RF send failed\n");
        return 1;
    }
    while (hp_rf_link_status(&g_link).queued) {
        if (link_step() < 0 || hp_rf_link_status(&g_link).failures != fails) {
            fprintf(stderr, "remote_sim: RF send failed\n");
            return 1;
        }
        nap_ms(1);
    }
    return 0;
}

/* remote_info, as the real remote sends it after link-up. 0 ok, 1 on failure. */
static long g_stored_gen, g_stored_pages; /* --stored */

static int send_info(long remote, long caps) {
    hp_rf_val_t v[7] = {{.u = (uint32_t)remote}, {.u = 2}, {.u = (uint32_t)caps}, {.u = 50},
                        {.u = (uint32_t)g_stored_gen}, {.u = (uint32_t)g_stored_pages}, {.u = 0x5151}}; /* fixed nonce: the sim reboots every run */
    uint8_t payload[HP_RF_MSG_MAX];
    size_t plen = 0;
    if (hp_rf_pack(HP_MSG_REMOTE_INFO, v, 7, payload, sizeof payload, &plen) < 0) return 1;
    uint32_t fails = hp_rf_link_status(&g_link).failures;
    if (hp_rf_link_send(&g_link, HP_MSG_REMOTE_INFO, payload, plen, (uint32_t)now_ms()) < 0) return 1;
    while (hp_rf_link_status(&g_link).queued) {
        if (link_step() < 0 || hp_rf_link_status(&g_link).failures != fails) return 1;
        nap_ms(1);
    }
    return 0;
}

static int press(long remote, long key, long action) {
    return send_key(remote, key, action);
}

static int hold(long remote, long key, long total_ms, long interval_ms) {
    if (send_key(remote, key, HP_KEY_ACTION_PRESS)) return 1;
    long start = now_ms();
    for (long next = interval_ms; next < total_ms; next += interval_ms) {
        long wait = start + next - now_ms();
        if (wait > 0) nap_ms(wait);
        if (send_key(remote, key, HP_KEY_ACTION_HOLD)) return 1;
    }
    long wait = start + total_ms - now_ms();
    if (wait > 0) nap_ms(wait);
    return send_key(remote, key, HP_KEY_ACTION_RELEASE);
}

static int dump_page(long timeout_ms) {
    long deadline = now_ms() + timeout_ms;
    hp_rf_msg_t msg;
    for (;;) {
        if (link_step() < 0) {
            fprintf(stderr, "remote_sim: hub disconnected\n");
            return 1;
        }
        while (inbox_pop(&msg)) {
            hp_rf_val_t v[3];
            if (msg.type != HP_MSG_PAGE_PUSH || hp_rf_unpack(HP_MSG_PAGE_PUSH, msg.payload, msg.len, v, 3) < 0) continue;
            char b64[4 * ((255 + 2) / 3) + 1];
            hp_base64_encode(v[2].p, v[2].len, b64);
            printf("{\"remote\":%u,\"page\":%u,\"data\":\"%s\"}\n", (unsigned)v[0].u, (unsigned)v[1].u, b64);
            fflush(stdout);
            return 0;
        }
        if (deadline - now_ms() <= 0) {
            fprintf(stderr, "remote_sim: timed out waiting for page_push\n");
            return 3;
        }
        nap_ms(1);
    }
}

#define SIM_PAGES 252

static struct {
    hp_page_t pages[SIM_PAGES];
    int used[SIM_PAGES];
    int last;
    unsigned last_remote;
    int have_gen;
    uint8_t gen;
} st = {.last = -1};

/* The remote's own completeness rule (remote/page/store.c): a set whose pages use theme id 7 is complete only with the theme
 * of its gen. g_themes counts theme_push objects heard, g_theme_first = a theme_push of the set's gen came before its first page. */
static hp_store_t g_store;
static int g_themes, g_theme_first, g_pages_first;

static void on_theme(const hp_rf_val_t *v) {
    if (v[2].len != HP_THEME_BYTES || v[2].p[0] != 1) {
        fprintf(stderr, "remote_sim: rejected theme_push\n");
        return;
    }
    if (hp_store_theme(&g_store, (uint8_t)v[1].u, v[2].p, v[2].len) < 0) return;
    g_themes++;
    if (!hp_store_count(&g_store)) g_theme_first = 1;
}

static int on_page(const hp_rf_val_t *v) {
    hp_page_t p;
    int rc = hp_page_decode(v[2].p, v[2].len, &p);
    if (rc < 0) {
        fprintf(stderr, "remote_sim: rejected page_push (page %u): error %d\n", (unsigned)v[1].u, -rc);
        return 0;
    }
    if (p.version == 2) {
        if (!hp_store_count(&g_store) && !(g_store.have_theme && g_store.theme_gen == p.gen)) g_pages_first = 1;
        (void)hp_store_put(&g_store, v[2].p, v[2].len);
    }
    if (p.version == 2) { /* a new gen is a new config: forget the old pages */
        if (st.have_gen && st.gen != p.gen) memset(st.used, 0, sizeof st.used);
        st.have_gen = 1;
        st.gen = p.gen;
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

static void on_tile(const hp_rf_val_t *v, int state) {
    for (int i = 0; i < SIM_PAGES; i++)
        if (st.used[i] && st.pages[i].page_id == v[1].u) {
            if (hp_page_set_tile_text(&st.pages[i], (uint8_t)v[2].u, (const char *)v[3].p, v[3].len) < 0)
                fprintf(stderr, "remote_sim: ignored tile_update (tile %u)\n", (unsigned)v[2].u);
            else if (state >= 0)
                for (unsigned k = 0; k < st.pages[i].count; k++)
                    if (st.pages[i].widgets[k].id == v[2].u) st.pages[i].widgets[k].tstate = (uint8_t)state;
            return;
        }
    fprintf(stderr, "remote_sim: tile_update for unknown page %u\n", (unsigned)v[1].u);
}

/* Receive until timeout_ms of quiet (or, with stop_on_page, until a page is stored).
 * 1 = a page arrived (stop_on_page), 0 = quiet/timeout, -1 = hub gone. */
static int pump(long timeout_ms, int stop_on_page) {
    long deadline = now_ms() + timeout_ms;
    hp_rf_msg_t msg;
    for (;;) {
        if (link_step() < 0) return -1;
        while (inbox_pop(&msg)) {
            hp_rf_val_t v[4], v5[5];
            if (msg.type == HP_MSG_PAGE_PUSH && hp_rf_unpack(HP_MSG_PAGE_PUSH, msg.payload, msg.len, v, 3) == 0) {
                if (on_page(v) && stop_on_page) return 1;
                if (!stop_on_page) deadline = now_ms() + timeout_ms;
            } else if (msg.type == HP_MSG_THEME_PUSH && hp_rf_unpack(HP_MSG_THEME_PUSH, msg.payload, msg.len, v, 3) == 0) {
                on_theme(v);
                if (!stop_on_page) deadline = now_ms() + timeout_ms;
            } else if (msg.type == HP_MSG_TILE_UPDATE &&
                       hp_rf_unpack(HP_MSG_TILE_UPDATE, msg.payload, msg.len, v, 4) == 0) {
                on_tile(v, -1);
                if (!stop_on_page) deadline = now_ms() + timeout_ms;
            } else if (msg.type == HP_MSG_TILE_STATE && hp_rf_unpack(HP_MSG_TILE_STATE, msg.payload, msg.len, v5, 5) == 0) {
                hp_rf_val_t t[4] = {v5[0], v5[1], v5[2], v5[4]}; /* remote,page,tile,text */
                on_tile(t, (int)v5[3].u);
                if (!stop_on_page) deadline = now_ms() + timeout_ms;
            }
        }
        if (deadline - now_ms() <= 0) return 0;
        nap_ms(1);
    }
}

/* Wait for a page, then let tile_updates settle. 0 ok, else an exit code. */
static int collect_page(long timeout_ms, long settle_ms) {
    int rc = pump(timeout_ms, 1);
    if (rc < 0) { fprintf(stderr, "remote_sim: hub disconnected\n"); return 1; }
    if (rc == 0) { fprintf(stderr, "remote_sim: timed out waiting for page_push\n"); return 3; }
    while ((rc = pump(settle_ms, 0)) == 1) {}
    if (rc < 0) { fprintf(stderr, "remote_sim: hub disconnected\n"); return 1; }
    return 0;
}

static int dump_layout(long timeout_ms, long settle_ms) {
    int rc = collect_page(timeout_ms, settle_ms);
    if (rc) return rc;
    const hp_page_t *p = &st.pages[st.last];
    cJSON *o = cJSON_CreateObject(), *ws = cJSON_CreateArray();
    cJSON_AddNumberToObject(o, "remote", st.last_remote);
    cJSON_AddNumberToObject(o, "page", p->page_id);
    cJSON_AddNumberToObject(o, "cols", p->cols);
    cJSON_AddNumberToObject(o, "rows", p->rows);
    if (p->version == 2) {
        static const char *kn[] = {"normal", "home", "favorites"};
        cJSON_AddNumberToObject(o, "version", 2);
        cJSON_AddNumberToObject(o, "gen", p->gen);
        cJSON_AddNumberToObject(o, "total", p->total);
        cJSON_AddStringToObject(o, "pkind", kn[p->kind % 3]);
        cJSON_AddBoolToObject(o, "back", p->back);
        if (p->seq_cnt) {
            cJSON *q = cJSON_AddObjectToObject(o, "seq");
            cJSON_AddNumberToObject(q, "idx", p->seq_idx);
            cJSON_AddNumberToObject(q, "cnt", p->seq_cnt);
        } else {
            cJSON_AddNullToObject(o, "seq");
        }
        cJSON_AddNumberToObject(o, "act", p->act);
        cJSON_AddStringToObject(o, "title", p->title);
    }
    if (p->version == 2) { /* the theme slot and the completeness rule, see g_store */
        cJSON_AddNumberToObject(o, "theme_id", p->theme);
        cJSON_AddBoolToObject(o, "complete", hp_store_complete(&g_store));
        cJSON_AddNumberToObject(o, "themes_heard", g_themes);
        cJSON_AddBoolToObject(o, "theme_first", g_theme_first && !g_pages_first);
        if (g_store.have_theme) {
            char hex[2 * HP_THEME_BYTES + 1];
            for (int i = 0; i < HP_THEME_BYTES; i++) snprintf(hex + 2 * i, 3, "%02x", g_store.theme[i]);
            cJSON *t = cJSON_AddObjectToObject(o, "theme");
            cJSON_AddNumberToObject(t, "gen", g_store.theme_gen);
            cJSON_AddStringToObject(t, "hex", hex);
        }
    }
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
        if (w->kind == HP_WIDGET_TILE) {
            cJSON_AddStringToObject(j, "text", w->text);
            if (w->tstate) cJSON_AddNumberToObject(j, "state", w->tstate);
        }
        if (p->version == 2) {
            static const char *rn[] = {"default", "primary", "secondary", "danger", "nav"};
            static const char *sn[] = {"card", "row", "pill", "monogram"};
            cJSON_AddStringToObject(j, "role", rn[w->role % 5]);
            cJSON_AddStringToObject(j, "shape", sn[w->shape % 4]);
            cJSON_AddNumberToObject(j, "icon", w->icon);
            cJSON_AddBoolToObject(j, "icon_only", w->icon_only);
            if (w->has_bind) cJSON_AddNumberToObject(j, "bind", w->bind);
            else cJSON_AddNullToObject(j, "bind");
        }
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

static int tap(long remote, long x, long y, long timeout_ms, long settle_ms) {
    int rc = collect_page(timeout_ms, settle_ms);
    if (rc) return rc;
    const hp_widget_t *w = hp_page_hit(&st.pages[st.last], (int)x, (int)y);
    if (!w || w->kind == HP_WIDGET_LABEL) {
        fprintf(stderr, "remote_sim: tap %ld,%ld hit %s: no key_event sent\n", x, y, w ? "a label" : "nothing");
        printf("{\"hit\":false}\n");
        fflush(stdout);
        return 0;
    }
    long key = 128 + w->id;
    if (send_key(remote, key, 0) || send_key(remote, key, 1)) return 1;
    printf("{\"hit\":true,\"id\":%u,\"key\":%ld}\n", (unsigned)w->id, key);
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv) {
    const char *path = NULL;
    long hold_key = -1, hold_ms = 0, hold_iv = HP_IROUT_HOLD_INTERVAL_MS;
    long key = -1, remote = 1, action = 0, timeout_ms = 5000, settle_ms = 150, tx = -1, ty = -1;
    int dump = 0, layout = 0, do_tap = 0, info = 0;
    long caps = 3;
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
        else if (!strcmp(argv[i], "--stored") && more) {
            char buf[32], *comma;
            snprintf(buf, sizeof buf, "%s", argv[++i]);
            if (!(comma = strchr(buf, ',')) || strlen(argv[i]) >= sizeof buf) return usage();
            *comma = 0;
            if (hp_parse_num(buf, 0, 255, &g_stored_gen) || hp_parse_num(comma + 1, 0, 255, &g_stored_pages)) return usage();
        }
        else if (!strcmp(argv[i], "--info") && more) { if (hp_parse_num(argv[++i], 0, 255, &caps)) return usage(); info = 1; }
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
    hp_rf_link_init(&g_link, rf, true, SIM_RF_SLOT, on_deliver, NULL);
    if (info && send_info(remote, caps)) {
        fprintf(stderr, "remote_sim: remote_info send failed\n");
        hp_rf_pair_destroy(rf);
        return 1;
    }
    rc = dump     ? dump_page(timeout_ms)
         : layout ? dump_layout(timeout_ms, settle_ms)
         : do_tap ? tap(remote, tx, ty, timeout_ms, settle_ms)
         : hold_key >= 0 ? hold(remote, hold_key, hold_ms, hold_iv)
                  : press(remote, key, action);
    hp_rf_pair_destroy(rf);
    return rc;
}
