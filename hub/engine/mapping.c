#define _POSIX_C_SOURCE 200809L
#include "hub/engine/mapping.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/statvfs.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hub/engine/atomic_file.h"
#include "hub/hp_errors.h"
#include "hub/ir/encode.h"
#include "vendor/cjson/cJSON.h"

_Static_assert(sizeof(hp_device_t) + 16 <= HP_MAP_DEV_BYTES, "HP_MAP_DEV_BYTES too small");
_Static_assert(sizeof(hp_devcmd_t) + 16 <= HP_MAP_CMD_BYTES, "HP_MAP_CMD_BYTES too small");
_Static_assert(sizeof(hp_keybind_t) <= HP_MAP_KEY_BYTES, "HP_MAP_KEY_BYTES too small");

void hp_map_init(hp_map_t *m) {
    memset(m, 0, sizeof *m);
    m->current = -1;
}

/* Allocation-failure injection for tests: the n-th allocation of the load fails. */
static unsigned g_fail_at, g_alloc_n;
void hp_map_test_fail_alloc(unsigned n) {
    g_fail_at = n;
    g_alloc_n = 0;
}
static bool alloc_fails(void) {
    if (g_fail_at && ++g_alloc_n == g_fail_at) {
        g_fail_at = 0;
        return true;
    }
    return false;
}
static void *xcalloc(size_t n, size_t sz) {
    if (n == 0 || alloc_fails()) return NULL;
    return calloc(n, sz);
}

/* Frees every table the map owns (not the cache text). */
static void release_tables(hp_map_t *m) {
    free(m->keys);
    free(m->devs);
    free(m->cmds);
    free(m->inputs);
    free(m->pool);
    free(m->idx_dev);
    free(m->idx_cmd);
    free(m->idx_in);
    for (int i = 0; i < HP_MAP_MAX_ACTIVITIES; i++) free(m->acts[i].keys);
    m->keys = NULL;
    m->devs = NULL;
    m->cmds = m->inputs = NULL;
    m->pool = NULL;
    m->idx_dev = m->idx_cmd = m->idx_in = NULL;
    for (int i = 0; i < HP_MAP_MAX_ACTIVITIES; i++) m->acts[i].keys = NULL;
    m->nkeys = 0;
    m->ndevs = m->nacts = 0;
    m->ncmds = m->ninputs = 0;
    m->pool_n = m->pool_cap = 0;
    m->mask_dev = m->mask_cmd = m->mask_in = 0;
}

void hp_map_free(hp_map_t *m) {
    release_tables(m);
    free(m->src);
    m->src = NULL;
    m->src_len = 0;
    m->est_bytes = 0;
    m->loaded = false;
}

void hp_map_set_budget(hp_map_t *m, size_t bytes) { m->budget = bytes; }
static size_t bud(const hp_map_t *m) { return m->budget ? m->budget : HP_MAP_BUDGET_BYTES; }
size_t hp_map_budget(const hp_map_t *m) { return bud(m); }

void hp_map_set_out(hp_map_t *m, hp_irout_t *out) { m->out = out; }
void hp_map_set_actops(hp_map_t *m, const hp_map_actops_t *ops) { m->actops = ops ? *ops : (hp_map_actops_t){0}; }
const char *hp_map_last_error(const hp_map_t *m) { return m->err; }

uint32_t hp_map_crc32(const uint8_t *p, size_t n) { return hp_map_crc32_update(0, p, n); }

uint32_t hp_map_crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    uint32_t c = ~crc;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
    }
    return ~c;
}

bool hp_map_hash_hex(const hp_map_t *m, char out[9]) {
    out[0] = '\0';
    if (!m->loaded) return false;
    snprintf(out, 9, "%08x", (unsigned)m->hash);
    return true;
}

/* ------------------------------------------------------------- parsing */

#define INVAL (-HP_E_INVAL)
#define TOOBIG (-HP_E_TOOBIG)

/* Records the first problem of a load in m->err and returns rc. */
static int fail(hp_map_t *m, int rc, const char *fmt, ...) {
    if (!m->err[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(m->err, sizeof m->err, fmt, ap);
        va_end(ap);
    }
    return rc;
}

static bool int_in(const cJSON *v, double lo, double hi) {
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) && v->valuedouble == floor(v->valuedouble) &&
           v->valuedouble >= lo && v->valuedouble <= hi;
}

static int parse_name(hp_map_t *m, const cJSON *v, char *dst, const char *what) {
    if (!cJSON_IsString(v)) return fail(m, INVAL, "%s must be a string", what);
    size_t n = strlen(v->valuestring);
    if (n == 0) return fail(m, INVAL, "%s must not be empty", what);
    if (n >= HP_MAP_NAME_MAX) return fail(m, TOOBIG, "%s longer than %d bytes", what, HP_MAP_NAME_MAX - 1);
    memcpy(dst, v->valuestring, n + 1);
    return 0;
}

static int oom(hp_map_t *m) { return fail(m, -HP_E_IO, "out of memory"); }

static int over_budget(hp_map_t *m, size_t need) {
    return fail(m, TOOBIG, "config needs ~%zu KiB of hub memory, the hub allows %zu KiB", (need + 1023) / 1024,
                bud(m) / 1024);
}

/* The pool is sized up front from the config (load) and only grows here for an ad-hoc action. */
static int pool_alloc(hp_map_t *m, size_t n, size_t *off) {
    if (m->pool_n + n > m->pool_cap) {
        size_t cap = m->pool_n + n;
        if (cap * HP_MAP_WORD_BYTES > bud(m)) return over_budget(m, cap * HP_MAP_WORD_BYTES);
        uint16_t *np = alloc_fails() ? NULL : realloc(m->pool, cap * sizeof *np);
        if (!np) return oom(m);
        m->pool = np;
        m->pool_cap = cap;
    }
    *off = m->pool_n;
    m->pool_n += n;
    return 0;
}

/* Runs the encoder over an IR action: a bad code is rejected here, not at key press. */
static int check_ir(hp_map_t *m, const hp_action_t *a, const char *where) {
    uint32_t raw[HP_IR_MAX_DUR];
    hp_ir_code_t c;
    hp_ir_signal_t sig;
    int r = hp_map_ir_code(m, a, raw, &c);
    if (r == 0) r = hp_ir_encode(&c, 0, &sig);
    if (r < 0) return fail(m, r, "%s: IR code rejected by the encoder", where);
    return 0;
}

/* Format 1: {"type":"ir","port_mask":n,"pronto":[ints]} */
static int parse_ir_v1(hp_map_t *m, const cJSON *j, hp_action_t *a) {
    const cJSON *pm = cJSON_GetObjectItemCaseSensitive(j, "port_mask");
    const cJSON *pr = cJSON_GetObjectItemCaseSensitive(j, "pronto");
    if (!int_in(pm, 1, 255) || !cJSON_IsArray(pr)) return fail(m, INVAL, "ir action needs port_mask 1..255 and a pronto array");
    int n = cJSON_GetArraySize(pr);
    if (n < 4) return fail(m, INVAL, "pronto needs at least 4 words");
    if (n > HP_MAP_MAX_PRONTO) return fail(m, TOOBIG, "pronto longer than %d words", HP_MAP_MAX_PRONTO);
    size_t off = 0, i = 0;
    int r = pool_alloc(m, (size_t)n, &off);
    if (r < 0) return r;
    for (const cJSON *w = pr->child; w; w = w->next, i++) {
        if (!int_in(w, 0, 65535)) return fail(m, INVAL, "pronto words must be integers 0..65535");
        m->pool[off + i] = (uint16_t)w->valuedouble;
    }
    a->type = HP_ACT_IR;
    a->port_mask = (uint8_t)pm->valuedouble;
    a->ir_kind = HP_IR_CODE_PRONTO;
    a->pool_off = (uint32_t)off;
    a->pool_n = (uint16_t)n;
    return 0;
}

