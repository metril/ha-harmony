#define _POSIX_C_SOURCE 200809L
#include "hub/engine/mapping.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hub/hp_errors.h"
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

/* ------------------------------------------------------------- parsing */

#define INVAL (-HP_E_INVAL)
#define TOOBIG (-HP_E_TOOBIG)

static bool int_in(const cJSON *v, double lo, double hi) {
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) && v->valuedouble == floor(v->valuedouble) &&
           v->valuedouble >= lo && v->valuedouble <= hi;
}

static int parse_name(const cJSON *v, char *dst) {
    if (!cJSON_IsString(v)) return INVAL;
    size_t n = strlen(v->valuestring);
    if (n == 0) return INVAL;
    if (n >= HP_MAP_NAME_MAX) return TOOBIG;
    memcpy(dst, v->valuestring, n + 1);
    return 0;
}

static int parse_action(const cJSON *j, hp_map_t *m, hp_action_t *a) {
    if (!cJSON_IsObject(j)) return INVAL;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "type");
    if (!cJSON_IsString(t)) return INVAL;
    memset(a, 0, sizeof *a);
    if (!strcmp(t->valuestring, "none")) {
        a->type = HP_ACT_NONE;
    } else if (!strcmp(t->valuestring, "bt")) {
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(j, "usage");
        if (!int_in(u, 0, 65535)) return INVAL;
        a->type = HP_ACT_BT;
        a->usage = (uint16_t)u->valuedouble;
    } else if (!strcmp(t->valuestring, "ha")) {
        a->type = HP_ACT_HA_EVENT;
        return parse_name(cJSON_GetObjectItemCaseSensitive(j, "event"), a->event);
    } else if (!strcmp(t->valuestring, "ir")) {
        const cJSON *pm = cJSON_GetObjectItemCaseSensitive(j, "port_mask");
        const cJSON *pr = cJSON_GetObjectItemCaseSensitive(j, "pronto");
        if (!int_in(pm, 1, 255) || !cJSON_IsArray(pr)) return INVAL;
        int n = cJSON_GetArraySize(pr);
        if (n < 4) return INVAL;
        if (n > HP_MAP_MAX_PRONTO || m->pool_n + (size_t)n > HP_MAP_POOL_WORDS) return TOOBIG;
        size_t off = m->pool_n, i = 0;
        for (const cJSON *w = pr->child; w; w = w->next, i++) {
            if (!int_in(w, 0, 65535)) return INVAL;
            m->pool[off + i] = (uint16_t)w->valuedouble;
        }
        m->pool_n += (size_t)n;
        a->type = HP_ACT_IR;
        a->port_mask = (uint8_t)pm->valuedouble;
        a->pronto_off = (uint16_t)off;
        a->pronto_n = (uint16_t)n;
    } else {
        return INVAL;
    }
    return 0;
}

static int parse_code(const char *s, uint16_t *out) {
    size_t n = strlen(s);
    if (n == 0 || n > 5) return INVAL;
    unsigned long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return INVAL;
        v = v * 10 + (unsigned long)(s[i] - '0');
    }
    if (v > 65535) return INVAL;
    *out = (uint16_t)v;
    return 0;
}

static int parse_keymap(const cJSON *j, hp_map_t *m, hp_keybind_t *keys, uint8_t *nkeys) {
    if (!j) return 0;
    if (!cJSON_IsObject(j)) return INVAL;
    *nkeys = 0;
    for (const cJSON *it = j->child; it; it = it->next) {
        uint16_t code;
        if (!it->string || parse_code(it->string, &code) < 0) return INVAL;
        if (*nkeys >= HP_MAP_MAX_KEYS) return TOOBIG;
        for (uint8_t i = 0; i < *nkeys; i++)
            if (keys[i].code == code) return INVAL;
        int r = parse_action(it, m, &keys[*nkeys].act);
        if (r < 0) return r;
        keys[(*nkeys)++].code = code;
    }
    return 0;
}

static int parse_steps(const cJSON *j, hp_map_t *m, hp_action_t *out, uint8_t *n) {
    if (!j) return 0;
    if (!cJSON_IsArray(j)) return INVAL;
    *n = 0;
    for (const cJSON *it = j->child; it; it = it->next) {
        if (*n >= HP_MAP_MAX_STEPS) return TOOBIG;
        int r = parse_action(it, m, &out[*n]);
        if (r < 0) return r;
        (*n)++;
    }
    return 0;
}

static int parse_config(const cJSON *root, hp_map_t *m) {
    if (!cJSON_IsObject(root)) return INVAL;
    int r = parse_keymap(cJSON_GetObjectItemCaseSensitive(root, "keys"), m, m->keys, &m->nkeys);
    if (r < 0) return r;
    const cJSON *acts = cJSON_GetObjectItemCaseSensitive(root, "activities");
    if (!acts) return 0;
    if (!cJSON_IsArray(acts)) return INVAL;
    for (const cJSON *a = acts->child; a; a = a->next) {
        if (m->nacts >= HP_MAP_MAX_ACTIVITIES) return TOOBIG;
        if (!cJSON_IsObject(a)) return INVAL;
        hp_activity_t *act = &m->acts[m->nacts];
        r = parse_name(cJSON_GetObjectItemCaseSensitive(a, "name"), act->name);
        if (r < 0) return r;
        for (uint8_t i = 0; i < m->nacts; i++)
            if (!strcmp(m->acts[i].name, act->name)) return INVAL;
        if ((r = parse_keymap(cJSON_GetObjectItemCaseSensitive(a, "keys"), m, act->keys, &act->nkeys)) < 0) return r;
        if ((r = parse_steps(cJSON_GetObjectItemCaseSensitive(a, "steps"), m, act->steps, &act->nsteps)) < 0) return r;
        if ((r = parse_steps(cJSON_GetObjectItemCaseSensitive(a, "off"), m, act->off, &act->noff)) < 0) return r;
        m->nacts++;
    }
    return 0;
}

