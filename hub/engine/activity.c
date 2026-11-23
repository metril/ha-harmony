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
    if (idx >= 0 && idx < m->nacts)
        snprintf(e->rep_key, sizeof e->rep_key, "%s", m->format == 2 ? m->acts[idx].id : m->acts[idx].name);
}

static int resolve_reported(const hp_actsm_t *e) {
    const hp_map_t *m = e->map;
    for (int i = 0; e->rep_key[0] && i < m->nacts; i++)
        if (!strcmp(e->rep_key, m->format == 2 ? m->acts[i].id : m->acts[i].name)) return i;
    return -1;
}

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
    if (e->dirty && e->state_path) (void)write_state(e);
}

/* Debounce: the first change after a quiet period of HP_ACT_PERSIST_MS writes at once; changes
 * inside the window coalesce into one trailing write at its end. */
static void mark_dirty(hp_actsm_t *e) {
    e->dirty = true;
    if (!e->state_path || e->persist_h) return;
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
    if (memcmp(buf, "HPST", 4) || buf[4] != HP_ACT_STATE_VER || get32(buf + n) != hp_map_crc32(buf, n) ||
        get32(buf + 5) != e->map->hash)
        return -HP_E_INVAL;
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
    if (*act && idx < 0) return -HP_E_INVAL;
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
    e->saved_hash = e->map->hash;
    e->dirty = false;
    return 0;
}

/* ------------------------------------------------------------------ plan */

static void cancel_plan(hp_actsm_t *e) {
    hp_sched_cancel(e->sched, e->timer);
    e->timer = 0;
    e->running = false;
    e->resync = false;
    e->nplan = e->pos = 0;
}

static void add(hp_actsm_t *e, hp_stepkind_t k, int dev, int input, const hp_action_t *act) {
    if (e->nplan >= HP_ACT_PLAN_MAX) return; /* sized for the worst case; unreachable */
    e->plan[e->nplan++] = (hp_actstep_t){(uint8_t)k, (uint8_t)dev, (int32_t)input, act};
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
    /* resync: discrete power / input codes are idempotent, so resend them whatever the tracked state says
     * (it may be stale); toggle power would flip the device and stays skipped by tracked state */
    bool force = e->resync && s->kind != HP_STEP_EXTRA && s->kind != HP_STEP_OFFX &&
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

static void arm(hp_actsm_t *e, uint32_t delay);

static void finish(hp_actsm_t *e) {
    e->running = false;
    e->resync = false;
    e->nplan = e->pos = 0;
    set_reported(e, e->target);
    e->map->current = e->target;
    emit(e, HP_ACTSM_EVT_ACTIVITY, e->reported);
    mark_dirty(e);
}

static void abort_plan(hp_actsm_t *e, int rc) {
    hp_log("hub: activity transition aborted (%d)\n", rc);
    cancel_plan(e);
    e->map->current = e->reported;
}

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
        if (delay) {
            arm(e, delay);
            return 0;
        }
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
            e->dev[s->dev].settling = dv->settle_ms > 0;
            e->dev[s->dev].settle_until = now + dv->settle_ms;
            set_dev(e, s->dev, 1, "");
            break;
        case HP_STEP_PWR_OFF:
            e->dev[s->dev].settling = false;
            set_dev(e, s->dev, 0, "");
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

static void arm(hp_actsm_t *e, uint32_t delay) {
    e->timer = hp_sched_after(e->sched, delay, step_cb, e);
    if (!e->timer) abort_plan(e, -HP_E_BUSY); /* timer queue full */
}

/* ------------------------------------------------------------- requests */

static int begin(hp_actsm_t *e, int target) {
    e->target = target;
    e->map->current = target;
    e->running = true;
    int rc = run(e);
    return rc < 0 ? rc : 0;
}

int hp_actsm_start_idx(hp_actsm_t *e, int idx) {
    hp_map_t *m = e->map;
    if (idx < 0 || idx >= m->nacts) return -HP_E_INVAL;
    emit(e, HP_ACTSM_EVT_START, idx);
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
    const hp_activity_t *a = &m->acts[idx];
    bool resync = e->reported == idx && m->current == idx;
    if (resync) hp_log("hub: activity %s re-pressed: resync (resending discrete steps)\n", a->name);
    bool used[HP_MAP_MAX_DEVICES] = {false};
    for (uint8_t i = 0; i < a->ndevs; i++) used[a->devs[i].dev] = true;
    for (int d = 0; d < e->ndev; d++)
        if (!used[d]) add(e, HP_STEP_PWR_OFF, d, -1, NULL);
    for (uint8_t i = 0; i < a->ndevs; i++) add(e, HP_STEP_PWR_ON, a->devs[i].dev, -1, NULL);
    for (uint8_t i = 0; i < a->ndevs; i++)
        if (a->devs[i].input >= 0) add(e, HP_STEP_INPUT, a->devs[i].dev, a->devs[i].input, NULL);
    for (uint8_t i = 0; i < a->nsteps; i++) add(e, HP_STEP_EXTRA, 0, -1, &a->steps[i]);
    e->resync = resync;
    int rc = begin(e, idx);
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
        emit(e, HP_ACTSM_EVT_ACTIVITY, -1);
        mark_dirty(e);
        return rc;
    }
    int prev = m->current; /* the activity (or interrupted target) whose off steps run */
    bool anyon = false;
    for (int d = 0; d < e->ndev; d++) anyon = anyon || e->dev[d].power;
    if (prev < 0 && !anyon) return -HP_E_INVAL;
    cancel_plan(e);
    hp_irout_cancel(m->out);
    hp_map_bt_release(m, e->sinks);
    for (int d = 0; d < e->ndev; d++) add(e, HP_STEP_PWR_OFF, d, -1, NULL);
    if (prev >= 0 && prev < m->nacts)
        for (uint8_t i = 0; i < m->acts[prev].noff; i++) add(e, HP_STEP_OFFX, 0, -1, &m->acts[prev].off[i]);
    int rc = begin(e, -1);
    return rc < 0 ? rc : (prev < 0 ? 0 : prev);
}

