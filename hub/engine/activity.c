#define _POSIX_C_SOURCE 200809L
#include "hub/net/hublog.h"
#include "hub/engine/activity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hub/engine/atomic_file.h"
#include "hub/hp_errors.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* Ms left until `deadline`, or 0. A stored deadline is only trusted while it lies at most `len`
 * (the span it was set with) ahead: a wraparound-style signed compare would see a deadline that
 * went stale >= 2^31 ms ago (hub idle ~25 days) as being in the future. */
static uint32_t remaining(uint32_t deadline, uint32_t now, uint32_t len) {
    uint32_t d = deadline - now;
    return d > 0 && d <= len ? d : 0;
}
static uint32_t now_of(const hp_actsm_t *e) { return hp_sched_now(e->sched); }

static void emit(hp_actsm_t *e, hp_actsm_evt_t ev, int idx) {
    if (e->notify) e->notify(e->notify_ctx, ev, idx);
}

/* The reported activity is remembered by identity (id for format 2, name for format 1) so a reload
 * can re-resolve it in the new map whatever its index/name became. */
static void set_reported(hp_actsm_t *e, int idx) {
    const hp_map_t *m = e->map;
    e->reported = idx;
    e->rep_key[0] = 0;
    if (idx >= 0 && idx < m->nacts) {
        snprintf(e->rep_key, sizeof e->rep_key, "%s", m->format == 2 ? m->acts[idx].id : m->acts[idx].name);
        snprintf(e->last_key, sizeof e->last_key, "%s", e->rep_key);
    }
}

static const char *act_key(const hp_map_t *m, int idx) {
    return idx >= 0 && idx < m->nacts ? (m->format == 2 ? m->acts[idx].id : m->acts[idx].name) : "";
}

static int resolve_key(const hp_actsm_t *e, const char *key) {
    const hp_map_t *m = e->map;
    for (int i = 0; key[0] && i < m->nacts; i++)
        if (!strcmp(key, act_key(m, i))) return i;
    return -1;
}

static int resolve_reported(const hp_actsm_t *e) { return resolve_key(e, e->rep_key); }

/* ------------------------------------------------------------ persistence */

#define STATE_HDR 9 /* magic(4) ver(1) hash(4) */
#define STATE_MAX (STATE_HDR + 1 + HP_MAP_NAME_MAX + 1 + HP_MAP_MAX_DEVICES * (3 + 2 * HP_MAP_NAME_MAX) + 4)

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static size_t put_str(uint8_t *p, const char *s) {
    size_t n = strlen(s);
    p[0] = (uint8_t)n;
    memcpy(p + 1, s, n);
    return 1 + n;
}

static void persist_tick(void *arg, uint32_t now);

static int write_state(hp_actsm_t *e) {
    uint8_t *buf = malloc(STATE_MAX); /* ~17 KiB at the device ceiling: not for the stack */
    if (!buf) return -HP_E_IO;
    size_t n = 0;
    memcpy(buf, "HPST", 4);
    buf[4] = HP_ACT_STATE_VER;
    put32(buf + 5, e->map->hash);
    n = STATE_HDR;
    n += put_str(buf + n, e->reported >= 0 && e->reported < e->map->nacts ? e->map->acts[e->reported].id : "");
    buf[n++] = e->ndev;
    for (uint8_t i = 0; i < e->ndev; i++) {
        n += put_str(buf + n, e->dev[i].id);
        buf[n++] = e->dev[i].power;
        n += put_str(buf + n, e->dev[i].input);
    }
    put32(buf + n, hp_map_crc32(buf, n));
    n += 4;
    hp_sched_cancel(e->sched, e->persist_h);
    e->persist_h = 0;
    int rc = hp_atomic_write(e->state_path, buf, n);
    free(buf);
    e->last_write = now_of(e);
    e->wrote = true;
    if (rc < 0) {
        hp_log("hub: state write to %s failed (%d)\n", e->state_path, rc);
        e->persist_h = hp_sched_after(e->sched, HP_ACT_PERSIST_MS, persist_tick, e); /* retry later */
        return rc;
    }
    e->writes++;
    e->dirty = false;
    e->saved_hash = e->map->hash;
    return 0;
}

static void persist_tick(void *arg, uint32_t now) {
    (void)now;
    hp_actsm_t *e = arg;
    e->persist_h = 0;
    if (e->dirty && e->state_path && !e->running) (void)write_state(e); /* a running transition writes once, in finish() */
}

/* Debounce: the first change after a quiet period of HP_ACT_PERSIST_MS writes at once; changes
 * inside the window coalesce into one trailing write at its end. */
static void mark_dirty(hp_actsm_t *e) {
    e->dirty = true;
    if (!e->state_path || e->persist_h || e->running) return;
    uint32_t since = now_of(e) - e->last_write; /* unsigned: a stale last_write reads as "long ago" */
    if (!e->wrote || since >= HP_ACT_PERSIST_MS) {
        (void)write_state(e);
        return;
    }
    e->persist_h = hp_sched_after(e->sched, HP_ACT_PERSIST_MS - since, persist_tick, e);
}