int hp_map_load(hp_map_t *m, const char *json, size_t len) {
    if (!m || !json) return INVAL;
    if (len > HP_MAP_MAX_JSON) return TOOBIG;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return INVAL;
    hp_map_t *scratch = calloc(1, sizeof *scratch);
    char *src = malloc(len + 1);
    int r = INVAL;
    if (!scratch || !src) {
        r = -HP_E_IO;
        goto out;
    }
    scratch->current = -1;
    r = parse_config(root, scratch);
    if (r < 0) goto out;
    memcpy(src, json, len);
    src[len] = '\0';
    /* keep the running activity if it survives the reload */
    if (m->current >= 0 && (size_t)m->current < m->nacts) {
        for (uint8_t i = 0; i < scratch->nacts; i++)
            if (!strcmp(scratch->acts[i].name, m->acts[m->current].name)) scratch->current = i;
    }
    free(m->src);
    *m = *scratch;
    m->src = src;
    m->src_len = len;
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

int hp_map_apply(const hp_map_t *m, const hp_action_t *a, const hp_sinks_t *s) {
    if (!m || !a || !s) return -HP_E_INVAL;
    switch (a->type) {
    case HP_ACT_IR:
        if (!s->ir || (size_t)a->pronto_off + a->pronto_n > HP_MAP_POOL_WORDS) return -HP_E_INVAL;
        return s->ir->send(s->ir->self, a->port_mask, m->pool + a->pronto_off, a->pronto_n);
    case HP_ACT_BT: {
        if (!s->bt) return -HP_E_INVAL;
        int r = s->bt->key(s->bt->self, a->usage, true);
        return r < 0 ? r : s->bt->key(s->bt->self, a->usage, false);
    }
    case HP_ACT_HA_EVENT:
    case HP_ACT_NONE:
        return 0;
    }
    return -HP_E_INVAL;
}

size_t hp_map_activity_count(const hp_map_t *m) { return m->nacts; }
const char *hp_map_activity_name(const hp_map_t *m, size_t i) { return i < m->nacts ? m->acts[i].name : NULL; }
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

int hp_key_handle(hp_map_t *m, const hp_sinks_t *s, uint8_t remote, uint8_t key, hp_key_action_t action,
                  hp_key_emit_fn emit, void *ctx) {
    if (!m || !s) return -HP_E_INVAL;
    int rc = 0;
    if (action == HP_KEY_ACTION_PRESS) {
        const char *cur = m->current >= 0 && m->current < m->nacts ? m->acts[m->current].name : NULL;
        hp_action_t a;
        if (hp_map_lookup(m, cur, key, &a) == 1) rc = hp_map_apply(m, &a, s);
    }
    if (emit) emit(ctx, remote, key, action);
    return rc;
}

/* --------------------------------------------------------------- cache */

static uint32_t crc32_ieee(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
    }
    return ~c;
}

#define CACHE_HDR 9 /* magic(4) ver(1) len(4) */
#define CACHE_VER 1

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int write_all(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int hp_map_save_cache(const hp_map_t *m, const char *path) {
    if (!m || !path || !m->src) return -HP_E_INVAL;
    size_t total = CACHE_HDR + m->src_len + 4;
    uint8_t *buf = malloc(total);
    char *tmp = malloc(strlen(path) + 32);
    if (!buf || !tmp) {
        free(buf);
        free(tmp);
        return -HP_E_IO;
    }
    memcpy(buf, "HPMC", 4);
    buf[4] = CACHE_VER;
    put32(buf + 5, (uint32_t)m->src_len);
    memcpy(buf + CACHE_HDR, m->src, m->src_len);
    put32(buf + CACHE_HDR + m->src_len, crc32_ieee(buf, CACHE_HDR + m->src_len));
    sprintf(tmp, "%s.tmp.%ld", path, (long)getpid());
    int rc = -HP_E_IO;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        bool ok = write_all(fd, buf, total) == 0 && fsync(fd) == 0;
        ok = (close(fd) == 0) && ok;
        if (ok && rename(tmp, path) == 0) {
            rc = 0;
            char *dup = strdup(path); /* best-effort: persist the rename */
            if (dup) {
                int dfd = open(dirname(dup), O_RDONLY);
                if (dfd >= 0) {
                    (void)fsync(dfd);
                    close(dfd);
                }
                free(dup);
            }
        } else {
            unlink(tmp);
        }
    }
    free(buf);
    free(tmp);
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
        if (len == total - CACHE_HDR - 4 && get32(buf + CACHE_HDR + len) == crc32_ieee(buf, CACHE_HDR + len))
            rc = hp_map_load(m, (const char *)buf + CACHE_HDR, len);
    }
    free(buf);
    return rc;
}