/* Format 2 IR fields (shared by "type":"ir" actions and device commands/power/inputs). */
static int parse_ir_v2(hp_map_t *m, const cJSON *j, hp_action_t *a) {
    if (!cJSON_IsObject(j)) return fail(m, INVAL, "IR action must be an object");
    const cJSON *pm = cJSON_GetObjectItemCaseSensitive(j, "port_mask");
    const cJSON *pr = cJSON_GetObjectItemCaseSensitive(j, "proto");
    const cJSON *rw = cJSON_GetObjectItemCaseSensitive(j, "raw");
    const cJSON *pt = cJSON_GetObjectItemCaseSensitive(j, "pronto");
    if (pm && !int_in(pm, 1, 255)) return fail(m, INVAL, "port_mask must be 1..255");
    if ((pr != NULL) + (rw != NULL) + (pt != NULL) != 1)
        return fail(m, INVAL, "IR action needs exactly one of proto, raw, pronto");
    const cJSON *rp = cJSON_GetObjectItemCaseSensitive(j, "repeats"); /* older hubs ignore this key */
    if (rp && !int_in(rp, 1, 8)) return fail(m, INVAL, "repeats must be 1..8");
    a->type = HP_ACT_IR;
    a->port_mask = pm ? (uint8_t)pm->valuedouble : 255;
    a->repeats = rp ? (uint8_t)rp->valuedouble : 1;
    if (pr) {
        const cJSON *ad = cJSON_GetObjectItemCaseSensitive(j, "addr");
        const cJSON *cm = cJSON_GetObjectItemCaseSensitive(j, "cmd");
        hp_ir_proto_t p;
        if (!cJSON_IsString(pr) || hp_ir_proto_from_name(pr->valuestring, &p) < 0)
            return fail(m, INVAL, "unknown IR protocol");
        if (!int_in(ad, 0, 4294967295.0) || !int_in(cm, 0, 4294967295.0))
            return fail(m, INVAL, "proto action needs integer addr and cmd");
        a->ir_kind = HP_IR_CODE_PROTO;
        a->proto = (uint8_t)p;
        a->addr = (uint32_t)ad->valuedouble;
        a->cmd = (uint32_t)cm->valuedouble;
    } else if (rw) {
        const cJSON *hz = cJSON_IsObject(rw) ? cJSON_GetObjectItemCaseSensitive(rw, "hz") : NULL;
        const cJSON *us = cJSON_IsObject(rw) ? cJSON_GetObjectItemCaseSensitive(rw, "us") : NULL;
        if (!int_in(hz, 1, 4294967295.0) || !cJSON_IsArray(us)) return fail(m, INVAL, "raw needs hz and a us array");
        int n = cJSON_GetArraySize(us);
        if (n < 1) return fail(m, INVAL, "raw us must not be empty");
        if (n > HP_IR_MAX_DUR) return fail(m, TOOBIG, "raw longer than %d durations", HP_IR_MAX_DUR);
        size_t off = 0, i = 0;
        int r = pool_alloc(m, (size_t)n, &off);
        if (r < 0) return r;
        for (const cJSON *d = us->child; d; d = d->next, i++) {
            if (!int_in(d, 0, 65535)) return fail(m, INVAL, "raw durations must be integers 0..65535");
            m->pool[off + i] = (uint16_t)d->valuedouble;
        }
        a->ir_kind = HP_IR_CODE_RAW;
        a->hz = (uint32_t)hz->valuedouble;
        a->pool_off = (uint32_t)off;
        a->pool_n = (uint16_t)n;
    } else {
        uint16_t w[HP_MAP_MAX_PRONTO];
        size_t n = 0, off = 0;
        if (!cJSON_IsString(pt)) return fail(m, INVAL, "pronto must be a hex string");
        int r = hp_ir_parse_pronto(pt->valuestring, w, HP_MAP_MAX_PRONTO, &n);
        if (r == -HP_E_TOOBIG) return fail(m, TOOBIG, "pronto longer than %d words", HP_MAP_MAX_PRONTO);
        if (r < 0) return fail(m, INVAL, "pronto is not valid hex words");
        if ((r = pool_alloc(m, n, &off)) < 0) return r;
        memcpy(m->pool + off, w, n * sizeof w[0]);
        a->ir_kind = HP_IR_CODE_PRONTO;
        a->pool_off = (uint32_t)off;
        a->pool_n = (uint16_t)n;
    }
    return check_ir(m, a, "action");
}

int hp_map_parse_ir(hp_map_t *m, const cJSON *j, hp_action_t *a) {
    if (!m || !j || !a) return INVAL;
    m->format = 2;
    m->current = -1;
    m->err[0] = '\0';
    memset(a, 0, sizeof *a);
    return parse_ir_v2(m, j, a);
}

/* ---- hash indexes: entry index + 1 per slot, linear probing, power-of-two slot count >= 2n */

static uint32_t hname(uint32_t dev, const char *s) {
    uint32_t h = 2166136261u ^ (dev * 16777619u);
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h ^ (h >> 15);
}

static uint32_t *idx_new(size_t n, uint32_t *mask) {
    size_t cap = 4;
    while (cap < n * 2) cap <<= 1;
    uint32_t *t = xcalloc(cap, sizeof *t);
    *mask = t ? (uint32_t)(cap - 1) : 0;
    return t;
}

static void idx_put(uint32_t *t, uint32_t mask, uint32_t h, uint32_t entry) {
    uint32_t i = h & mask;
    while (t[i]) i = (i + 1) & mask;
    t[i] = entry + 1;
}

static int find_dev_i(const hp_map_t *m, const char *id) {
    if (!m->idx_dev) return -1;
    for (uint32_t i = hname(0, id) & m->mask_dev; m->idx_dev[i]; i = (i + 1) & m->mask_dev)
        if (!strcmp(m->devs[m->idx_dev[i] - 1].id, id)) return (int)m->idx_dev[i] - 1;
    return -1;
}

static int find_named_i(const hp_devcmd_t *tbl, const uint32_t *idx, uint32_t mask, int dev, const char *name) {
    if (!idx) return -1;
    for (uint32_t i = hname((uint32_t)dev, name) & mask; idx[i]; i = (i + 1) & mask)
        if (tbl[idx[i] - 1].dev == dev && !strcmp(tbl[idx[i] - 1].name, name)) return (int)idx[i] - 1;
    return -1;
}

int hp_map_find_cmd(const hp_map_t *m, int dev, const char *name) {
    return m && name ? find_named_i(m->cmds, m->idx_cmd, m->mask_cmd, dev, name) : -1;
}
int hp_map_find_input(const hp_map_t *m, int dev, const char *name) {
    return m && name ? find_named_i(m->inputs, m->idx_in, m->mask_in, dev, name) : -1;
}

static const hp_device_t *find_dev(const hp_map_t *m, const char *id, int *idx) {
    int i = find_dev_i(m, id);
    if (i < 0) return NULL;
    if (idx) *idx = i;
    return &m->devs[i];
}

/* {"type":"device","device":id,"command":name | "input":name} -> a copy of the target IR action. */
static int resolve_device(hp_map_t *m, const cJSON *j, hp_action_t *a) {
    const cJSON *dv = cJSON_GetObjectItemCaseSensitive(j, "device");
    const cJSON *cm = cJSON_GetObjectItemCaseSensitive(j, "command");
    const cJSON *in = cJSON_GetObjectItemCaseSensitive(j, "input");
    int di;
    if (!cJSON_IsString(dv) || (!cJSON_IsString(cm) == !cJSON_IsString(in)))
        return fail(m, INVAL, "device action needs device and exactly one of command, input");
    const hp_device_t *d = find_dev(m, dv->valuestring, &di);
    if (!d) return fail(m, INVAL, "unknown device '%.31s'", dv->valuestring);
    if (cJSON_IsString(in)) {
        int ci = hp_map_find_input(m, di, in->valuestring);
        if (ci < 0) return fail(m, INVAL, "device '%s' has no input '%.31s'", d->id, in->valuestring);
        *a = m->inputs[ci].act;
        return 0;
    }
    const char *cmd = cm->valuestring;
    if (!strcmp(cmd, "power_on") || !strcmp(cmd, "power_off")) {
        if (d->pwr == HP_PWR_NONE) return fail(m, INVAL, "device '%s' has no power codes", d->id);
        *a = (d->pwr == HP_PWR_ONOFF && !strcmp(cmd, "power_off")) ? d->pwr_off : d->pwr_on;
        return 0;
    }
    int ci = hp_map_find_cmd(m, di, cmd);
    if (ci < 0) return fail(m, INVAL, "device '%s' has no command '%.31s'", d->id, cmd);
    *a = m->cmds[ci].act;
    return 0;
}

