#include "hub/io/sim_sinks.h"

#include <string.h>

#include "hub/hp_errors.h"

static hp_sim_rec_t *next_rec(hp_sim_t *s) {
    hp_sim_rec_t *r = &s->ring[s->total % HP_SIM_RING];
    s->total++;
    return r;
}

static int sim_ir_send(void *self, uint8_t port_mask, const uint16_t *pronto, size_t n) {
    hp_sim_t *s = self;
    if (!pronto || n == 0 || n > HP_SIM_MAX_PRONTO) return -HP_E_INVAL;
    hp_sim_rec_t *r = next_rec(s);
    r->kind = HP_SIM_IR;
    r->port_mask = port_mask;
    r->n = n;
    memcpy(r->pronto, pronto, n * sizeof *pronto);
    if (s->mirror) {
        fprintf(s->mirror, "ir port=%u n=%zu pronto=", (unsigned)port_mask, n);
        for (size_t i = 0; i < n; i++) fprintf(s->mirror, i ? " %04x" : "%04x", (unsigned)pronto[i]);
        fputc('\n', s->mirror);
        fflush(s->mirror);
    }
    return 0;
}

static int sim_bt_key(void *self, uint16_t usage, bool down) {
    hp_sim_t *s = self;
    hp_sim_rec_t *r = next_rec(s);
    r->kind = HP_SIM_BT;
    r->usage = usage;
    r->down = down ? 1 : 0;
    if (s->mirror) {
        fprintf(s->mirror, "bt usage=%04x down=%d\n", (unsigned)usage, r->down);
        fflush(s->mirror);
    }
    return 0;
}

static int sim_rf_send(void *self, const uint8_t *frame, size_t len) {
    (void)self; (void)frame; (void)len;
    return 0; /* sim RF has no peer yet */
}

static int sim_rf_recv(void *self, uint8_t *frame, size_t cap, int timeout_ms) {
    hp_sim_t *s = self;
    (void)timeout_ms; /* scripted: never blocks */
    if (!s->rf_claimed) return -HP_E_INVAL;
    if (s->rfq_count == 0) return 0;
    size_t len = s->rfq_len[s->rfq_head];
    if (len > cap) return -HP_E_TOOBIG;
    memcpy(frame, s->rfq[s->rfq_head], len);
    s->rfq_head = (s->rfq_head + 1) % HP_SIM_RF_QUEUE;
    s->rfq_count--;
    return (int)len;
}

static int sim_rf_claim(void *self) {
    hp_sim_t *s = self;
    if (s->rf_claimed) return -HP_E_BUSY;
    s->rf_claimed = true;
    return 0;
}

void hp_sim_init(hp_sim_t *s, FILE *mirror) {
    memset(s, 0, sizeof *s);
    s->mirror = mirror;
    s->ir = (hp_ir_sink){s, sim_ir_send};
    s->bt = (hp_bt_sink){s, sim_bt_key};
    s->rf = (hp_rf_transport){s, sim_rf_send, sim_rf_recv, sim_rf_claim};
}

hp_sinks_t hp_sim_sinks(hp_sim_t *s) {
    return (hp_sinks_t){&s->ir, &s->bt, &s->rf};
}

size_t hp_sim_count(const hp_sim_t *s) { return s->total; }

const hp_sim_rec_t *hp_sim_rec(const hp_sim_t *s, size_t index) {
    if (index >= s->total || s->total - index > HP_SIM_RING) return NULL;
    return &s->ring[index % HP_SIM_RING];
}

int hp_sim_rf_script(hp_sim_t *s, const uint8_t *frame, size_t len) {
    if (len > HP_SIM_RF_MAX || (len && !frame)) return -HP_E_TOOBIG;
    if (s->rfq_count == HP_SIM_RF_QUEUE) return -HP_E_BUSY;
    size_t slot = (s->rfq_head + s->rfq_count) % HP_SIM_RF_QUEUE;
    memcpy(s->rfq[slot], frame, len);
    s->rfq_len[slot] = len;
    s->rfq_count++;
    return 0;
}