void hp_actsm_set_state_path(hp_actsm_t *e, const char *path) { e->state_path = path; }

int hp_actsm_flush_state(hp_actsm_t *e) {
    if (!e->state_path || !e->dirty) return 0;
    return write_state(e);
}

static int input_index(const hp_map_t *m, int dev, const char *name) { return hp_map_find_input(m, dev, name); }

static int load_state_buf(hp_actsm_t *e, const uint8_t *buf, ssize_t got, hp_devstate_t *st);

int hp_actsm_load_state(hp_actsm_t *e) {
    if (!e->state_path) return -HP_E_INVAL;
    int fd = open(e->state_path, O_RDONLY);
    if (fd < 0) return -HP_E_NOENT;
    uint8_t *buf = malloc(STATE_MAX + 1); /* ~17 KiB each at the device ceiling: not for the stack */
    hp_devstate_t *st = malloc(HP_MAP_MAX_DEVICES * sizeof *st);
    ssize_t got = 0, r = 0;
    int rc = -HP_E_IO;
    if (buf && st) {
        while ((r = read(fd, buf + got, STATE_MAX + 1 - (size_t)got)) > 0) got += r;
        rc = r < 0 ? -HP_E_INVAL : load_state_buf(e, buf, got, st);
    }
    close(fd);
    free(buf);
    free(st);
    return rc;
}

static int load_state_buf(hp_actsm_t *e, const uint8_t *buf, ssize_t got, hp_devstate_t *st) {
    if (got < STATE_HDR + 2 + 4 || (size_t)got > STATE_MAX) return -HP_E_INVAL;
    size_t n = (size_t)got - 4;
    if (memcmp(buf, "HPST", 4) || buf[4] != HP_ACT_STATE_VER || get32(buf + n) != hp_map_crc32(buf, n))
        return -HP_E_INVAL;
    uint32_t file_hash = get32(buf + 5); /* another config: still restored, per id */
    size_t p = STATE_HDR;
    /* parse into locals first: a malformed body must not half-apply */
    char act[HP_MAP_NAME_MAX];
    if (p >= n || buf[p] >= HP_MAP_NAME_MAX || p + 1 + buf[p] >= n) return -HP_E_INVAL;
    memcpy(act, buf + p + 1, buf[p]);
    act[buf[p]] = 0;
    p += 1 + buf[p];
    unsigned nd = buf[p++];
    if (nd > HP_MAP_MAX_DEVICES) return -HP_E_INVAL;
    for (unsigned i = 0; i < nd; i++) {
        memset(&st[i], 0, sizeof st[i]);
        if (p >= n || buf[p] >= HP_MAP_NAME_MAX || p + 1 + buf[p] + 1 >= n) return -HP_E_INVAL;
        memcpy(st[i].id, buf + p + 1, buf[p]);
        p += 1 + buf[p];
        st[i].power = buf[p++] ? 1 : 0;
        if (buf[p] >= HP_MAP_NAME_MAX || p + 1 + buf[p] > n) return -HP_E_INVAL;
        memcpy(st[i].input, buf + p + 1, buf[p]);
        p += 1 + buf[p];
    }
    if (p != n) return -HP_E_INVAL;
    int idx = -1;
    for (uint8_t i = 0; *act && i < e->map->nacts; i++)
        if (!strcmp(e->map->acts[i].id, act)) idx = i;
    for (unsigned i = 0; i < nd; i++)
        for (uint8_t d = 0; d < e->ndev; d++) {
            if (strcmp(e->dev[d].id, st[i].id)) continue;
            e->dev[d].power = st[i].power;
            e->dev[d].input[0] = 0;
            if (st[i].power && input_index(e->map, d, st[i].input) >= 0) strcpy(e->dev[d].input, st[i].input);
        }
    set_reported(e, idx);
    e->target = idx;
    e->map->current = idx;
    e->saved_hash = file_hash;
    e->dirty = file_hash != e->map->hash; /* rewritten with the new hash at the next change / shutdown */
    return 0;
}

/* ------------------------------------------------------------------ plan */

static void cancel_plan(hp_actsm_t *e) {
    hp_sched_cancel(e->sched, e->timer);
    e->timer = 0;
    e->running = false;
    e->resync = false;
    e->nplan = e->pos = 0;
    hp_sched_cancel(e->sched, e->kq_h);
    e->kq_h = 0;
    hp_map_keyq_drop(e->map); /* key presses queued under the cancelled plan are not replayed */
}

static void add(hp_actsm_t *e, hp_stepkind_t k, int dev, int input, const hp_action_t *act, bool force) {
    if (e->nplan >= HP_ACT_PLAN_MAX) return; /* sized for the worst case; unreachable */
    e->plan[e->nplan++] = (hp_actstep_t){(uint8_t)k, (uint8_t)dev, force, (int32_t)input, act};
}