/* allow_activity: key maps only. */
static int parse_action(hp_map_t *m, const cJSON *j, hp_action_t *a, bool allow_activity) {
    if (!cJSON_IsObject(j)) return fail(m, INVAL, "action must be an object");
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "type");
    if (!cJSON_IsString(t)) return fail(m, INVAL, "action needs a string type");
    memset(a, 0, sizeof *a);
    const char *ty = t->valuestring;
    if (!strcmp(ty, "none")) {
        a->type = HP_ACT_NONE;
    } else if (!strcmp(ty, "bt")) {
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(j, "usage");
        if (!int_in(u, 0, 65535)) return fail(m, INVAL, "bt usage must be 0..65535");
        a->type = HP_ACT_BT;
        a->usage = (uint16_t)u->valuedouble;
    } else if (!strcmp(ty, "ha")) {
        a->type = HP_ACT_HA_EVENT;
        return parse_name(m, cJSON_GetObjectItemCaseSensitive(j, "event"), a->event, "ha event");
    } else if (!strcmp(ty, "ir")) {
        if (m->format == 1) {
            int r = parse_ir_v1(m, j, a);
            return r < 0 ? r : check_ir(m, a, "action");
        }
        return parse_ir_v2(m, j, a);
    } else if (m->format == 2 && !strcmp(ty, "device")) {
        return resolve_device(m, j, a);
    } else if (m->format == 2 && !strcmp(ty, "activity")) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "activity");
        if (!allow_activity) return fail(m, INVAL, "activity actions are only allowed in key maps");
        if (!cJSON_IsString(id)) return fail(m, INVAL, "activity action needs an activity id");
        a->type = HP_ACT_ACTIVITY;
        if (!strcmp(id->valuestring, "off")) {
            a->target = HP_TARGET_OFF;
            return 0;
        }
        for (uint8_t i = 0; i < m->nacts; i++)
            if (!strcmp(m->acts[i].id, id->valuestring)) {
                a->target = i;
                return 0;
            }
        return fail(m, INVAL, "unknown activity '%.31s'", id->valuestring);
    } else {
        return fail(m, INVAL, "unknown action type '%.31s'", ty);
    }
    return 0;
}

static int parse_code(hp_map_t *m, const char *s, uint16_t *out) {
    unsigned long max = m->format == 2 ? 255 : 65535;
    size_t n = strlen(s);
    if (n == 0 || n > 5) return fail(m, INVAL, "bad key code '%.8s'", s);
    unsigned long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return fail(m, INVAL, "bad key code '%.8s'", s);
        v = v * 10 + (unsigned long)(s[i] - '0');
    }
    if (v > max) return fail(m, INVAL, "key code %lu above %lu", v, max);
    *out = (uint16_t)v;
    return 0;
}

/* Allocates the key table for exactly the entries of the object `j`. */
static int parse_keymap(hp_map_t *m, const cJSON *j, hp_keybind_t **keys, uint16_t *nkeys) {
    if (!j) return 0;
    if (!cJSON_IsObject(j)) return fail(m, INVAL, "keys must be an object");
    int cap = cJSON_GetArraySize(j);
    if (cap > HP_MAP_MAX_KEYS) return fail(m, TOOBIG, "more than %d keys in one key map", HP_MAP_MAX_KEYS);
    *nkeys = 0;
    if (cap == 0) return 0;
    hp_keybind_t *tbl = xcalloc((size_t)cap, sizeof *tbl);
    if (!tbl) return oom(m);
    *keys = tbl;
    for (const cJSON *it = j->child; it; it = it->next) {
        uint16_t code = 0;
        int r;
        if (!it->string || (r = parse_code(m, it->string, &code)) < 0) return it->string ? r : INVAL;
        for (uint16_t i = 0; i < *nkeys; i++)
            if (tbl[i].code == code) return fail(m, INVAL, "duplicate key %u", (unsigned)code);
        if ((r = parse_action(m, it, &tbl[*nkeys].act, true)) < 0) return r;
        tbl[(*nkeys)++].code = code;
    }
    return 0;
}

static int parse_steps(hp_map_t *m, const cJSON *j, hp_action_t *out, uint8_t *n) {
    if (!j) return 0;
    if (!cJSON_IsArray(j)) return fail(m, INVAL, "steps/off must be arrays");
    *n = 0;
    for (const cJSON *it = j->child; it; it = it->next) {
        if (*n >= HP_MAP_MAX_STEPS) return fail(m, TOOBIG, "more than %d steps", HP_MAP_MAX_STEPS);
        int r = parse_action(m, it, &out[*n], false);
        if (r < 0) return r;
        (*n)++;
    }
    return 0;
}

/* Adds one {"name": <ir>} entry to tbl (commands or inputs; `idx` is their hash index, both sized
 * at load for every entry of the config). */
static int add_irmap_entry(hp_map_t *m, const char *name, const cJSON *val, int dev, hp_devcmd_t *tbl, uint32_t *n,
                           uint32_t *idx, uint32_t mask, const char *what) {
    hp_devcmd_t *e = &tbl[*n];
    memset(e, 0, sizeof *e);
    e->dev = (uint8_t)dev;
    int r = parse_name(m, &(cJSON){.type = cJSON_String, .valuestring = (char *)name}, e->name, what);
    if (r < 0) return r;
    if (find_named_i(tbl, idx, mask, dev, e->name) >= 0) return fail(m, INVAL, "duplicate %s '%s'", what, e->name);
    if ((r = parse_ir_v2(m, val, &e->act)) < 0) return r;
    idx_put(idx, mask, hname((uint32_t)dev, e->name), *n);
    (*n)++;
    return 0;
}

/* The device object `d` WITHOUT its commands/inputs (those are streamed): id, name, settle_ms, power. */
static int parse_device_head(hp_map_t *m, const cJSON *d) {
    if (m->ndevs >= HP_MAP_MAX_DEVICES) return fail(m, TOOBIG, "more than %d devices", HP_MAP_MAX_DEVICES);
    hp_device_t *dev = &m->devs[m->ndevs];
    int r = parse_name(m, cJSON_GetObjectItemCaseSensitive(d, "id"), dev->id, "device id");
    if (r < 0) return r;
    if (find_dev_i(m, dev->id) >= 0) return fail(m, INVAL, "duplicate device id '%s'", dev->id);
    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(d, "name");
    if (nm) {
        if ((r = parse_name(m, nm, dev->name, "device name")) < 0) return r;
    } else {
        memcpy(dev->name, dev->id, sizeof dev->name);
    }
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(d, "settle_ms");
    if (st) {
        if (!int_in(st, 0, 60000)) return fail(m, INVAL, "settle_ms must be 0..60000");
        dev->settle_ms = (uint16_t)st->valuedouble;
    }
    const cJSON *pw = cJSON_GetObjectItemCaseSensitive(d, "power");
    if (pw) {
        const cJSON *tg = cJSON_IsObject(pw) ? cJSON_GetObjectItemCaseSensitive(pw, "toggle") : NULL;
        const cJSON *on = cJSON_IsObject(pw) ? cJSON_GetObjectItemCaseSensitive(pw, "on") : NULL;
        const cJSON *off = cJSON_IsObject(pw) ? cJSON_GetObjectItemCaseSensitive(pw, "off") : NULL;
        if (tg && !on && !off) {
            dev->pwr = HP_PWR_TOGGLE;
            if ((r = parse_ir_v2(m, tg, &dev->pwr_on)) < 0) return r;
        } else if (!tg && on && off) {
            dev->pwr = HP_PWR_ONOFF;
            if ((r = parse_ir_v2(m, on, &dev->pwr_on)) < 0) return r;
            if ((r = parse_ir_v2(m, off, &dev->pwr_off)) < 0) return r;
        } else {
            return fail(m, INVAL, "device '%s' power needs toggle, or both on and off", dev->id);
        }
    }
    idx_put(m->idx_dev, m->mask_dev, hname(0, dev->id), m->ndevs);
    m->ndevs++; /* visible to the lookups that follow */
    return 0;
}

