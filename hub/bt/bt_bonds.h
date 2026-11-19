#ifndef HH_BT_BONDS_H
#define HH_BT_BONDS_H

/* Bond file safety, run before BTstack opens its TLV file (no BTstack dependency).
 * The file is rewritten at every start: valid records only (a truncated or garbled tail is
 * dropped, the records before it kept), last value per tag, deleted tags removed. It is written
 * to PATH.tmp, fsync'd and renamed over PATH; the previous good file is kept as PATH.bak. If PATH
 * is missing or invalid (bad header) PATH.bak is used instead.
 * Returns 1 when a file was rewritten, 0 when there was nothing to load, -1 on a write error. */
int bt_bonds_prepare(const char *path);

#endif