/* Ms until the (selected) devices have settled, 0 = none pending. */
static uint32_t settle_wait(const hp_actsm_t *e, int dev, uint32_t now) {
    uint32_t w = 0;
    for (int i = 0; i < e->ndev; i++) {
        if ((dev >= 0 && i != dev) || !e->dev[i].settling) continue;
        uint32_t r = remaining(e->dev[i].settle_until, now, e->map->devs[i].settle_ms);
        if (r > w) w = r;
    }
    return w;
}

static bool redundant(const hp_actsm_t *e, const hp_actstep_t *s) {
    /* forced steps (a resync, a stale Off): discrete power / input codes are idempotent, so resend them whatever
     * the tracked state says (it may be stale); toggle power would flip the device and stays skipped by tracked state */
    bool force = s->force && s->kind != HP_STEP_EXTRA && s->kind != HP_STEP_OFFX &&
                 (s->kind == HP_STEP_INPUT || e->map->devs[s->dev].pwr == HP_PWR_ONOFF);
    if (force) return false;
    switch (s->kind) {
    case HP_STEP_PWR_OFF: return !e->dev[s->dev].power;
    case HP_STEP_PWR_ON: return e->dev[s->dev].power;
    case HP_STEP_INPUT: return !strcmp(e->dev[s->dev].input, e->map->inputs[s->input].name);
    default: return false;
    }
}

/* A plan step the tracked state calls unnecessary is not sent: say which (a stale tracked state shows up here). */
static void log_redundant(const hp_actsm_t *e, const hp_actstep_t *s) {
    const char *what = s->kind == HP_STEP_PWR_OFF ? "already off" : s->kind == HP_STEP_PWR_ON ? "already on" : "already on that input";
    hp_log("hub: step skipped, tracked state says %s is %s\n", e->map->devs[s->dev].name, what);
}

static void set_dev(hp_actsm_t *e, int d, int power, const char *input) {
    hp_devstate_t *ds = &e->dev[d];
    if (ds->power == power && !strcmp(ds->input, input)) return;
    ds->power = (uint8_t)power;
    snprintf(ds->input, sizeof ds->input, "%s", input);
    emit(e, HP_ACTSM_EVT_DEVICE, d);
    mark_dirty(e);
}

/* Builds the plan from the tracked state. idx >= 0: start that activity (`resync`: it is the one already
 * current, resend its own discrete power / input steps, no off frames to the unused devices). idx < 0: power
 * down; `prev` = the activity whose off steps run (-1 none). */
static void build_plan(hp_actsm_t *e, int idx, int prev, bool resync) {
    const hp_map_t *m = e->map;
    e->nplan = e->pos = 0;
    if (idx >= 0) {
        const hp_activity_t *a = &m->acts[idx];
        bool used[HP_MAP_MAX_DEVICES] = {false};
        for (uint8_t i = 0; i < a->ndevs; i++) used[a->devs[i].dev] = true;
        for (int d = 0; d < e->ndev && !resync; d++)
            if (!used[d]) add(e, HP_STEP_PWR_OFF, d, -1, NULL, false);
        for (uint8_t i = 0; i < a->ndevs; i++) add(e, HP_STEP_PWR_ON, a->devs[i].dev, -1, NULL, resync);
        for (uint8_t i = 0; i < a->ndevs; i++)
            if (a->devs[i].input >= 0) add(e, HP_STEP_INPUT, a->devs[i].dev, a->devs[i].input, NULL, resync);
        for (uint8_t i = 0; i < a->nsteps; i++) add(e, HP_STEP_EXTRA, 0, -1, &a->steps[i], false);
        return;
    }
    /* Off: whatever is tracked on goes off; the devices of the current (else the last) activity - every device when
     * none is known - also get their discrete off code when the tracking says they already are off (it may be stale) */
    int act = prev >= 0 ? prev : resolve_key(e, e->last_key);
    bool used[HP_MAP_MAX_DEVICES];
    memset(used, act < 0, sizeof used);
    for (uint8_t i = 0; act >= 0 && i < m->acts[act].ndevs; i++) used[m->acts[act].devs[i].dev] = true;
    for (int d = 0; d < e->ndev; d++) {
        if (e->dev[d].power) add(e, HP_STEP_PWR_OFF, d, -1, NULL, false);
        else if (used[d] && m->devs[d].pwr == HP_PWR_ONOFF) add(e, HP_STEP_PWR_OFF, d, -1, NULL, true);
    }
    if (prev >= 0 && prev < m->nacts)
        for (uint8_t i = 0; i < m->acts[prev].noff; i++) add(e, HP_STEP_OFFX, 0, -1, &m->acts[prev].off[i], false);
}

