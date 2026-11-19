#ifndef HP_NET_IDENT_H
#define HP_NET_IDENT_H

/* The hub's identity: id, display name, stable hardware id (docs/proto/hub-identity.md).
 *
 * The id is chosen, highest first, from: the --hub-id flag (hub.conf `hub_id`: PINNED, the daemon never
 * changes it), the `hub_id` file in the daemon's own state directory (written by set_hub_id), the default
 * (the hostname-derived id). The display name is the `hub_name` file there. Both files are replaced
 * atomically; hub.conf is never written. */

#include <stdbool.h>
#include <stddef.h>

#define HP_ID_MAX 63   /* one DNS label */
#define HP_NAME_MAX 48 /* bytes of UTF-8 */
#define HP_HW_MAX 31

typedef struct {
    char dir[256];            /* "" = nothing is persisted (a change lives until restart) */
    char id[HP_ID_MAX + 1];
    char name[HP_NAME_MAX + 1];
    char hw[HP_HW_MAX + 1];   /* "" = unknown */
    bool pinned;              /* the id came from --hub-id */
    char warn[160];           /* "" or why a configured/derived id was replaced (log it) */
} hp_ident_t;

/* NEW ids: [A-Za-z0-9_-], 1..63 chars, not "default" (the add-on's placeholder id); hp_ident_set additionally
 * wants a letter or digit first. No '.': it is not valid in an mDNS label. */
bool hp_ident_id_valid(const char *id);
/* Ids already in use may still contain '.' (older hub.conf / saved files): accepted as they are. */
bool hp_ident_id_existing_ok(const char *id);
/* The mDNS label for an id: '.' becomes '-'. */
void hp_ident_mdns_label(const char *id, char out[HP_ID_MAX + 1]);
/* UTF-8, <= 48 bytes, no control characters; "" is valid (no name). */
bool hp_ident_name_valid(const char *name);

/* flag_id: --hub-id (NULL = none), def: the default id, hw: the hardware id or NULL, dir: state dir or NULL.
 * An unusable flag/file is ignored (the next source wins; a bad flag id means not pinned); an unusable default is
 * sanitised (bad chars -> '-', cut to 63, never "default"). Never fails: always 0, with `warn` set when it fell back. */
int hp_ident_init(hp_ident_t *, const char *dir, const char *flag_id, const char *def, const char *hw);

/* Result codes of hp_ident_set. */
enum { HP_IDENT_OK = 0, HP_IDENT_BAD_ID = 1, HP_IDENT_BAD_NAME = 2, HP_IDENT_PINNED = 3, HP_IDENT_IO = 4 };
/* Sets the id (the current one keeps it) and, when `name` is not NULL, the name ("" clears it). Everything is
 * validated first and persisted atomically before the live values change: on any non-OK result nothing has
 * changed (a failed second file restores the first). Setting a pinned hub to a different id is refused. */
int hp_ident_set(hp_ident_t *, const char *id, const char *name);

/* Best-effort stable hardware id from the first usable network interface MAC ("" = none): 12 lowercase hex.
 * `root` is the sysfs net directory (NULL = /sys/class/net); tests pass a fixture. */
void hp_ident_hw_read(const char *root, char out[HP_HW_MAX + 1]);

#endif
