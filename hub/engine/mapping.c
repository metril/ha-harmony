#define _POSIX_C_SOURCE 200809L
#include "hub/engine/mapping.h"

#include <errno.h>
#include <fcntl.h>
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

void hp_map_init(hp_map_t *m) {
    memset(m, 0, sizeof *m);
    m->current = -1;
}

void hp_map_free(hp_map_t *m) {
    free(m->src);
    m->src = NULL;
    m->src_len = 0;
}

void hp_map_set_out(hp_map_t *m, hp_irout_t *out) { m->out = out; }
void hp_map_set_actops(hp_map_t *m, const hp_map_actops_t *ops) { m->actops = ops ? *ops : (hp_map_actops_t){0}; }
const char *hp_map_last_error(const hp_map_t *m) { return m->err; }

uint32_t hp_map_crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
    }
    return ~c;
}

bool hp_map_hash_hex(const hp_map_t *m, char out[9]) {
    out[0] = '\0';
    if (!m->src) return false;
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

static int pool_alloc(hp_map_t *m, size_t n, size_t *off) {
    if (m->pool_n + n > HP_MAP_POOL_WORDS)
        return fail(m, TOOBIG, "IR data exceeds the %d-word config pool", HP_MAP_POOL_WORDS);
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
    size_t off, i = 0;
    int r = pool_alloc(m, (size_t)n, &off);
    if (r < 0) return r;
    for (const cJSON *w = pr->child; w; w = w->next, i++) {
        if (!int_in(w, 0, 65535)) return fail(m, INVAL, "pronto words must be integers 0..65535");
        m->pool[off + i] = (uint16_t)w->valuedouble;
    }
    a->type = HP_ACT_IR;
    a->port_mask = (uint8_t)pm->valuedouble;
    a->ir_kind = HP_IR_CODE_PRONTO;
    a->pool_off = (uint16_t)off;
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
    a->type = HP_ACT_IR;
    a->port_mask = pm ? (uint8_t)pm->valuedouble : 255;
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
        size_t off, i = 0;
        int r = pool_alloc(m, (size_t)n, &off);
        if (r < 0) return r;
        for (const cJSON *d = us->child; d; d = d->next, i++) {
            if (!int_in(d, 0, 65535)) return fail(m, INVAL, "raw durations must be integers 0..65535");
            m->pool[off + i] = (uint16_t)d->valuedouble;
        }
        a->ir_kind = HP_IR_CODE_RAW;
        a->hz = (uint32_t)hz->valuedouble;
        a->pool_off = (uint16_t)off;
        a->pool_n = (uint16_t)n;
    } else {
        uint16_t w[HP_MAP_MAX_PRONTO];
        size_t n = 0, off;
        if (!cJSON_IsString(pt)) return fail(m, INVAL, "pronto must be a hex string");
        int r = hp_ir_parse_pronto(pt->valuestring, w, HP_MAP_MAX_PRONTO, &n);
        if (r == -HP_E_TOOBIG) return fail(m, TOOBIG, "pronto longer than %d words", HP_MAP_MAX_PRONTO);
        if (r < 0) return fail(m, INVAL, "pronto is not valid hex words");
        if ((r = pool_alloc(m, n, &off)) < 0) return r;
        memcpy(m->pool + off, w, n * sizeof w[0]);
        a->ir_kind = HP_IR_CODE_PRONTO;
        a->pool_off = (uint16_t)off;
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

static const hp_device_t *find_dev(const hp_map_t *m, const char *id, int *idx) {
    for (int i = 0; i < m->ndevs; i++)
        if (!strcmp(m->devs[i].id, id)) {
            if (idx) *idx = i;
            return &m->devs[i];
        }
    return NULL;
}

static const hp_devcmd_t *find_named(const hp_devcmd_t *tbl, size_t n, int dev, const char *name, int *idx) {
    for (size_t i = 0; i < n; i++)
        if (tbl[i].dev == dev && !strcmp(tbl[i].name, name)) {
            if (idx) *idx = (int)i;
            return &tbl[i];
        }
    return NULL;
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
        const hp_devcmd_t *c = find_named(m->inputs, m->ninputs, di, in->valuestring, NULL);
        if (!c) return fail(m, INVAL, "device '%s' has no input '%.31s'", d->id, in->valuestring);
        *a = c->act;
        return 0;
    }
    const char *cmd = cm->valuestring;
    if (!strcmp(cmd, "power_on") || !strcmp(cmd, "power_off")) {
        if (d->pwr == HP_PWR_NONE) return fail(m, INVAL, "device '%s' has no power codes", d->id);
        *a = (d->pwr == HP_PWR_ONOFF && !strcmp(cmd, "power_off")) ? d->pwr_off : d->pwr_on;
        return 0;
    }
    const hp_devcmd_t *c = find_named(m->cmds, m->ncmds, di, cmd, NULL);
    if (!c) return fail(m, INVAL, "device '%s' has no command '%.31s'", d->id, cmd);
    *a = c->act;
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

static int parse_keymap(hp_map_t *m, const cJSON *j, hp_keybind_t *keys, uint8_t *nkeys) {
    if (!j) return 0;
    if (!cJSON_IsObject(j)) return fail(m, INVAL, "keys must be an object");
    *nkeys = 0;
    for (const cJSON *it = j->child; it; it = it->next) {
        uint16_t code;
        int r;
        if (!it->string || (r = parse_code(m, it->string, &code)) < 0) return it->string ? r : INVAL;
        if (*nkeys >= HP_MAP_MAX_KEYS) return fail(m, TOOBIG, "more than %d keys in one key map", HP_MAP_MAX_KEYS);
        for (uint8_t i = 0; i < *nkeys; i++)
            if (keys[i].code == code) return fail(m, INVAL, "duplicate key %u", (unsigned)code);
        if ((r = parse_action(m, it, &keys[*nkeys].act, true)) < 0) return r;
        keys[(*nkeys)++].code = code;
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

/* Adds the entries of a {"name": <ir>} object to tbl (commands or inputs). */
static int parse_irmap(hp_map_t *m, const cJSON *j, int dev, hp_devcmd_t *tbl, uint16_t *n, size_t cap,
                       const char *what) {
    if (!j) return 0;
    if (!cJSON_IsObject(j)) return fail(m, INVAL, "%s must be an object", what);
    for (const cJSON *it = j->child; it; it = it->next) {
        if (*n >= cap) return fail(m, TOOBIG, "more than %zu %s in total", cap, what);
        hp_devcmd_t *e = &tbl[*n];
        memset(e, 0, sizeof *e);
        e->dev = (uint8_t)dev;
        int r = it->string ? parse_name(m, &(cJSON){.type = cJSON_String, .valuestring = it->string}, e->name, what)
                           : INVAL;
        if (r < 0) return r;
        if (find_named(tbl, *n, dev, e->name, NULL)) return fail(m, INVAL, "duplicate %s '%s'", what, e->name);
        if ((r = parse_ir_v2(m, it, &e->act)) < 0) return r;
        (*n)++;
    }
    return 0;
}

static int parse_devices(hp_map_t *m, const cJSON *j) {
    if (!j) return 0;
    if (!cJSON_IsArray(j)) return fail(m, INVAL, "devices must be an array");
    for (const cJSON *d = j->child; d; d = d->next) {
        if (m->ndevs >= HP_MAP_MAX_DEVICES) return fail(m, TOOBIG, "more than %d devices", HP_MAP_MAX_DEVICES);
        if (!cJSON_IsObject(d)) return fail(m, INVAL, "device must be an object");
        hp_device_t *dev = &m->devs[m->ndevs];
        int r = parse_name(m, cJSON_GetObjectItemCaseSensitive(d, "id"), dev->id, "device id");
        if (r < 0) return r;
        if (find_dev(m, dev->id, NULL)) return fail(m, INVAL, "duplicate device id '%s'", dev->id);
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
        int idx = m->ndevs;
        m->ndevs++; /* visible to the lookups below */
        if ((r = parse_irmap(m, cJSON_GetObjectItemCaseSensitive(d, "inputs"), idx, m->inputs, &m->ninputs,
                             HP_MAP_MAX_INPUTS, "inputs")) < 0)
            return r;
        if ((r = parse_irmap(m, cJSON_GetObjectItemCaseSensitive(d, "commands"), idx, m->cmds, &m->ncmds,
                             HP_MAP_MAX_COMMANDS, "commands")) < 0)
            return r;
    }
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
            int ii;
            if (!find_named(m->inputs, m->ninputs, di, in->valuestring, &ii))
                return fail(m, INVAL, "device '%s' has no input '%.31s'", m->devs[di].id, in->valuestring);
            ad->input = (int16_t)ii;
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
    if (m->format == 2 && (r = parse_devices(m, cJSON_GetObjectItemCaseSensitive(root, "devices"))) < 0) return r;
    const cJSON *acts = cJSON_GetObjectItemCaseSensitive(root, "activities");
    if (acts && !cJSON_IsArray(acts)) return fail(m, INVAL, "activities must be an array");
    if (acts && (r = scan_activities(m, acts)) < 0) return r;
    if ((r = parse_keymap(m, cJSON_GetObjectItemCaseSensitive(root, "keys"), m->keys, &m->nkeys)) < 0) return r;
    if (!acts) return 0;
    uint8_t i = 0;
    for (const cJSON *a = acts->child; a; a = a->next, i++) {
        hp_activity_t *act = &m->acts[i];
        if ((r = parse_keymap(m, cJSON_GetObjectItemCaseSensitive(a, "keys"), act->keys, &act->nkeys)) < 0) return r;
        if ((r = parse_steps(m, cJSON_GetObjectItemCaseSensitive(a, "steps"), act->steps, &act->nsteps)) < 0) return r;
        if ((r = parse_steps(m, cJSON_GetObjectItemCaseSensitive(a, "off"), act->off, &act->noff)) < 0) return r;
        if (m->format == 2 && (r = parse_actdevs(m, cJSON_GetObjectItemCaseSensitive(a, "devices"), act)) < 0)
            return r;
    }
    return 0;
}

int hp_map_load(hp_map_t *m, const char *json, size_t len) {
    if (!m || !json) return INVAL;
    m->err[0] = '\0';
    if (len > HP_MAP_MAX_JSON) return fail(m, TOOBIG, "config larger than %d bytes", HP_MAP_MAX_JSON);
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return fail(m, INVAL, "config is not valid JSON");
    hp_map_t *scratch = calloc(1, sizeof *scratch);
    char *src = malloc(len + 1);
    int r = INVAL;
    if (!scratch || !src) {
        r = -HP_E_IO;
        goto out;
    }
    scratch->current = -1;
    r = parse_config(scratch, root);
    if (r < 0) {
        memcpy(m->err, scratch->err, sizeof m->err);
        goto out;
    }
    memcpy(src, json, len);
    src[len] = '\0';
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
    free(m->src);
    *m = *scratch;
    m->out = outrt;
    m->actops = ops;
    m->src = src;
    m->src_len = len;
    m->hash = hp_map_crc32((const uint8_t *)src, len);
    src = NULL;
    r = 0;
out:
    free(src);
    free(scratch);
    cJSON_Delete(root);
    return r;
}

/* ------------------------------------------------------- lookup / apply */

static const hp_action_t NONE_ACTION = {.type = HP_ACT_NONE};

static const hp_keybind_t *find_key(const hp_keybind_t *keys, uint8_t n, uint16_t code) {
    for (uint8_t i = 0; i < n; i++)
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
        if (!raw || a->pool_n > HP_IR_MAX_DUR || (size_t)a->pool_off + a->pool_n > HP_MAP_POOL_WORDS)
            return -HP_E_INVAL;
        for (size_t i = 0; i < a->pool_n; i++) raw[i] = m->pool[a->pool_off + i];
        c->u.raw.hz = a->hz;
        c->u.raw.us = raw;
        c->u.raw.n = a->pool_n;
        return 0;
    case HP_IR_CODE_PRONTO:
        if ((size_t)a->pool_off + a->pool_n > HP_MAP_POOL_WORDS) return -HP_E_INVAL;
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
        if (m->out) return hp_irout_send(m->out, s->ir, &c, a->port_mask, key);
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

int hp_map_load_cache(hp_map_t *m, const char *path) {
    if (!m || !path) return -HP_E_INVAL;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? -HP_E_NOENT : -HP_E_IO;
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return -HP_E_IO;
    }
    if (st.st_size < CACHE_HDR + 4 || st.st_size > CACHE_HDR + HP_MAP_MAX_JSON + 4) {
        close(fd);
        return -HP_E_INVAL;
    }
    size_t total = (size_t)st.st_size, got = 0;
    uint8_t *buf = malloc(total);
    if (!buf) {
        close(fd);
        return -HP_E_IO;
    }
    while (got < total) {
        ssize_t r = read(fd, buf + got, total - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    int rc = -HP_E_INVAL;
    if (got == total && !memcmp(buf, "HPMC", 4) && buf[4] == CACHE_VER) {
        size_t len = get32(buf + 5);
        if (len == total - CACHE_HDR - 4 && get32(buf + CACHE_HDR + len) == hp_map_crc32(buf, CACHE_HDR + len))
            rc = hp_map_load(m, (const char *)buf + CACHE_HDR, len);
    }
    free(buf);
    return rc;
}
