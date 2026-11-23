#define _POSIX_C_SOURCE 200809L
#include "hub/io/rf_pair.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "hub/hp_errors.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define RF_MAX 32
#define ATTACH_BYTE 'A'
#define PEND_MAX 4

typedef struct {
    hp_rf_transport t; /* first member: transport pointer == pair pointer */
    bool hub;
    bool claimed;
    int lfd; /* hub: listener */
    int fd;  /* hub: attached peer; remote: the link */
    int pend[PEND_MAX]; /* hub: connections that arrived behind a hung-up peer */
    int npend;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
} pair_t;

static int set_nonblock_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return 0;
}

static int fill_addr(struct sockaddr_un *a, const char *path) {
    if (!path || !*path || strlen(path) >= sizeof a->sun_path) return -HP_E_INVAL;
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    strcpy(a->sun_path, path);
    return 0;
}

static int xsend(int fd, const uint8_t *frame, size_t len) {
    if (len > RF_MAX) return -HP_E_TOOBIG;
    ssize_t n = send(fd, frame, len, MSG_NOSIGNAL);
    if (n < 0) return errno == EAGAIN ? -HP_E_BUSY : -HP_E_IO;
    return 0;
}

/* True when the attached peer has hung up (it is no longer "attached"; recv() must still drain
 * it and report its departure). */
static bool peer_hung_up(const pair_t *p) {
    struct pollfd f = {p->fd, 0, 0};
    return poll(&f, 1, 0) == 1;
}

/* Attaches c as the peer: the handshake byte, then it is the link. */
static bool attach_conn(pair_t *p, int c) {
    uint8_t a = ATTACH_BYTE;
    if (set_nonblock_cloexec(c) < 0 || send(c, &a, 1, MSG_NOSIGNAL) != 1) {
        close(c);
        return false;
    }
    p->fd = c;
    return true;
}

/* Accepts pending connections: the first becomes the peer (when none is attached), others are
 * closed immediately (refused). A connection made after the peer hung up must never be refused as a
 * "second remote" (the RF reconnect flake): while the dead peer still has frames to drain, it waits in
 * p->pend (FIFO) until recv() has reported the departure. The peer's state is checked AFTER accept():
 * a check before it is a race, since the remote may hang up and its successor connect in between. */
static void accept_pending(pair_t *p) {
    for (;;) {
        while (p->fd < 0 && p->npend > 0) {
            int c = p->pend[0];
            memmove(p->pend, p->pend + 1, (size_t)--p->npend * sizeof p->pend[0]);
            attach_conn(p, c);
        }
        if (p->fd >= 0 && peer_hung_up(p)) return; /* let it drain; newcomers stay queued in the kernel */
        int c = accept(p->lfd, NULL, NULL);
        if (c < 0) return;
        if (p->fd < 0) {
            attach_conn(p, c);
        } else if (peer_hung_up(p) && p->npend < PEND_MAX) {
            p->pend[p->npend++] = c;
        } else {
            close(c);
        }
    }
}

/* >0 frame, 0 nothing, -NOENT peer gone (peer closed here). */
static int peer_read(pair_t *p, uint8_t *frame, size_t cap) {
    if (p->fd < 0) return 0;
    ssize_t n = recv(p->fd, frame, cap, 0);
    if (n > 0) return (int)n;
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    if (n == 0) { /* EOF, or an empty datagram (garbage; skipped): only POLLHUP means gone */
        struct pollfd f = {p->fd, 0, 0};
        if (poll(&f, 1, 0) == 0) return 0; /* no HUP/ERR: still connected */
    }
    close(p->fd);
    p->fd = -1;
    return -HP_E_NOENT;
}

static int hub_recv(void *self, uint8_t *frame, size_t cap, int timeout_ms) {
    pair_t *p = self;
    if (!p->claimed) return -HP_E_INVAL;
    for (int pass = 0; pass < 2; pass++) {
        int r = peer_read(p, frame, cap);
        if (r) {
            if (r < 0) accept_pending(p); /* the successor attaches now: nothing else would wake the loop for it */
            return r;
        }
        accept_pending(p);
        if (pass == 0 && timeout_ms != 0) {
            struct pollfd f[2] = {{p->lfd, POLLIN, 0}, {p->fd, POLLIN, 0}};
            int rc = poll(f, p->fd >= 0 ? 2 : 1, timeout_ms);
            if (rc < 0 && errno != EINTR) return -HP_E_IO;
            if (rc <= 0) return 0;
        } else {
            break;
        }
    }
    return 0;
}

static int hub_send(void *self, const uint8_t *frame, size_t len) {
    pair_t *p = self;
    if (!p->claimed) return -HP_E_INVAL;
    accept_pending(p);
    if (p->fd < 0) return -HP_E_NOENT;
    return xsend(p->fd, frame, len);
}

