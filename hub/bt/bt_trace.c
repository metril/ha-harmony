#include "bt_trace.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "hci_dump.h"

/* first byte offset that carries key material (len when none) */
static uint16_t redact_from(uint8_t type, const uint8_t *p, uint16_t len) {
    if (type == 1 && len >= 3) { /* command: opcode, plen, params */
        switch ((uint16_t)(p[0] | (p[1] << 8))) {
        case 0x040B: return 9; /* Link Key Request Reply: addr, key */
        case 0x2019: return 5; /* LE Start Encryption: handle, rand, ediv, ltk */
        case 0x201A: return 5; /* LE LTK Request Reply: handle, ltk */
        case 0x2017: return 3; /* LE Encrypt: key, plaintext */
        }
    } else if (type == 4 && len >= 2) { /* event */
        if (p[0] == 0x18) return 8; /* Link Key Notification: addr, key, type */
        if (p[0] == 0x0E && len >= 6 && (p[3] | (p[4] << 8)) == 0x2017) return 6; /* LE Encrypt result */
    } else if (type == 2 && len >= 9 && (p[6] | (p[7] << 8)) == 0x0006) { /* ACL, L2CAP CID 6 = SMP */
        switch (p[8]) {
        case 0x03: case 0x04: case 0x06: case 0x07: case 0x08: case 0x0A: case 0x0D: return 9; /* confirm, random, enc/id/signing info, master id, DH key check */
        }
    }
    return len;
}

size_t bt_trace_format(char *out, size_t n, uint32_t ms, int in, uint8_t type, const uint8_t *pkt, uint16_t len) {
    int o = snprintf(out, n, "%u %c type=%u len=%u", (unsigned)ms, in ? '<' : '>', (unsigned)type, (unsigned)len);
    uint16_t red = redact_from(type, pkt, len);
    for (uint16_t i = 0; i < len && i < BT_TRACE_BYTES && o >= 0 && (size_t)o + 4 < n; i++)
        o += i >= red ? snprintf(out + o, n - (size_t)o, " **") : snprintf(out + o, n - (size_t)o, " %02x", pkt[i]);
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

/* owner-only, created fresh (never follows an old file's mode) */
static FILE *open_file(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "w");
    if (!f) close(fd);
    return f;
}

static void log_packet(uint8_t type, uint8_t in, uint8_t *pkt, uint16_t len) {
    if (!s_f) return;
    char line[BT_TRACE_BYTES * 3 + 64];
    size_t n = bt_trace_format(line, sizeof line - 1, now_ms(), in, type, pkt, len);
    line[n++] = '\n';
    if (s_bytes + n > s_cap) { /* full: start the file again */
        fclose(s_f);
        s_f = open_file(s_path);
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
    s_f = open_file(path);
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
