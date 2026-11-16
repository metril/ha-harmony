#ifndef HP_ENGINE_ATOMIC_FILE_H
#define HP_ENGINE_ATOMIC_FILE_H

#include <stddef.h>
#include <stdint.h>

/* Crash-safe replace: writes to "<path>.tmp.<pid>" in the same directory, fsyncs it, rename(2)s it
 * over `path`, then best-effort fsyncs the directory. 0, or -HP_E_INVAL (bad args) / -HP_E_IO.
 * On failure the temp file is removed and `path` is untouched. */
int hp_atomic_write(const char *path, const uint8_t *data, size_t len);

#endif