bool hp_actsm_busy(const hp_actsm_t *e) { return e->running; }
int hp_actsm_reported(const hp_actsm_t *e) { return e->reported; }

const hp_devstate_t *hp_actsm_device(const hp_actsm_t *e, size_t i) { return i < e->ndev ? &e->dev[i] : NULL; }

int hp_actsm_set_device(hp_actsm_t *e, const char *device, int power, const char *input) {
    int d = -1;
    for (int i = 0; device && i < e->ndev; i++)
        if (!strcmp(e->dev[i].id, device)) d = i;
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

/* Device command from a client: sends through the deferrable (line-owner) path and updates the
 * tracked state. A toggle code is never sent when the tracked state already is the wanted one. */
int hp_actsm_device_command(hp_actsm_t *e, const char *device, const char *command) {
    int d = -1;
    for (int i = 0; device && i < e->ndev; i++)
        if (!strcmp(e->dev[i].id, device)) d = i;
    if (d < 0) return -HP_E_NOENT;
    if (!command) return -HP_E_INVAL;
    if (e->running) return -HP_E_BUSY;
    const hp_map_t *m = e->map;
    const hp_device_t *dv = &m->devs[d];
    const hp_action_t *act = NULL;
    int power = -1; /* new tracked power, -1 keep */
    const char *input = NULL; /* NULL keep */
    bool on = !strcmp(command, "power_on");
    if (on || !strcmp(command, "power_off")) {
        if (dv->pwr == HP_PWR_NONE) return -HP_E_INVAL;
        if (dv->pwr == HP_PWR_TOGGLE && e->dev[d].power == (on ? 1 : 0)) return 0; /* already there */
        act = dv->pwr == HP_PWR_ONOFF && !on ? &dv->pwr_off : &dv->pwr_on;
        power = on ? 1 : 0;
        if (!on) input = "";
    } else if (!strncmp(command, "input_", 6)) {
        int ii = input_index(m, d, command + 6);
        if (ii < 0) return -HP_E_INVAL;
        act = &m->inputs[ii].act;
        power = 1;
        input = m->inputs[ii].name;
    } else {
        int ci = hp_map_find_cmd(m, d, command);
        if (ci < 0) return -HP_E_INVAL;
        act = &m->cmds[ci].act;
    }
    int rc = hp_map_apply_deferrable(m, act, e->sinks);
    if (rc < 0) return rc;
    uint32_t now = now_of(e);
    if (power == 1 && dv->settle_ms) {
        e->dev[d].settling = true;
        e->dev[d].settle_until = now + dv->settle_ms;
    }
    if (power == 0) e->dev[d].settling = false;
    if (power >= 0) {
        char keep[HP_MAP_NAME_MAX];
        snprintf(keep, sizeof keep, "%s", input ? input : (power ? e->dev[d].input : ""));
        set_dev(e, d, power, keep);
    }
    return 0;
}

/* --------------------------------------------------------------- reload */

void hp_actsm_reloaded(hp_actsm_t *e) {
    hp_map_t *m = e->map;
    cancel_plan(e);
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
     * this reload is never reported: map->current already holds its unfinished target. */
    int prev = e->reported;
    set_reported(e, resolve_reported(e));
    m->current = e->reported;
    e->target = e->reported;
    for (uint8_t i = 0; i < e->ndev; i++)
        if (push[i]) emit(e, HP_ACTSM_EVT_DEVICE, i);
    if (prev != e->reported) emit(e, HP_ACTSM_EVT_ACTIVITY, e->reported);
    if (changed || prev != e->reported) mark_dirty(e);
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