/* Pass 1 over activities: ids and names, so key actions can reference any activity. */
static int scan_activities(hp_map_t *m, const cJSON *acts) {
    for (const cJSON *a = acts->child; a; a = a->next) {
        if (m->nacts >= HP_MAP_MAX_ACTIVITIES)
            return fail(m, TOOBIG, "more than %d activities", HP_MAP_MAX_ACTIVITIES);
        if (!cJSON_IsObject(a)) return fail(m, INVAL, "activity must be an object");
        hp_activity_t *act = &m->acts[m->nacts];
        int r;
        if (m->format == 2) {
            if ((r = parse_name(m, cJSON_GetObjectItemCaseSensitive(a, "id"), act->id, "activity id")) < 0) return r;
            if (!strcmp(act->id, "off")) return fail(m, INVAL, "activity id 'off' is reserved");
            const cJSON *nm = cJSON_GetObjectItemCaseSensitive(a, "name");
            if (nm) {
                if ((r = parse_name(m, nm, act->name, "activity name")) < 0) return r;
            } else {
                memcpy(act->name, act->id, sizeof act->name);
            }
        } else {
            if ((r = parse_name(m, cJSON_GetObjectItemCaseSensitive(a, "name"), act->name, "activity name")) < 0)
                return r;
            memcpy(act->id, act->name, sizeof act->id);
        }
        for (uint8_t i = 0; i < m->nacts; i++)
            if (!strcmp(m->acts[i].name, act->name) || !strcmp(m->acts[i].id, act->id))
                return fail(m, INVAL, "duplicate activity '%s'", act->name);
        m->nacts++;
    }
    return 0;
}

static int parse_actdevs(hp_map_t *m, const cJSON *j, hp_activity_t *act) {
    if (!j) return 0;
    if (!cJSON_IsArray(j)) return fail(m, INVAL, "activity devices must be an array");
    for (const cJSON *e = j->child; e; e = e->next) {
        if (act->ndevs >= HP_MAP_MAX_ACT_DEVS)
            return fail(m, TOOBIG, "more than %d devices in one activity", HP_MAP_MAX_ACT_DEVS);
        const cJSON *dv = cJSON_IsObject(e) ? cJSON_GetObjectItemCaseSensitive(e, "device") : NULL;
        const cJSON *in = cJSON_IsObject(e) ? cJSON_GetObjectItemCaseSensitive(e, "input") : NULL;
        int di;
        if (!cJSON_IsString(dv) || (in && !cJSON_IsString(in))) return fail(m, INVAL, "activity device needs a device id");
        if (!find_dev(m, dv->valuestring, &di)) return fail(m, INVAL, "unknown device '%.31s'", dv->valuestring);
        hp_actdev_t *ad = &act->devs[act->ndevs];
        ad->dev = (uint8_t)di;
        ad->input = -1;
        if (in) {
            int ii = hp_map_find_input(m, di, in->valuestring);
            if (ii < 0) return fail(m, INVAL, "device '%s' has no input '%.31s'", m->devs[di].id, in->valuestring);
            ad->input = ii;
        }
        act->ndevs++;
    }
    return 0;
}

static int parse_config(hp_map_t *m, const cJSON *root) {
    if (!cJSON_IsObject(root)) return fail(m, INVAL, "config must be an object");
    const cJSON *fmt = cJSON_GetObjectItemCaseSensitive(root, "format");
    m->format = 1;
    if (fmt) {
        if (!int_in(fmt, 1, 2)) return fail(m, INVAL, "unsupported config format");
        m->format = (int)fmt->valuedouble;
    }
    int r;
    const cJSON *acts = cJSON_GetObjectItemCaseSensitive(root, "activities");
    if (acts && !cJSON_IsArray(acts)) return fail(m, INVAL, "activities must be an array");
    if (acts && (r = scan_activities(m, acts)) < 0) return r;
    if ((r = parse_keymap(m, cJSON_GetObjectItemCaseSensitive(root, "keys"), &m->keys, &m->nkeys)) < 0) return r;
    if (!acts) return 0;
    uint8_t i = 0;
    for (const cJSON *a = acts->child; a; a = a->next, i++) {
        hp_activity_t *act = &m->acts[i];
        if ((r = parse_keymap(m, cJSON_GetObjectItemCaseSensitive(a, "keys"), &act->keys, &act->nkeys)) < 0) return r;
        if ((r = parse_steps(m, cJSON_GetObjectItemCaseSensitive(a, "steps"), act->steps, &act->nsteps)) < 0) return r;
        if ((r = parse_steps(m, cJSON_GetObjectItemCaseSensitive(a, "off"), act->off, &act->noff)) < 0) return r;
        if (m->format == 2 && (r = parse_actdevs(m, cJSON_GetObjectItemCaseSensitive(a, "devices"), act)) < 0)
            return r;
    }
    return 0;
}

static size_t pronto_tokens(const char *s) {
    size_t n = 0;
    bool in = false;
    for (; *s; s++) {
        bool sp = *s == ' ' || *s == '\t' || *s == '\n' || *s == '\r';
        if (!sp && !in) n++;
        in = !sp;
    }
    return n;
}

/* Upper bound of the pool words of every IR action under `j` (raw us arrays, pronto strings/arrays). */
static size_t pool_words(const cJSON *j) {
    size_t n = 0;
    for (const cJSON *c = j->child; c; c = c->next) {
        if (cJSON_IsObject(c) || cJSON_IsArray(c)) n += pool_words(c);
    }
    if (cJSON_IsObject(j)) {
        const cJSON *rw = cJSON_GetObjectItemCaseSensitive(j, "raw");
        const cJSON *pt = cJSON_GetObjectItemCaseSensitive(j, "pronto");
        if (cJSON_IsObject(rw)) {
            const cJSON *us = cJSON_GetObjectItemCaseSensitive(rw, "us");
            if (cJSON_IsArray(us)) n += (size_t)cJSON_GetArraySize(us);
        }
        if (cJSON_IsString(pt)) n += pronto_tokens(pt->valuestring);
        else if (cJSON_IsArray(pt)) n += (size_t)cJSON_GetArraySize(pt);
    }
    return n;
}

/* ---- streaming load. The config text is never held whole in RAM and never parsed into one tree:
 * a reader walks it (a FILE: the cache file being received, or fmemopen over a caller's text),
 * parses ONE small value at a time with cJSON (a command, a device's header, `keys`, one
 * activity) and frees it. Peak memory is the final tables plus the one value in hand.
 * Pass 1 counts what the tables need (and checks the byte budget); pass 2 fills them. */

typedef struct {
    size_t ndevs, ncmds, ninputs, keys, pool;
} dims_t;

#define VAL_CMD_MAX (64u * 1024)      /* one command / device header */
#define VAL_SECTION_MAX (128u * 1024) /* `keys`, one activity, `format`: parsed as a tree, sized in the slack */
#define GETC(f) getc_unlocked(f)