static void arm_cq(hp_actsm_t *e, uint32_t delay);
static void drain(hp_actsm_t *e);
static void after_finish(hp_actsm_t *e);
static void kq_cb(void *arg, uint32_t now);
static void cq_drop_inputs(hp_actsm_t *e, const char *dev);

/* Queued keys and commands run once every device has settled (a booting device ignores them). */
static void after_finish(hp_actsm_t *e) {
    uint32_t w = settle_wait(e, -1, now_of(e));
    if (w) {
        hp_sched_cancel(e->sched, e->kq_h);
        e->kq_h = hp_sched_after(e->sched, w, kq_cb, e); /* one-shot, tied to the settle time */
        if (e->kq_h) return;
    }
    hp_map_keyq_replay(e->map, e->sinks);
    drain(e);
}

static void finish(hp_actsm_t *e) {
    e->running = false;
    e->resync = false;
    e->nplan = e->pos = 0;
    set_reported(e, e->target);
    e->map->current = e->target;
    e->off_emit = e->off_press && e->target < 0; /* an actual Off press, not a resync / reload */
    e->off_press = false;
    emit(e, HP_ACTSM_EVT_ACTIVITY, e->reported);
    e->off_emit = false;
    mark_dirty(e); /* the transition's one state write */
    after_finish(e);
}

/* The plan could not go on: back to the reported activity, and every client is told so it resyncs. */
static void abort_plan(hp_actsm_t *e, int rc) {
    hp_log("hub: activity transition aborted (%d)\n", rc);
    cancel_plan(e);
    e->off_press = false;
    e->map->current = e->reported;
    e->target = e->reported;
    emit(e, HP_ACTSM_EVT_ACTIVITY, e->reported);
    mark_dirty(e);
    drain(e);
}

static int arm(hp_actsm_t *e, uint32_t delay);

/* Executes due steps; schedules the timer for the first one that must wait. 0 or a send error
 * (the plan is then aborted). */
static int run(hp_actsm_t *e) {
    for (;;) {
        if (e->pos >= e->nplan) {
            finish(e);
            return 0;
        }
        const hp_actstep_t *s = &e->plan[e->pos];
        uint32_t now = now_of(e);
        if (redundant(e, s)) {
            log_redundant(e, s);
            e->pos++;
            continue;
        }
        const hp_action_t *act = s->act;
        const hp_device_t *dv = s->kind == HP_STEP_EXTRA || s->kind == HP_STEP_OFFX ? NULL : &e->map->devs[s->dev];
        if (s->kind == HP_STEP_PWR_ON) act = dv->pwr == HP_PWR_NONE ? NULL : &dv->pwr_on;
        else if (s->kind == HP_STEP_PWR_OFF) act = dv->pwr == HP_PWR_ONOFF ? &dv->pwr_off : dv->pwr == HP_PWR_TOGGLE ? &dv->pwr_on : NULL;
        else if (s->kind == HP_STEP_INPUT) act = &e->map->inputs[s->input].act;
        bool sends = act && (act->type == HP_ACT_IR || act->type == HP_ACT_BT);

        uint32_t delay = 0, w;
        if (sends && e->bus_busy) delay = remaining(e->bus_free, now, e->bus_len);
        if (act && act->type == HP_ACT_IR && e->map->out && (w = hp_irout_wait_ms(e->map->out, now)) > delay)
            delay = w; /* a key press / ir_send / SIRC copies still own the line */
        if ((s->kind == HP_STEP_INPUT || s->kind == HP_STEP_EXTRA) &&
            (w = settle_wait(e, s->kind == HP_STEP_INPUT ? s->dev : -1, now)) > delay)
            delay = w;
        if (delay) return arm(e, delay);
        if (act) {
            int rc = hp_map_apply(e->map, act, e->sinks);
            if (rc < 0) {
                abort_plan(e, rc);
                return rc;
            }
            if (sends) {
                uint32_t busy = act->type == HP_ACT_IR && e->map->out ? e->map->out->busy_ms : 0;
                e->bus_len = busy + HP_ACT_GUARD_MS;
                e->bus_free = now + e->bus_len;
                e->bus_busy = true;
            }
        }
        switch (s->kind) {
        case HP_STEP_PWR_ON:
            e->dev[s->dev].settling = dv->settle_ms > 0; /* a forced resend to a device tracked on settles too (it may be booting) */
            e->dev[s->dev].settle_until = now + dv->settle_ms;
            if (!e->dev[s->dev].power) set_dev(e, s->dev, 1, ""); /* ... but keeps its tracked input */
            break;
        case HP_STEP_PWR_OFF:
            e->dev[s->dev].settling = false;
            set_dev(e, s->dev, 0, "");
            cq_drop_inputs(e, e->dev[s->dev].id);
            break;
        case HP_STEP_INPUT:
            e->dev[s->dev].settling = dv->settle_ms > 0;
            e->dev[s->dev].settle_until = now + dv->settle_ms;
            set_dev(e, s->dev, e->dev[s->dev].power, e->map->inputs[s->input].name);
            break;
        default:
            break;
        }
        e->pos++;
    }
}

