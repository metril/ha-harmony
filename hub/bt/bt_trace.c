#include "bt_trace.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "hci_dump.h"

size_t bt_trace_format(char *out, size_t n, uint32_t ms, int in, uint8_t type, const uint8_t *pkt, uint16_t len) {
    int o = snprintf(out, n, "%u %c type=%u len=%u", (unsigned)ms, in ? '<' : '>', (unsigned)type, (unsigned)len);
    for (uint16_t i = 0; i < len && i < BT_TRACE_BYTES && o >= 0 && (size_t)o + 4 < n; i++) o += snprintf(out + o, n - (size_t)o, " %02x", pkt[i]);
    if (len > BT_TRACE_BYTES && o >= 0 && (size_t)o + 5 < n) o += snprintf(out + o, n - (size_t)o, " ...");
    return o < 0 ? 0 : (size_t)o;
}

static FILE *s_f;
static size_t s_cap, s_bytes;
static const char *s_path;

static uint32_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}

static void log_packet(uint8_t type, uint8_t in, uint8_t *pkt, uint16_t len) {
    if (!s_f) return;
    char line[BT_TRACE_BYTES * 3 + 64];
    size_t n = bt_trace_format(line, sizeof line - 1, now_ms(), in, type, pkt, len);
    line[n++] = '\n';
    if (s_bytes + n > s_cap) { /* full: start the file again */
        s_f = freopen(s_path, "w", s_f);
        s_bytes = 0;
        if (!s_f) return;
    }
    fwrite(line, 1, n, s_f);
    fflush(s_f);
    s_bytes += n;
}

static void log_message(int level, const char *fmt, va_list ap) { (void)level; (void)fmt; (void)ap; }
static void reset_noop(void) {}

static const hci_dump_t s_impl = {.reset = reset_noop, .log_packet = log_packet, .log_message = log_message};

int bt_trace_start(const char *path, size_t cap) {
    bt_trace_stop();
    s_f = fopen(path, "w");
    if (!s_f) return -1;
    s_path = path;
    s_cap = cap;
    s_bytes = 0;
    hci_dump_init(&s_impl);
    hci_dump_enable_packet_log(true);
    return 0;
}

void bt_trace_stop(void) {
    if (s_f) fclose(s_f);
    s_f = NULL;
}