typedef struct {
    hp_map_t *w;   /* the map being built (errors go here) */
    FILE *f;
    int pass;      /* 1 count, 2 fill */
    bool fmt2;
    dims_t d;
    cJSON *rest;   /* format, activities, keys (small): collected in pass 1 */
    cJSON *dev;    /* the device header being read */
    long dev_off[2], dev_end; /* [0] inputs, [1] commands: offset of the `{`, -1 if absent; end of the device */
    char *vb;      /* the one value in hand */
    size_t vn, vcap, vmax;
} sctx_t;

static int vb_put(sctx_t *s, int c) {
    if (s->vn + 1 >= s->vmax) return -1;
    if (s->vn + 1 >= s->vcap) {
        size_t nc = s->vcap ? s->vcap * 2 : 1024;
        char *np = alloc_fails() ? NULL : realloc(s->vb, nc);
        if (!np) return -2;
        s->vb = np;
        s->vcap = nc;
    }
    s->vb[s->vn++] = (char)c;
    return 0;
}

static int rd_ws(FILE *f) {
    int c;
    while ((c = GETC(f)) == ' ' || c == '\t' || c == '\n' || c == '\r') {}
    return c;
}

/* Reads the JSON value whose first character `c` was just consumed. Keeps its text in s->vb when
 * `keep`, else skips it. */
