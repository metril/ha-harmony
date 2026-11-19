/* Outbound byte queue for a non-blocking fd: frames are appended whole and drained in order, so a
 * full socket buffer or a short write never drops or splits a frame. Pure, no BTstack. */
#ifndef HH_BT_TXQ_H
#define HH_BT_TXQ_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint8_t *buf;
    size_t n, cap;
} bt_txq_t;

/* 0 = queued, -1 = would exceed the cap (the peer is stuck: the caller gives up) */
static inline int bt_txq_push(bt_txq_t *q, const uint8_t *frame, size_t len) {
    if (q->n + len > q->cap) return -1;
    memcpy(q->buf + q->n, frame, len);
    q->n += len;
    return 0;
}

/* writes what the fd takes; 0 = ok (maybe bytes left), -1 = the peer is gone */
static inline int bt_txq_drain(bt_txq_t *q, int fd) {
    while (q->n) {
        ssize_t w = write(fd, q->buf, q->n);
        if (w > 0) {
            memmove(q->buf, q->buf + w, q->n - (size_t)w);
            q->n -= (size_t)w;
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        } else {
            return -1;
        }
    }
    return 0;
}

#endif