static int remote_recv(void *self, uint8_t *frame, size_t cap, int timeout_ms) {
    pair_t *p = self;
    if (!p->claimed) return -HP_E_INVAL;
    struct pollfd f = {p->fd, POLLIN, 0};
    int rc = poll(&f, 1, timeout_ms);
    if (rc < 0) return errno == EINTR ? 0 : -HP_E_IO;
    if (rc == 0) return 0;
    ssize_t n = recv(p->fd, frame, cap, 0);
    if (n > 0) return (int)n;
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    return -HP_E_IO;
}

static int remote_send(void *self, const uint8_t *frame, size_t len) {
    pair_t *p = self;
    if (!p->claimed) return -HP_E_INVAL;
    return xsend(p->fd, frame, len);
}

static int pair_claim(void *self) {
    pair_t *p = self;
    if (p->claimed) return -HP_E_BUSY;
    p->claimed = true;
    return 0;
}

static bool hub_attached(void *self) {
    pair_t *p = self;
    accept_pending(p);
    return p->fd >= 0;
}

int hp_rf_pair_create(const char *path, hp_rf_transport **out) {
    struct sockaddr_un a;
    int rc = fill_addr(&a, path);
    if (rc < 0 || !out) return rc < 0 ? rc : -HP_E_INVAL;
    pair_t *p = calloc(1, sizeof *p);
    if (!p) return -HP_E_IO;
    p->hub = true;
    p->fd = -1;
    p->lfd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (p->lfd < 0 || set_nonblock_cloexec(p->lfd) < 0) goto fail;
    unlink(path);
    if (bind(p->lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(p->lfd, 4) < 0) goto fail;
    strcpy(p->path, path);
    p->t = (hp_rf_transport){p, hub_send, hub_recv, pair_claim, hub_attached, NULL, NULL};
    *out = &p->t;
    return 0;
fail:
    if (p->lfd >= 0) close(p->lfd);
    free(p);
    return -HP_E_IO;
}

int hp_rf_pair_connect(const char *path, hp_rf_transport **out) {
    struct sockaddr_un a;
    int rc = fill_addr(&a, path);
    if (rc < 0 || !out) return rc < 0 ? rc : -HP_E_INVAL;
    pair_t *p = calloc(1, sizeof *p);
    if (!p) return -HP_E_IO;
    p->lfd = -1;
    p->fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (p->fd < 0 || connect(p->fd, (struct sockaddr *)&a, sizeof a) < 0 ||
        set_nonblock_cloexec(p->fd) < 0) {
        if (p->fd >= 0) close(p->fd);
        free(p);
        return -HP_E_IO;
    }
    p->t = (hp_rf_transport){p, remote_send, remote_recv, pair_claim, NULL, NULL, NULL};
    *out = &p->t;
    return 0;
}

int hp_rf_pair_wait_attached(hp_rf_transport *t, int timeout_ms) {
    pair_t *p = (pair_t *)t;
    if (!p || p->hub) return -HP_E_INVAL;
    struct pollfd f = {p->fd, POLLIN, 0};
    int rc = poll(&f, 1, timeout_ms);
    if (rc <= 0) return -HP_E_IO;
    uint8_t b;
    ssize_t n = recv(p->fd, &b, 1, 0);
    if (n == 1 && b == ATTACH_BYTE) return 0;
    return n == 0 ? -HP_E_BUSY : -HP_E_IO; /* EOF before the handshake = refused */
}

void hp_rf_pair_destroy(hp_rf_transport *t) {
    pair_t *p = (pair_t *)t;
    if (!p) return;
    if (p->fd >= 0 && !p->hub) {
        /* Closing with unread data resets the connection and the hub would
         * lose frames we already sent; drain what the hub queued for us. */
        uint8_t junk[RF_MAX];
        while (recv(p->fd, junk, sizeof junk, 0) > 0) {}
    }
    if (p->fd >= 0) close(p->fd);
    if (p->hub) {
        for (int i = 0; i < p->npend; i++) close(p->pend[i]);
        close(p->lfd);
        unlink(p->path);
    }
    free(p);
}

int hp_rf_pair_fds(const hp_rf_transport *t, int out[2]) {
    const pair_t *p = (const pair_t *)t;
    int n = 0;
    if (!p || !p->hub) return 0;
    out[n++] = p->lfd;
    if (p->fd >= 0) out[n++] = p->fd;
    return n;
}

bool hp_rf_pair_connected(hp_rf_transport *t) {
    pair_t *p = (pair_t *)t;
    if (!p || !p->hub) return false;
    accept_pending(p);
    return p->fd >= 0;
}