static void step_cb(void *arg, uint32_t now) {
    (void)now;
    hp_actsm_t *e = arg;
    e->timer = 0;
    if (e->running) (void)run(e);
}

static int arm(hp_actsm_t *e, uint32_t delay) {
    e->timer = hp_sched_after(e->sched, delay, step_cb, e);
    if (e->timer) return 0;
    abort_plan(e, -HP_E_BUSY); /* timer queue full */
    return -HP_E_BUSY;
}

/* ------------------------------------------------------------- requests */

static int begin(hp_actsm_t *e, int target, int prev, bool resync) {
    e->target = target;
    snprintf(e->tgt_key, sizeof e->tgt_key, "%s", act_key(e->map, target));
    snprintf(e->prev_key, sizeof e->prev_key, "%s", act_key(e->map, prev));
    e->resync = resync;
    build_plan(e, target, prev, resync);
    e->map->current = target;
    e->running = true;
    int rc = run(e);
    return rc < 0 ? rc : 0;
}

int hp_actsm_start_idx(hp_actsm_t *e, int idx) {
    hp_map_t *m = e->map;
    if (idx < 0 || idx >= m->nacts) return -HP_E_INVAL;
    emit(e, HP_ACTSM_EVT_START, idx);
    e->off_press = false; /* a start supersedes a pending Off */
    if (m->format != 2) {
        int rc = hp_activity_set(m, m->acts[idx].name, e->sinks);
        if (rc < 0) return rc;
        set_reported(e, rc);
        emit(e, HP_ACTSM_EVT_ACTIVITY, rc);
        mark_dirty(e);
        return rc;
    }
    cancel_plan(e);
    hp_irout_cancel(m->out); /* a key hold must not repeat into the plan's frames */
    hp_map_bt_release(m, e->sinks);
    bool resync = e->reported == idx && m->current == idx;
    if (resync) hp_log("hub: activity %s re-pressed: resync (resending discrete steps)\n", m->acts[idx].name);
    int rc = begin(e, idx, -1, resync);
    return rc < 0 ? rc : idx;
}

int hp_actsm_start(hp_actsm_t *e, const char *name) {
    for (uint8_t i = 0; name && i < e->map->nacts; i++)
        if (!strcmp(e->map->acts[i].name, name)) return hp_actsm_start_idx(e, i);
    return -HP_E_INVAL;
}

int hp_actsm_off(hp_actsm_t *e) {
    hp_map_t *m = e->map;
    if (m->format != 2) {
        int rc = hp_activity_off(m, e->sinks);
        if (rc < 0) return rc;
        set_reported(e, -1);
        e->off_emit = true;
        emit(e, HP_ACTSM_EVT_ACTIVITY, -1);
        e->off_emit = false;
        mark_dirty(e);
        return rc;
    }
    int prev = m->current; /* the activity (or interrupted target) whose off steps run */
    cancel_plan(e);
    e->off_press = true;
    hp_irout_cancel(m->out);
    hp_map_bt_release(m, e->sinks);
    int rc = begin(e, -1, prev, false);
    return rc < 0 ? rc : (prev < 0 ? 0 : prev);
}

bool hp_actsm_off_press(const hp_actsm_t *e) { return e->off_emit; }
bool hp_actsm_busy(const hp_actsm_t *e) { return e->running; }
int hp_actsm_reported(const hp_actsm_t *e) { return e->reported; }

const hp_devstate_t *hp_actsm_device(const hp_actsm_t *e, size_t i) { return i < e->ndev ? &e->dev[i] : NULL; }

static int find_dev(const hp_actsm_t *e, const char *id) {
    int d = -1;
    for (int i = 0; id && i < e->ndev; i++)
        if (!strcmp(e->dev[i].id, id)) d = i;
    return d;
}

int hp_actsm_set_device(hp_actsm_t *e, const char *device, int power, const char *input) {
    int d = find_dev(e, device);
    if (d < 0) return -HP_E_INVAL;
    if (input && *input && input_index(e->map, d, input) < 0) return -HP_E_INVAL;
    int np = power >= 0 ? (power ? 1 : 0) : e->dev[d].power;
    const char *ni = input ? input : e->dev[d].input;
    if (!np && !input) ni = ""; /* off devices have no known input */
    char keep[HP_MAP_NAME_MAX];
    snprintf(keep, sizeof keep, "%s", ni);
    set_dev(e, d, np, keep);
    return 0;
}

/* ------------------------------------------------------ queued requests */

static int cq_push(hp_actsm_t *e, const hp_actcmd_t *c, bool front) {
    if (e->cq_n >= HP_ACT_CMDQ_MAX) return -HP_E_BUSY;
    if (front) memmove(&e->cq[1], &e->cq[0], e->cq_n * sizeof e->cq[0]);
    e->cq[front ? 0 : e->cq_n] = *c;
    e->cq_n++;
    return 0;
}

