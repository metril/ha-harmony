#define _POSIX_C_SOURCE 200809L
#include "hub/net/ident.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "hub/engine/atomic_file.h"
#include "hub/hp_errors.h"
#include "hub/net/ws.h"

static bool id_chars_ok(const char *id, bool dots) {
    if (!id) return false;
    size_t n = strlen(id);
    if (n == 0 || n > HP_ID_MAX || !strcmp(id, "default")) return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (dots && c == '.') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

bool hp_ident_id_valid(const char *id) { return id_chars_ok(id, false); }
bool hp_ident_id_existing_ok(const char *id) { return id_chars_ok(id, true); }

void hp_ident_mdns_label(const char *id, char out[HP_ID_MAX + 1]) {
    size_t i = 0;
    for (; id[i] && i < HP_ID_MAX; i++) out[i] = id[i] == '.' ? '-' : id[i];
    out[i] = 0;
}

/* The hostname-derived default made safe: bad characters become '-', cut to one label; never empty or "default". */
static void derive_default(const char *def, char out[HP_ID_MAX + 1]) {
    size_t n = 0;
    for (; def && def[n] && n < HP_ID_MAX; n++) {
        char c = def[n];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        out[n] = ok ? c : '-';
    }
    out[n] = 0;
    if (n == 0 || !strcmp(out, "default")) snprintf(out, HP_ID_MAX + 1, "ha-harmony-hub");
}

bool hp_ident_name_valid(const char *name) {
    if (!name) return false;
    size_t n = strlen(name);
    if (n > HP_NAME_MAX || !hp_utf8_valid((const uint8_t *)name, n)) return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)name[i] < 0x20 || (unsigned char)name[i] == 0x7f) return false;
    return true;
}

static bool path_of(const hp_ident_t *t, const char *file, char *out, size_t cap) {
    if (!t->dir[0]) return false;
    int n = snprintf(out, cap, "%s/%s", t->dir, file);
    return n > 0 && (size_t)n < cap;
}

/* Reads a one-line file into out (no newline); false when missing or too long. */
static bool read_line(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(out, 1, cap - 1, f);
    bool more = fgetc(f) != EOF;
    fclose(f);
    if (more) return false;
    out[n] = 0;
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    return true;
}

static int write_file(const char *path, const char *s) {
    return hp_atomic_write(path, (const uint8_t *)s, strlen(s)) < 0 ? -1 : 0;
}

int hp_ident_init(hp_ident_t *t, const char *dir, const char *flag_id, const char *def, const char *hw) {
    memset(t, 0, sizeof *t);
    if (dir && strlen(dir) < sizeof t->dir) snprintf(t->dir, sizeof t->dir, "%s", dir);
    if (hw && strlen(hw) <= HP_HW_MAX) snprintf(t->hw, sizeof t->hw, "%s", hw);
    char p[300], buf[160];
    if (flag_id && *flag_id) {
        if (hp_ident_id_existing_ok(flag_id)) {
            snprintf(t->id, sizeof t->id, "%s", flag_id);
            t->pinned = true;
        } else {
            snprintf(t->warn, sizeof t->warn, "hub_id in hub.conf is not usable (1-63 of A-Z a-z 0-9 _ -, not \"default\"): ignored, treated as not pinned");
        }
    }
    if (!t->id[0]) {
        if (path_of(t, "hub_id", p, sizeof p) && read_line(p, buf, sizeof buf) && hp_ident_id_existing_ok(buf)) memcpy(t->id, buf, strlen(buf) + 1);
        else if (def && hp_ident_id_valid(def)) snprintf(t->id, sizeof t->id, "%s", def);
        else {
            derive_default(def, t->id);
            if (!t->warn[0]) snprintf(t->warn, sizeof t->warn, "the hostname-derived id is not a valid id: using %s", t->id);
        }
    }
    if (path_of(t, "hub_name", p, sizeof p) && read_line(p, buf, sizeof buf) && hp_ident_name_valid(buf)) memcpy(t->name, buf, strlen(buf) + 1);
    return 0;
}

int hp_ident_set(hp_ident_t *t, const char *id, const char *name) {
    /* a new id must also start with a letter or digit: the add-on keys its store files by it */
    if (!hp_ident_id_valid(id) || !((id[0] >= 'a' && id[0] <= 'z') || (id[0] >= 'A' && id[0] <= 'Z') || (id[0] >= '0' && id[0] <= '9'))) return HP_IDENT_BAD_ID;
    if (name && !hp_ident_name_valid(name)) return HP_IDENT_BAD_NAME;
    bool id_change = strcmp(id, t->id) != 0;
    if (id_change && t->pinned) return HP_IDENT_PINNED;
    bool name_change = name && strcmp(name, t->name) != 0;
    char idp[300] = "", np[300] = "";
    bool persist = path_of(t, "hub_id", idp, sizeof idp) && path_of(t, "hub_name", np, sizeof np);
    if (persist && id_change && write_file(idp, id) < 0) return HP_IDENT_IO;
    if (persist && name_change) {
        int rc = name[0] ? write_file(np, name) : ((unlink(np) == 0 || access(np, F_OK) != 0) ? 0 : -1);
        if (rc < 0) {
            if (id_change) (void)write_file(idp, t->id); /* put the first file back */
            return HP_IDENT_IO;
        }
    }
    if (id_change) snprintf(t->id, sizeof t->id, "%s", id);
    if (name_change) snprintf(t->name, sizeof t->name, "%s", name);
    return HP_IDENT_OK;
}

void hp_ident_hw_read(const char *root, char out[HP_HW_MAX + 1]) {
    static const char *const ifs[] = {"wlan0", "eth0", "wlan1", "eth1", "br0", "ath0"};
    out[0] = 0;
    if (!root) root = "/sys/class/net";
    for (size_t i = 0; i < sizeof ifs / sizeof ifs[0]; i++) {
        char p[300], mac[64];
        snprintf(p, sizeof p, "%s/%s/address", root, ifs[i]);
        if (!read_line(p, mac, sizeof mac)) continue;
        char hex[13];
        size_t k = 0;
        bool ok = true, nonzero = false;
        for (const char *c = mac; *c; c++) {
            if (*c == ':') continue;
            char l = (*c >= 'A' && *c <= 'F') ? (char)(*c + 32) : *c;
            if (!((l >= '0' && l <= '9') || (l >= 'a' && l <= 'f')) || k >= 12) {
                ok = false;
                break;
            }
            if (l != '0') nonzero = true;
            hex[k++] = l;
        }
        if (ok && k == 12 && nonzero) {
            hex[12] = 0;
            snprintf(out, HP_HW_MAX + 1, "%s", hex);
            return;
        }
    }
}
