#ifndef HH_BT_TRACE_H
#define HH_BT_TRACE_H
/* Optional HCI packet trace for bench diagnosis: one text line per packet in a size-capped file (never in flash).
 * Enabled by the existence of <bt dir>/hci_trace when the helper starts; written to BT_TRACE_FILE. */
#include <stddef.h>
#include <stdint.h>

#define BT_TRACE_FILE "/tmp/bt-hci.log"
#define BT_TRACE_CAP (512u * 1024u) /* the file restarts when it would exceed this */
#define BT_TRACE_BYTES 48           /* packet bytes shown per line */

/* "<ms> <|> type=<n> len=<n> <hex>" (< = from the controller); returns the line length (without the newline).
 * Key material is shown as "**": link keys, LE LTK / start encryption / encrypt, and SMP key-distribution and confirm/random/DH-check PDUs. */
size_t bt_trace_format(char *out, size_t n, uint32_t ms, int in, uint8_t type, const uint8_t *pkt, uint16_t len);
/* opens path (truncating; the string must outlive the trace) and hooks BTstack's hci_dump; cap is the size limit */
int bt_trace_start(const char *path, size_t cap);
void bt_trace_stop(void);

#endif
