/* Outbound HCI packet queue for a non-blocking raw HCI socket: each packet is one datagram, so packets are kept
 * whole and written in order. The transport is "synchronous" to BTstack (it does not keep the buffer after
 * send_packet returns), so a packet that hits EAGAIN is copied here instead of being dropped or waited for.
 * Pure, no BTstack. */
#ifndef HH_BT_PKTQ_H
#define HH_BT_PKTQ_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define BT_PKTQ_SLOTS 8
#define BT_PKTQ_MAX 1032 /* packet type byte + the largest HCI packet (HCI_ACL_PAYLOAD_SIZE + header slack) */

typedef struct {
    uint8_t d[BT_PKTQ_SLOTS][BT_PKTQ_MAX];
    uint16_t len[BT_PKTQ_SLOTS];
    unsigned head, n;
} bt_pktq_t;

/* `buf` is the packet with its type byte already in front. 0 = queued, -1 = full or oversize. */
static inline int bt_pktq_push(bt_pktq_t *q, const uint8_t *buf, size_t len) {
    if (q->n == BT_PKTQ_SLOTS || len == 0 || len > BT_PKTQ_MAX) return -1;
    unsigned i = (q->head + q->n++) % BT_PKTQ_SLOTS;
    memcpy(q->d[i], buf, len);
    q->len[i] = (uint16_t)len;
    return 0;
}

/* Writes queued packets in order until the socket would block. 0 = ok (maybe packets left); -1 = a packet failed
 * with a hard error (errno set) and was dropped; the caller logs it and may call again. */
static inline int bt_pktq_drain(bt_pktq_t *q, int fd) {
    while (q->n) {
        ssize_t w = write(fd, q->d[q->head], q->len[q->head]);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        q->head = (q->head + 1) % BT_PKTQ_SLOTS;
        q->n--;
        if (w < 0) return -1;
    }
    return 0;
}

#endif