static void cq_pop(hp_actsm_t *e) {
    memmove(&e->cq[0], &e->cq[1], (size_t)(e->cq_n - 1) * sizeof e->cq[0]);
    e->cq_n--;
}

int hp_actsm_defer_irsend(hp_actsm_t *e, char *json) {
    hp_actcmd_t c = {.kind = 2, .text = json};
    if (cq_push(e, &c, false) < 0) {
        hp_log("hub: ir_send dropped: transition in progress, queue full\n");
        return -HP_E_BUSY;
    }
    hp_log("hub: ir_send queued: transition in progress\n");
    return 0;
}

void hp_actsm_set_irsend(hp_actsm_t *e, void (*fn)(void *, const char *), void *ctx) {
    e->irsend_fn = fn;
    e->irsend_ctx = ctx;
}

static void kq_cb(void *arg, uint32_t now) {
    (void)now;
    hp_actsm_t *e = arg;
    e->kq_h = 0;
    if (e->running) return;
    after_finish(e);
}

/* A device that was powered off must not get an input queued behind its power-on. */
static void cq_drop_inputs(hp_actsm_t *e, const char *dev) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < e->cq_n; i++) {
        bool drop = e->cq[i].kind == 1 && e->cq[i].nopre && !strcmp(e->cq[i].dev, dev);
        if (drop) hp_log("hub: queued %s %s dropped: device powered off\n", e->cq[i].dev, e->cq[i].cmd);
        else e->cq[n++] = e->cq[i];
    }
    e->cq_n = n;
}

static void cq_cb(void *arg, uint32_t now) {
    (void)now;
    hp_actsm_t *e = arg;
    e->cq_h = 0;
    drain(e);
}

static void arm_cq(hp_actsm_t *e, uint32_t delay) {
    hp_sched_cancel(e->sched, e->cq_h);
    e->cq_h = hp_sched_after(e->sched, delay, cq_cb, e); /* one-shot, tied to the device's settle time */
    if (!e->cq_h) hp_log("hub: timer queue full, queued command waits for the next event\n");
}

static int devcmd(hp_actsm_t *e, const char *device, const char *command, bool nopre);

/* Runs the queued requests in order while no transition runs; an input command waits for its device to settle. */
static void drain(hp_actsm_t *e) {
    if (e->draining) return;
    e->draining = true;
    while (e->cq_n && !e->running && !e->npc) {
        hp_actcmd_t c = e->cq[0];
        if (c.kind == 1 && !strncmp(c.cmd, "input_", 6)) {
            int d = find_dev(e, c.dev);
            uint32_t w = d >= 0 ? settle_wait(e, d, now_of(e)) : 0;
            if (w) {
                arm_cq(e, w);
                break;
            }
        }
        if (c.kind == 1 && c.nopre) { /* its power-on was undone or failed meanwhile: not sent */
            int d = find_dev(e, c.dev);
            if (d >= 0 && !e->dev[d].power && e->map->devs[d].pwr != HP_PWR_NONE) {
                hp_log("hub: queued %s %s dropped: device is off\n", c.dev, c.cmd);
                cq_pop(e);
                continue;
            }
        }
        cq_pop(e);
        if (c.kind == 2) {
            if (e->irsend_fn) e->irsend_fn(e->irsend_ctx, c.text);
            free(c.text);
            continue;
        }
        int rc = devcmd(e, c.dev, c.cmd, c.nopre);
        if (rc < 0) hp_log("hub: queued command %s %s failed (%d)\n", c.dev, c.cmd, rc);
    }
    e->draining = false;
}

/* --------------------------------------------------------- device command */

/* Applies the tracked-state effect of a device command that went out. power: -1 keep. */
static void apply_effect(hp_actsm_t *e, int d, int power, const char *input, bool settle) {
    const hp_device_t *dv = &e->map->devs[d];
    uint32_t now = now_of(e);
    if (settle && dv->settle_ms) {
        e->dev[d].settling = true;
        e->dev[d].settle_until = now + dv->settle_ms;
    }
    if (power == 0) {
        e->dev[d].settling = false;
        cq_drop_inputs(e, e->dev[d].id);
    }
    if (power < 0 && !input) return;
    int np = power >= 0 ? power : e->dev[d].power;
    char keep[HP_MAP_NAME_MAX];
    snprintf(keep, sizeof keep, "%s", input ? input : (np ? e->dev[d].input : ""));
    set_dev(e, d, np, keep);
}

