#ifndef HH_BT_LEADDR_H
#define HH_BT_LEADDR_H
/* The helper's LE address: a random STATIC address (not the controller's public BD_ADDR, which Classic hosts know), made once and
 * kept in <bt dir>/le_addr (6 bytes, display order) so LE bonds still reconnect after every start. Pure file/RNG code, no stack. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* random static: the two top bits of the first byte are 11 and the rest is neither all zeros nor all ones */
bool bt_leaddr_valid(const uint8_t a[6]);
/* fills a from rnd (NULL: /dev/urandom); false when the source fails */
bool bt_leaddr_generate(uint8_t a[6], bool (*rnd)(uint8_t *buf, size_t n));
/* loads path, or (missing / invalid) generates and writes it atomically (tmp + fsync + rename).
 * Returns 0 loaded, 1 created, -1 when it could not be stored or generated (a must-persist address: do not use a) */
int bt_leaddr_load_or_create(const char *path, uint8_t a[6], bool (*rnd)(uint8_t *buf, size_t n));

#endif