static int rd_value(sctx_t *s, int c, bool keep, size_t vmax) {
    FILE *f = s->f;
    int depth = 0, r;
    bool in_str = false;
    s->vn = 0;
    s->vmax = vmax;
#define PUT(ch) do { if (keep && (r = vb_put(s, (ch))) < 0) return r == -1 ? TOOBIG : oom(s->w); } while (0)
    for (;; c = GETC(f)) {
        if (c == EOF) return fail(s->w, INVAL, "config ends inside a value");
        PUT(c);
        if (in_str) {
            if (c == '\\') {
                if ((c = GETC(f)) == EOF) return fail(s->w, INVAL, "config ends inside a value");
                PUT(c);
            } else if (c == '"') {
                in_str = false;
                if (depth == 0) break;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '{' || c == '[') {
            depth++;
        } else if (c == '}' || c == ']') {
            if (--depth <= 0) break;
        } else if (depth == 0) { /* number / true / false / null */
            for (;;) {
                int n = GETC(f);
                if (n == EOF) break;
                if (n == ',' || n == '}' || n == ']' || n == ' ' || n == '\n' || n == '\t' || n == '\r') {
                    ungetc(n, f);
                    break;
                }
                PUT(n);
            }
            break;
        }
    }
#undef PUT
    if (keep) s->vb[s->vn] = '\0';
    return 0;
}

static int too_big_value(sctx_t *s, int r, const char *what) {
    return r == TOOBIG ? fail(s->w, TOOBIG, "%s larger than %u KiB", what, (unsigned)(s->vmax / 1024)) : r;
}

/* The value in hand as a cJSON tree (caller frees); NULL with the error recorded. */
static cJSON *vb_tree(sctx_t *s) {
    cJSON *t = cJSON_ParseWithLength(s->vb, s->vn);
    if (!t) fail(s->w, INVAL, "config is not valid JSON");
    return t;
}

/* A member key (after its opening quote was consumed as `c`) into key[cap], decoded. */
static int rd_key(sctx_t *s, int c, char *key, size_t cap) {
    int r = rd_value(s, c, true, 4096);
    if (r < 0) return too_big_value(s, r, "a key");
    if (!memchr(s->vb, '\\', s->vn)) { /* the common case: no escapes */
        size_t n = s->vn >= 2 ? s->vn - 2 : 0;
        if (n >= cap) n = cap - 1;
        memcpy(key, s->vb + 1, n);
        key[n] = '\0';
        return 0;
    }
    cJSON *t = vb_tree(s);
    if (!t) return INVAL;
    snprintf(key, cap, "%s", cJSON_IsString(t) ? t->valuestring : "");
    cJSON_Delete(t);
    return 0;
}

typedef int (*member_fn)(sctx_t *s, const char *key, int c);

/* Walks the object whose `{` was consumed; cb consumes each member's value (first char `c`). */
static int rd_members(sctx_t *s, member_fn cb) {
    char key[256];
    int c = rd_ws(s->f);
    if (c == '}') return 0;
    for (;;) {
        if (c != '"') return fail(s->w, INVAL, "config is not valid JSON");
        int r = rd_key(s, c, key, sizeof key);
        if (r < 0) return r;
        if (rd_ws(s->f) != ':') return fail(s->w, INVAL, "config is not valid JSON");
        c = rd_ws(s->f);
        if (c == EOF) return fail(s->w, INVAL, "config ends inside a value");
        if ((r = cb(s, key, c)) < 0) return r;
        c = rd_ws(s->f);
        if (c == ',') {
            c = rd_ws(s->f);
            continue;
        }
        if (c == '}') return 0;
        return fail(s->w, INVAL, "config is not valid JSON");
    }
}

/* ---- one device's commands / inputs, streamed member by member */

static int cur_dev, cur_which; /* set around rd_members of a bulk object (single-threaded load) */

static int bulk_member(sctx_t *s, const char *key, int c) {
    hp_map_t *m = s->w;
    int r = rd_value(s, c, true, VAL_CMD_MAX);
    if (r < 0) return too_big_value(s, r, "a command");
    cJSON *val = vb_tree(s);
    if (!val) return INVAL;
    if (s->pass == 1) {
        if (cur_which == 0) s->d.ninputs++;
        else s->d.ncmds++;
        s->d.pool += pool_words(val);
        cJSON_Delete(val);
        return 0;
    }
    if (cur_which == 0) r = add_irmap_entry(m, key, val, cur_dev, m->inputs, &m->ninputs, m->idx_in, m->mask_in, "inputs");
    else r = add_irmap_entry(m, key, val, cur_dev, m->cmds, &m->ncmds, m->idx_cmd, m->mask_cmd, "commands");
    cJSON_Delete(val);
    return r;
}

static int do_bulk(sctx_t *s, int which, int dev) {
    if (s->dev_off[which] < 0) return 0;
    if (fseeko(s->f, s->dev_off[which], SEEK_SET) < 0) return fail(s->w, -HP_E_IO, "cannot read the config");
    int c = GETC(s->f); /* the `{` recorded before */
    if (c != '{') return fail(s->w, INVAL, "%s must be an object", which ? "commands" : "inputs");
    cur_which = which;
    cur_dev = dev;
    return rd_members(s, bulk_member);
}

static int dev_member(sctx_t *s, const char *key, int c) {
    int which = !strcmp(key, "inputs") ? 0 : !strcmp(key, "commands") ? 1 : -1;
    if (which >= 0) {
        if (c != '{') return fail(s->w, INVAL, "%s must be an object", key);
        s->dev_off[which] = (long)ftello(s->f) - 1;
        return rd_value(s, c, false, 0);
    }
    int r = rd_value(s, c, true, VAL_CMD_MAX);
    if (r < 0) return too_big_value(s, r, "a device field");
    cJSON *v = vb_tree(s);
    if (!v) return INVAL;
    if (!s->dev || !cJSON_AddItemToObject(s->dev, key, v)) {
        cJSON_Delete(v);
        return oom(s->w);
    }
    return 0;
}

static int do_device(sctx_t *s) { /* the `{` was consumed */
    s->dev_off[0] = s->dev_off[1] = -1;
    s->dev = cJSON_CreateObject();
    if (!s->dev) return oom(s->w);
    int r = rd_members(s, dev_member);
    if (r == 0) {
        long end = (long)ftello(s->f);
        int dev = 0;
        if (s->pass == 1) {
            s->d.ndevs++;
        } else if (s->fmt2) {
            r = parse_device_head(s->w, s->dev);
            dev = s->w->ndevs - 1;
        }
        if (r == 0 && (s->pass == 1 || s->fmt2)) {
            if ((r = do_bulk(s, 0, dev)) == 0) r = do_bulk(s, 1, dev);
        }
        if (r == 0 && fseeko(s->f, end, SEEK_SET) < 0) r = fail(s->w, -HP_E_IO, "cannot read the config");
    }
    cJSON_Delete(s->dev);
    s->dev = NULL;
    return r;
}

static int top_member(sctx_t *s, const char *key, int c) {
    if (!strcmp(key, "devices")) {
        if (c != '[') return fail(s->w, INVAL, "devices must be an array");
        c = rd_ws(s->f);
        if (c == ']') return 0;
        for (;;) {
            if (c != '{') return fail(s->w, INVAL, "device must be an object");
            int r = do_device(s);
            if (r < 0) return r;
            c = rd_ws(s->f);
            if (c == ',') {
                c = rd_ws(s->f);
                continue;
            }
            if (c == ']') return 0;
            return fail(s->w, INVAL, "config is not valid JSON");
        }
    }
    bool want = s->pass == 1 && (!strcmp(key, "format") || !strcmp(key, "activities") || !strcmp(key, "keys"));
    int r = rd_value(s, c, want, VAL_SECTION_MAX);
    if (r < 0) return too_big_value(s, r, key);
    if (!want) return 0;
    cJSON *v = vb_tree(s);
    if (!v) return INVAL;
    if (!cJSON_AddItemToObject(s->rest, key, v)) {
        cJSON_Delete(v);
        return oom(s->w);
    }
    return 0;
}

static int stream_pass(sctx_t *s, size_t len, off_t base) {
    if (fseeko(s->f, base, SEEK_SET) < 0) return fail(s->w, -HP_E_IO, "cannot read the config");
    int c = rd_ws(s->f);
    if (c != '{') return fail(s->w, INVAL, "config must be an object");
    int r = rd_members(s, top_member);
    if (r < 0) return r;
    while ((size_t)(ftello(s->f) - base) < len) { /* nothing but whitespace after the object */
        c = GETC(s->f);
        if (c == EOF) break;
        if (c != ' ' && c != '\n' && c != '\t' && c != '\r') return fail(s->w, INVAL, "config is not valid JSON");
    }
    return 0;
}

/* ---- sizing */

static void count_rest(const cJSON *rest, dims_t *d) {
    const cJSON *keys = cJSON_GetObjectItemCaseSensitive(rest, "keys");
    if (cJSON_IsObject(keys)) d->keys += (size_t)cJSON_GetArraySize(keys);
    const cJSON *acts = cJSON_GetObjectItemCaseSensitive(rest, "activities");
    if (cJSON_IsArray(acts)) {
        for (const cJSON *a = acts->child; a; a = a->next) {
            const cJSON *k = cJSON_IsObject(a) ? cJSON_GetObjectItemCaseSensitive(a, "keys") : NULL;
            if (cJSON_IsObject(k)) d->keys += (size_t)cJSON_GetArraySize(k);
        }
    }
    d->pool += pool_words(rest);
}

static size_t est_full(const dims_t *d) {
    return HP_MAP_SLACK_BYTES + d->ndevs * HP_MAP_DEV_BYTES + (d->ncmds + d->ninputs) * HP_MAP_CMD_BYTES +
           d->keys * HP_MAP_KEY_BYTES + d->pool * HP_MAP_WORD_BYTES;
}

/* One allocation per table, each exactly as large as this config needs. */
static int alloc_tables(hp_map_t *m, const dims_t *d) {
    if (d->ndevs > HP_MAP_MAX_DEVICES) return fail(m, TOOBIG, "more than %d devices", HP_MAP_MAX_DEVICES);
    if (d->ndevs && !(m->devs = xcalloc(d->ndevs, sizeof *m->devs))) return oom(m);
    if (d->ndevs && !(m->idx_dev = idx_new(d->ndevs, &m->mask_dev))) return oom(m);
    if (d->ncmds && !(m->cmds = xcalloc(d->ncmds, sizeof *m->cmds))) return oom(m);
    if (d->ncmds && !(m->idx_cmd = idx_new(d->ncmds, &m->mask_cmd))) return oom(m);
    if (d->ninputs && !(m->inputs = xcalloc(d->ninputs, sizeof *m->inputs))) return oom(m);
    if (d->ninputs && !(m->idx_in = idx_new(d->ninputs, &m->mask_in))) return oom(m);
    if (d->pool) {
        if (alloc_fails() || !(m->pool = malloc(d->pool * sizeof *m->pool))) return oom(m);
        m->pool_cap = d->pool;
    }
    return 0;
}

static uint32_t crc_of_file(FILE *f, size_t len, off_t base) {
    uint8_t buf[4096];
    uint32_t crc = 0;
    if (fseeko(f, base, SEEK_SET) < 0) return 0;
    while (len) {
        size_t n = fread(buf, 1, len < sizeof buf ? len : sizeof buf, f);
        if (n == 0) break;
        crc = hp_map_crc32_update(crc, buf, n);
        len -= n;
    }
    return crc;
}

/* est_only: just report the estimate (no tables, no pass 2). */
static int prepare_impl(hp_map_t *m, FILE *f, size_t len, hp_map_t **out, size_t *est_out) {
    m->err[0] = '\0';
    off_t base = ftello(f);
    hp_map_t *w = calloc(1, sizeof *w);
    sctx_t s = {.w = w, .f = f};
    int r = -HP_E_IO;
    if (!w) {
        fail(m, r, "out of memory");
        return r;
    }
    w->current = -1;
    w->budget = m->budget;
    s.rest = cJSON_CreateObject();
    if (!s.rest) {
        r = oom(w);
        goto out;
    }
    s.pass = 1;
    if ((r = stream_pass(&s, len, base)) < 0) goto out;
    count_rest(s.rest, &s.d);
    {
        const cJSON *fmt = cJSON_GetObjectItemCaseSensitive(s.rest, "format");
        w->format = 1;
        if (fmt) {
            if (!int_in(fmt, 1, 2)) {
                r = fail(w, INVAL, "unsupported config format");
                goto out;
            }
            w->format = (int)fmt->valuedouble;
        }
        s.fmt2 = w->format == 2;
    }
    size_t est = est_full(&s.d);
    if (est_out) *est_out = est;
    if (est > bud(m)) {
        r = over_budget(w, est);
        goto out;
    }
    if (!out) {
        r = 0;
        goto out;
    }
    if ((r = alloc_tables(w, &s.d)) < 0) goto out;
    s.pass = 2;
    if ((r = stream_pass(&s, len, base)) < 0) goto out;
    if ((r = parse_config(w, s.rest)) < 0) goto out;
    if (w->pool_n < w->pool_cap) { /* the count is an upper bound: give the slack back */
        uint16_t *np = w->pool_n ? realloc(w->pool, w->pool_n * sizeof *np) : NULL;
        if (np || !w->pool_n) {
            if (!w->pool_n) free(w->pool);
            w->pool = np;
            w->pool_cap = w->pool_n;
        }
    }
    w->est_bytes = est;
    w->src_len = len;
    w->hash = crc_of_file(f, len, base);
    r = 0;
out:
    free(s.vb);
    cJSON_Delete(s.rest);
    if (r < 0 || !out) {
        memcpy(m->err, w->err, sizeof m->err);
        release_tables(w);
        free(w);
    } else {
        *out = w;
    }
    return r;
}

int hp_map_prepare(hp_map_t *m, FILE *f, size_t len, hp_map_t **out) {
    if (!m || !f || !out || len == 0) return INVAL;
    return prepare_impl(m, f, len, out, NULL);
}

void hp_map_discard(hp_map_t *scratch) {
    if (!scratch) return;
    release_tables(scratch);
    free(scratch);
}

/* Swaps the prepared map in. `src` (owned, may be NULL) is the config text kept for hp_map_save_cache. */
void hp_map_commit(hp_map_t *m, hp_map_t *scratch, char *src) {
    /* keep the running activity if it survives the reload: format 2 identity is the activity id
     * (a rename keeps it), format 1 has only names */
    if (m->current >= 0 && (size_t)m->current < m->nacts) {
        bool by_id = m->format == 2 && scratch->format == 2;
        const hp_activity_t *cur = &m->acts[m->current];
        for (uint8_t i = 0; i < scratch->nacts; i++)
            if (!strcmp(by_id ? scratch->acts[i].id : scratch->acts[i].name, by_id ? cur->id : cur->name))
                scratch->current = i;
    }
    hp_irout_t *outrt = m->out;
    hp_map_actops_t ops = m->actops;
    size_t budget = m->budget;
    hp_map_free(m);
    *m = *scratch;
    m->out = outrt;
    m->actops = ops;
    m->budget = budget;
    m->src = src;
    m->loaded = true;
    free(scratch);
}

int hp_map_estimate(const char *json, size_t len, size_t *est) {
    if (!json || !est || !len) return INVAL;
    FILE *f = fmemopen((void *)(uintptr_t)json, len, "r");
    if (!f) return -HP_E_IO;
    hp_map_t tmp;
    hp_map_init(&tmp);
    tmp.budget = SIZE_MAX; /* just the number */
    int r = prepare_impl(&tmp, f, len, NULL, est);
    fclose(f);
    return r;
}

int hp_map_load(hp_map_t *m, const char *json, size_t len) {
    if (!m || !json) return INVAL;
    m->err[0] = '\0';
    if (len == 0) return fail(m, INVAL, "config is not valid JSON");
    FILE *f = fmemopen((void *)(uintptr_t)json, len, "r");
    char *src = alloc_fails() ? NULL : malloc(len + 1);
    if (!f || !src) {
        if (f) fclose(f);
        free(src);
        return fail(m, -HP_E_IO, "out of memory");
    }
    hp_map_t *scratch = NULL;
    int r = prepare_impl(m, f, len, &scratch, NULL);
    fclose(f);
    if (r < 0) {
        free(src);
        return r;
    }
    memcpy(src, json, len);
    src[len] = '\0';
    hp_map_commit(m, scratch, src);
    m->src_len = len;
    return 0;
}

/* ------------------------------------------------------- lookup / apply */

static const hp_action_t NONE_ACTION = {.type = HP_ACT_NONE};

static const hp_keybind_t *find_key(const hp_keybind_t *keys, uint16_t n, uint16_t code) {
    for (uint16_t i = 0; i < n; i++)
        if (keys[i].code == code) return &keys[i];
    return NULL;
}

int hp_map_lookup(const hp_map_t *m, const char *activity, uint16_t key_code, hp_action_t *out) {
    if (!m || !out) return -HP_E_INVAL;
    const hp_keybind_t *kb = NULL;
    if (activity) {
        const hp_activity_t *a = NULL;
        for (uint8_t i = 0; i < m->nacts; i++)
            if (!strcmp(m->acts[i].name, activity)) a = &m->acts[i];
        if (!a) return -HP_E_INVAL;
        kb = find_key(a->keys, a->nkeys, key_code);
    }
    if (!kb) kb = find_key(m->keys, m->nkeys, key_code);
    *out = kb ? kb->act : NONE_ACTION;
    return kb ? 1 : 0;
}

int hp_map_ir_code(const hp_map_t *m, const hp_action_t *a, uint32_t *raw, hp_ir_code_t *c) {
    if (!m || !a || !c || a->type != HP_ACT_IR) return -HP_E_INVAL;
    memset(c, 0, sizeof *c);
    c->kind = (hp_ir_kind_t)a->ir_kind;
    switch (c->kind) {
    case HP_IR_CODE_PROTO:
        c->u.p.proto = (hp_ir_proto_t)a->proto;
        c->u.p.addr = a->addr;
        c->u.p.cmd = a->cmd;
        return 0;
    case HP_IR_CODE_RAW:
        if (!raw || a->pool_n > HP_IR_MAX_DUR || (size_t)a->pool_off + a->pool_n > m->pool_n)
            return -HP_E_INVAL;
        for (size_t i = 0; i < a->pool_n; i++) raw[i] = m->pool[a->pool_off + i];
        c->u.raw.hz = a->hz;
        c->u.raw.us = raw;
        c->u.raw.n = a->pool_n;
        return 0;
    case HP_IR_CODE_PRONTO:
        if ((size_t)a->pool_off + a->pool_n > m->pool_n) return -HP_E_INVAL;
        c->u.pronto.words = m->pool + a->pool_off;
        c->u.pronto.n = a->pool_n;
        return 0;
    }
    return -HP_E_INVAL;
}

/* key >= 0: this is a physical key press, a hold candidate. */
static int apply_action(const hp_map_t *m, const hp_action_t *a, const hp_sinks_t *s, int key) {
    if (!m || !a || !s) return -HP_E_INVAL;
    switch (a->type) {
    case HP_ACT_IR: {
        uint32_t raw[HP_IR_MAX_DUR];
        hp_ir_code_t c;
        if (!s->ir) return -HP_E_INVAL;
        int r = hp_map_ir_code(m, a, raw, &c);
        if (r < 0) return r;
        if (m->out) return hp_irout_send_n(m->out, s->ir, &c, a->port_mask, key, a->repeats ? a->repeats : 1);
        hp_ir_signal_t sig;
        if ((r = hp_ir_encode(&c, 0, &sig)) < 0) return r;
        return s->ir->send(s->ir->self, a->port_mask, &sig);
    }
    case HP_ACT_BT: {
        if (!s->bt) return -HP_E_INVAL;
        int r = s->bt->key(s->bt->self, a->usage, true);
        return r < 0 ? r : s->bt->key(s->bt->self, a->usage, false);
    }
    case HP_ACT_HA_EVENT:
    case HP_ACT_NONE:
        return 0;
    case HP_ACT_ACTIVITY:
        break;
    }
    return -HP_E_INVAL;
}

int hp_map_apply(const hp_map_t *m, const hp_action_t *a, const hp_sinks_t *s) {
    return apply_action(m, a, s, -1);
}

int hp_map_apply_deferrable(const hp_map_t *m, const hp_action_t *a, const hp_sinks_t *s) {
    return apply_action(m, a, s, HP_IROUT_KEY_SEND);
}

size_t hp_map_activity_count(const hp_map_t *m) { return m->nacts; }
const char *hp_map_activity_name(const hp_map_t *m, size_t i) { return i < m->nacts ? m->acts[i].name : NULL; }
const char *hp_map_activity_id(const hp_map_t *m, size_t i) { return i < m->nacts ? m->acts[i].id : NULL; }
int hp_map_current(const hp_map_t *m) { return m->current; }

static int run_steps(const hp_map_t *m, const hp_action_t *steps, uint8_t n, const hp_sinks_t *s) {
    for (uint8_t i = 0; i < n; i++) {
        int r = hp_map_apply(m, &steps[i], s);
        if (r < 0) return r;
    }
    return 0;
}

int hp_activity_set(hp_map_t *m, const char *name, const hp_sinks_t *s) {
    if (!m || !name || !s) return -HP_E_INVAL;
    for (uint8_t i = 0; i < m->nacts; i++) {
        if (strcmp(m->acts[i].name, name)) continue;
        int r = run_steps(m, m->acts[i].steps, m->acts[i].nsteps, s);
        if (r < 0) return r;
        m->current = i;
        return i;
    }
    return -HP_E_INVAL;
}

int hp_activity_off(hp_map_t *m, const hp_sinks_t *s) {
    if (!m || !s || m->current < 0 || m->current >= m->nacts) return -HP_E_INVAL;
    int idx = m->current;
    int r = run_steps(m, m->acts[idx].off, m->acts[idx].noff, s);
    if (r < 0) return r;
    m->current = -1;
    return idx;
}

static int apply_key_action(hp_map_t *m, const hp_action_t *a, const hp_sinks_t *s, uint8_t key) {
    if (a->type != HP_ACT_ACTIVITY) return apply_action(m, a, s, key);
    int r;
    if (m->actops.start) {
        if (a->target == HP_TARGET_OFF) {
            r = m->actops.off(m->actops.ctx);
            if (r == -HP_E_INVAL) r = 0; /* nothing on: fine */
        } else if (a->target < m->nacts) {
            r = m->actops.start(m->actops.ctx, a->target);
        } else {
            r = -HP_E_INVAL;
        }
        return r < 0 ? r : 0;
    }
    if (a->target == HP_TARGET_OFF) r = m->current < 0 ? 0 : hp_activity_off(m, s);
    else if (a->target < m->nacts) r = hp_activity_set(m, m->acts[a->target].name, s);
    else r = -HP_E_INVAL;
    return r < 0 ? r : 0;
}

int hp_key_handle(hp_map_t *m, const hp_sinks_t *s, uint8_t remote, uint8_t key, hp_key_action_t action,
                  hp_key_emit_fn emit, void *ctx) {
    if (!m || !s) return -HP_E_INVAL;
    int rc = 0;
    if (action == HP_KEY_ACTION_PRESS) {
        hp_irout_cancel(m->out); /* a new press ends any hold */
        const char *cur = m->current >= 0 && m->current < m->nacts ? m->acts[m->current].name : NULL;
        hp_action_t a;
        if (hp_map_lookup(m, cur, key, &a) == 1) {
            if (a.type != HP_ACT_ACTIVITY && m->actops.busy && m->actops.busy(m->actops.ctx))
                rc = 0; /* mid-transition: the key would overlap the planned frames */
            else rc = apply_key_action(m, &a, s, key);
        }
    } else if (action == HP_KEY_ACTION_HOLD) {
        hp_irout_hold(m->out, key);
    } else if (action == HP_KEY_ACTION_RELEASE) {
        hp_irout_release(m->out, key);
    }
    if (emit) emit(ctx, remote, key, action);
    return rc;
}

/* --------------------------------------------------------------- cache */

#define CACHE_HDR 9 /* magic(4) ver(1) len(4) */
#define CACHE_VER 1

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int hp_map_save_cache(const hp_map_t *m, const char *path) {
    if (!m || !path || !m->src) return -HP_E_INVAL;
    size_t total = CACHE_HDR + m->src_len + 4;
    uint8_t *buf = malloc(total);
    if (!buf) return -HP_E_IO;
    memcpy(buf, "HPMC", 4);
    buf[4] = CACHE_VER;
    put32(buf + 5, (uint32_t)m->src_len);
    memcpy(buf + CACHE_HDR, m->src, m->src_len);
    put32(buf + CACHE_HDR + m->src_len, hp_map_crc32(buf, CACHE_HDR + m->src_len));
    int rc = hp_atomic_write(path, buf, total);
    free(buf);
    return rc;
}

/* Loads the cache file by streaming it: the text is never held in RAM. */
int hp_map_load_cache(hp_map_t *m, const char *path) {
    if (!m || !path) return -HP_E_INVAL;
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? -HP_E_NOENT : -HP_E_IO;
    uint8_t hdr[CACHE_HDR];
    long size = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1;
    int rc = -HP_E_INVAL;
    if (size >= CACHE_HDR + 4 && fseek(f, 0, SEEK_SET) == 0 && fread(hdr, 1, sizeof hdr, f) == sizeof hdr &&
        !memcmp(hdr, "HPMC", 4) && hdr[4] == CACHE_VER) {
        size_t len = get32(hdr + 5);
        if (len == (size_t)size - CACHE_HDR - 4) {
            uint8_t tail[4];
            uint32_t crc = crc_of_file(f, len + CACHE_HDR, 0);
            if (fseek(f, (long)(CACHE_HDR + len), SEEK_SET) == 0 && fread(tail, 1, 4, f) == 4 && get32(tail) == crc) {
                hp_map_t *scratch = NULL;
                fseek(f, CACHE_HDR, SEEK_SET);
                rc = len ? prepare_impl(m, f, len, &scratch, NULL) : -HP_E_INVAL;
                if (rc == 0) hp_map_commit(m, scratch, NULL);
            }
        }
    }
    fclose(f);
    return rc;
}

/* ---- the cache file as it is received: header, text appended chunk by chunk, crc trailer. Built beside the
 * live cache and renamed over it only after the config it holds has loaded. */

struct hp_cachew {
    FILE *f;
    char *tmp, *path; /* tmp NULL: anonymous (no cache path) */
    size_t total, got;
    uint32_t crc;
};

size_t hp_cache_free_bytes(const char *path) {
    if (!path) return SIZE_MAX;
    char *dir = strdup(path);
    if (!dir) return SIZE_MAX;
    char *sl = strrchr(dir, '/');
    if (sl) *sl = '\0';
    else strcpy(dir, ".");
    struct statvfs sv;
    size_t r = SIZE_MAX;
    if (statvfs(dir, &sv) == 0 && sv.f_bavail != (fsblkcnt_t)-1) r = (size_t)sv.f_bavail * (size_t)sv.f_frsize;
    free(dir);
    return r;
}

int hp_cachew_open(hp_cachew_t **out, const char *path, size_t total) {
    hp_cachew_t *w = calloc(1, sizeof *w);
    if (!w) return -HP_E_IO;
    w->total = total;
    if (path) {
        size_t need = total + CACHE_HDR + 4 + 4096; /* jffs2 compresses, but never count on it */
        if (hp_cache_free_bytes(path) < need) {
            free(w);
            return -HP_E_TOOBIG;
        }
        w->path = strdup(path);
        w->tmp = malloc(strlen(path) + 8);
        if (!w->path || !w->tmp) goto fail;
        sprintf(w->tmp, "%s.new", path);
        w->f = fopen(w->tmp, "w+b");
    } else {
        w->f = tmpfile();
    }
    if (!w->f) goto fail;
    uint8_t hdr[CACHE_HDR];
    memcpy(hdr, "HPMC", 4);
    hdr[4] = CACHE_VER;
    put32(hdr + 5, (uint32_t)total);
    w->crc = hp_map_crc32_update(0, hdr, sizeof hdr);
    if (fwrite(hdr, 1, sizeof hdr, w->f) != sizeof hdr) goto fail;
    *out = w;
    return 0;
fail:
    hp_cachew_abort(w);
    return -HP_E_IO;
}

int hp_cachew_append(hp_cachew_t *w, const void *p, size_t n) {
    if (w->got + n > w->total) return -HP_E_INVAL;
    if (fwrite(p, 1, n, w->f) != n) return -HP_E_IO; /* ENOSPC shows here or at the flush */
    w->crc = hp_map_crc32_update(w->crc, p, n);
    w->got += n;
    return 0;
}

int hp_cachew_finish(hp_cachew_t *w, FILE **text) {
    if (w->got != w->total) return -HP_E_INVAL;
    uint8_t tail[4];
    put32(tail, w->crc);
    if (fwrite(tail, 1, 4, w->f) != 4 || fflush(w->f) != 0 || (w->tmp && fsync(fileno(w->f)) != 0)) return -HP_E_IO;
    if (fseek(w->f, CACHE_HDR, SEEK_SET) != 0) return -HP_E_IO;
    *text = w->f;
    return 0;
}

int hp_cachew_publish(hp_cachew_t *w) {
    int rc = 0;
    if (w->tmp) {
        fclose(w->f);
        w->f = NULL;
        if (rename(w->tmp, w->path) != 0) {
            unlink(w->tmp);
            rc = -HP_E_IO;
        } else {
            char *dup = strdup(w->path); /* best effort: persist the rename */
            int dfd = dup ? open(dirname(dup), O_RDONLY) : -1;
            if (dfd >= 0) {
                (void)fsync(dfd);
                close(dfd);
            }
            free(dup);
        }
    }
    hp_cachew_abort(w);
    return rc;
}

void hp_cachew_abort(hp_cachew_t *w) {
    if (!w) return;
    if (w->f) fclose(w->f);
    if (w->tmp && w->f) unlink(w->tmp);
    free(w->tmp);
    free(w->path);
    free(w);
}