static void pc_done(void *arg, int rc) {
    hp_actpc_t *pc = arg;
    hp_actsm_t *e = pc->e;
    int d = find_dev(e, pc->dev);
    pc->used = false;
    if (e->npc) e->npc--;
    if (rc < 0) hp_log("hub: deferred command for %s failed (%d), tracked state unchanged\n", pc->dev, rc);
    else if (d >= 0) apply_effect(e, d, pc->power, pc->has_input ? pc->input : NULL, pc->settle);
    drain(e);
}

/* Device command from a client: sends through the deferrable (line-owner) path and updates the tracked
 * state once the frame is sent. A toggle code is never sent when the tracked state already is the wanted one.
 * An input on a device tracked off powers it on first (its settle time is respected), and an input alone never
 * marks the power on. While a transition runs the command is queued. */
static int devcmd(hp_actsm_t *e, const char *device, const char *command, bool nopre) {
    int d = find_dev(e, device);
    if (d < 0) return -HP_E_NOENT;
    if (!command) return -HP_E_INVAL;
    const hp_map_t *m = e->map;
    const hp_device_t *dv = &m->devs[d];
    const hp_action_t *act = NULL;
    int power = -1; /* new tracked power, -1 keep */
    const char *input = NULL; /* NULL keep */
    bool settle = false, pre = false;
    bool on = !strcmp(command, "power_on");
    if (on || !strcmp(command, "power_off")) {
        if (dv->pwr == HP_PWR_NONE) return -HP_E_INVAL;
        if (dv->pwr == HP_PWR_TOGGLE && e->dev[d].power == (on ? 1 : 0)) return 0; /* already there */
        act = dv->pwr == HP_PWR_ONOFF && !on ? &dv->pwr_off : &dv->pwr_on;
        power = on ? 1 : 0;
        settle = on;
        if (!on) input = "";
    } else if (!strncmp(command, "input_", 6)) {
        int ii = input_index(m, d, command + 6);
        if (ii < 0) return -HP_E_INVAL;
        act = &m->inputs[ii].act;
        pre = !nopre && !e->dev[d].power && dv->pwr != HP_PWR_NONE;
        if (e->dev[d].power || nopre) { /* an input to a device that is not tracked on says nothing about its power */
            input = m->inputs[ii].name;
            settle = true;
        }
    } else {
        int ci = hp_map_find_cmd(m, d, command);
        if (ci < 0) return -HP_E_INVAL;
        act = &m->cmds[ci].act;
    }
    if (e->running || (e->cq_n && !e->draining && !nopre)) { /* mid-transition (or behind queued requests): in order, later */
        hp_actcmd_t c = {.kind = 1};
        if (snprintf(c.dev, sizeof c.dev, "%s", device) >= (int)sizeof c.dev ||
            snprintf(c.cmd, sizeof c.cmd, "%s", command) >= (int)sizeof c.cmd)
            return -HP_E_INVAL;
        c.nopre = nopre;
        if (cq_push(e, &c, false) < 0) {
            hp_log("hub: device command %s %s dropped: transition in progress, queue full\n", device, command);
            return -HP_E_BUSY;
        }
        hp_log("hub: device command %s %s queued: transition in progress\n", device, command);
        return 0;
    }
    if (pre) { /* power it on, then the input once it has settled */
        hp_actcmd_t c = {.kind = 1, .nopre = true};
        snprintf(c.dev, sizeof c.dev, "%s", device);
        snprintf(c.cmd, sizeof c.cmd, "%s", command);
        if (cq_push(e, &c, true) < 0) return -HP_E_BUSY;
        int rc = devcmd(e, device, "power_on", true);
        if (rc < 0) { /* nothing was sent: drop the input we queued */
            cq_pop(e);
            return rc;
        }
        drain(e);
        return 0;
    }
    hp_actpc_t *pc = NULL;
    if (power >= 0 || input) {
        for (uint8_t i = 0; i < HP_IROUT_PEND && !pc; i++)
            if (!e->pc[i].used) pc = &e->pc[i];
        if (!pc) return -HP_E_BUSY;
        *pc = (hp_actpc_t){.e = e, .used = true, .has_input = input != NULL, .settle = settle, .power = (int8_t)power};
        snprintf(pc->dev, sizeof pc->dev, "%s", e->dev[d].id);
        if (input) snprintf(pc->input, sizeof pc->input, "%s", input);
        e->npc++;
    }
    int rc = hp_map_apply_deferrable_cb(m, act, e->sinks, pc ? pc_done : NULL, pc);
    if (pc && rc != 1) { /* sent now, or refused: no callback will come */
        pc->used = false;
        e->npc--;
    }
    if (rc < 0) return rc;
    if (rc == 0 && (power >= 0 || input)) apply_effect(e, d, power, input, settle);
    return 0;
}

int hp_actsm_device_command(hp_actsm_t *e, const char *device, const char *command) {
    return devcmd(e, device, command, false);
}

/* --------------------------------------------------------------- reload */

void hp_actsm_reloaded(hp_actsm_t *e) {
    hp_map_t *m = e->map;
    if (e->have_hash && e->loaded_hash == m->hash) {
        /* nothing changed: a running transition goes on; its steps only re-point at the reloaded tables */
        if (e->running && m->format == 2) {
            const hp_activity_t *ta = e->target >= 0 ? &m->acts[e->target] : NULL;
            int pv = resolve_key(e, e->prev_key);
            const hp_activity_t *pa = e->target < 0 && pv >= 0 ? &m->acts[pv] : NULL;
            uint8_t nx = 0, no = 0;
            for (uint16_t i = 0; i < e->nplan; i++) { /* same steps, pointers into the reloaded tables */
                hp_actstep_t *st = &e->plan[i];
                if (st->kind == HP_STEP_EXTRA && ta && nx < ta->nsteps) st->act = &ta->steps[nx++];
                else if (st->kind == HP_STEP_OFFX && pa && no < pa->noff) st->act = &pa->off[no++];
            }
        }
        return;
    }
    e->have_hash = true;
    e->loaded_hash = m->hash;
    bool was_running = e->running;
    bool rs = e->resync;
    char tkey[HP_MAP_NAME_MAX], pkey[HP_MAP_NAME_MAX];
    snprintf(tkey, sizeof tkey, "%s", e->tgt_key);
    snprintf(pkey, sizeof pkey, "%s", e->prev_key);
    hp_sched_cancel(e->sched, e->timer); /* the plan is rebuilt below when its target survives */
    e->timer = 0;
    hp_sched_cancel(e->sched, e->kq_h);
    e->kq_h = 0;
    e->running = false;
    e->resync = false;
    e->nplan = e->pos = 0;
    hp_devstate_t *old = malloc(sizeof e->dev); /* up to 255 devices (~18 KiB): not for the stack */
    uint8_t nold = old ? e->ndev : 0;           /* no memory: the tracked state restarts, as for new devices */
    if (old) memcpy(old, e->dev, sizeof e->dev);
    bool changed = e->map->hash != e->saved_hash;
    memset(e->dev, 0, sizeof e->dev);
    e->ndev = m->format == 2 ? m->ndevs : 0;
    bool push[HP_MAP_MAX_DEVICES] = {false};
    for (uint8_t i = 0; i < e->ndev; i++) {
        snprintf(e->dev[i].id, sizeof e->dev[i].id, "%s", m->devs[i].id);
        push[i] = true; /* new to the tracker: clients need its state; unchanged devices stay quiet */
        for (uint8_t j = 0; j < nold; j++) {
            if (strcmp(old[j].id, e->dev[i].id)) continue;
            e->dev[i].power = old[j].power;
            e->dev[i].settling = old[j].settling;
            e->dev[i].settle_until = old[j].settle_until;
            if (input_index(m, i, old[j].input) >= 0) strcpy(e->dev[i].input, old[j].input);
            push[i] = e->dev[i].power != old[j].power || strcmp(e->dev[i].input, old[j].input);
        }
        changed = changed || push[i];
    }
    changed = changed || nold != e->ndev;
    free(old);
    /* The last COMPLETED activity survives (re-resolved by identity); a transition cut short by
     * this reload is never reported before it completes: map->current already holds its unfinished target. */
    int prev = e->reported;
    set_reported(e, resolve_reported(e));
    m->current = e->reported;
    e->target = e->reported;
    for (uint8_t i = 0; i < e->ndev; i++)
        if (push[i]) emit(e, HP_ACTSM_EVT_DEVICE, i);
    if (prev != e->reported) emit(e, HP_ACTSM_EVT_ACTIVITY, e->reported);
    if (changed || prev != e->reported) mark_dirty(e);
    if (!was_running) return;
    /* restart the plan for the surviving target (from the tracked state); a target the new config lacks is cancelled */
    int t = resolve_key(e, tkey);
    if (m->format != 2 || (tkey[0] && t < 0)) {
        hp_map_keyq_drop(m);
        drain(e); /* nothing runs any more: the queued commands go out */
        return;
    }
    hp_log("hub: config reloaded mid-transition: restarting the plan\n");
    (void)begin(e, t, resolve_key(e, pkey), rs && t >= 0 && t == e->reported);
}

static int op_start(void *c, int idx) { return hp_actsm_start_idx(c, idx); }
static int op_off(void *c) { return hp_actsm_off(c); }
static bool op_busy(void *c) { return hp_actsm_busy(c); }

void hp_actsm_init(hp_actsm_t *e, hp_map_t *m, hp_sched_t *s, const hp_sinks_t *sinks, hp_actsm_notify_fn fn,
                   void *ctx) {
    memset(e, 0, sizeof *e);
    e->map = m;
    e->sched = s;
    e->sinks = sinks;
    e->notify = fn;
    e->notify_ctx = ctx;
    set_reported(e, m->current);
    e->target = m->current;
    hp_map_set_actops(m, &(hp_map_actops_t){e, op_start, op_off, op_busy});
}
