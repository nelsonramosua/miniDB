/* src/net.c – event loop, connection management, non-blocking I/O
 *
 * Responsibilities (only these):
 *   - TCP accept / client lifecycle
 *   - Non-blocking read -> parse -> dispatch -> buffer response
 *   - Non-blocking write with per-tick budget
 *   - Call serverTick() every ~100 ms for maintenance
 *
 * net.c has no knowledge of persistence, TTL expiry, or store internals.
 * That logic lives in server.c (serverTick) and the layers below it.
 */

#define _POSIX_C_SOURCE 200809L

#include "../include/net.h"
#include "../include/server.h"   /* serverTick, Server */
#include "../include/command.h"  /* cmdDispatch */
#include "../include/protocol.h" /* RespBuf, protoParse */
#include "../include/persist.h"  /* persistLoad (startup only) */
#include "../include/object.h"   /* nowMs */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ── Client ─────────────────────────────────────────────────────────────── *
 *
 * Each client owns:
 *   inbuf / inlen / incap  — incoming data ring, grows up to NET_INBUF_MAX
 *   out                    — RespBuf holding pending response bytes
 */
typedef struct {
    int fd;
    char *inbuf;
    size_t inlen;
    size_t incap;
    RespBuf out;
} Client;

static int clientWrite(Client *c);

static Client *clientNew(int fd) {
    Client *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->fd = fd;
    c->inbuf = malloc(NET_INBUF_SIZE);
    if (!c->inbuf) {
        free(c);
        return NULL;
    }
    c->incap = NET_INBUF_SIZE;
    return c;
}

static void clientFree(Client *c) {
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c->inbuf);
    respBufFree(&c->out);
    free(c);
}

/* ── Socket helpers ─────────────────────────────────────────────────────── */

static int setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int tcpListen(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 128) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    setNonBlocking(fd);
    return fd;
}

/* ── Signal handling ────────────────────────────────────────────────────── */

static volatile sig_atomic_t gStop = 0;

static void handleSignal(int sig) {
    (void)sig;
    gStop = 1;
}

/* ── Per-client I/O ─────────────────────────────────────────────────────── */

/* Read from socket -> parse commands -> dispatch. Returns 0 to close. */
static int clientRead(Server *srv, Client *c) {
    int peerClosed = 0;

    /* Grow inbuf if needed */
    if (c->inlen + 1 >= c->incap) {
        size_t newCap = c->incap * 2;
        if (newCap > NET_INBUF_MAX) newCap = NET_INBUF_MAX;
        if (newCap <= c->incap) {
            respErr(&c->out, "request too large");
            return 0;
        }
        char *nb = realloc(c->inbuf, newCap);
        if (!nb) {
            respErr(&c->out, "OOM");
            return 0;
        }
        c->inbuf = nb;
        c->incap = newCap;
    }

    ssize_t n = read(c->fd, c->inbuf + c->inlen, (c->incap - 1) - c->inlen);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
        return 0; /* hard error */
    }
    if (n == 0) {
        /* Peer closed write-side. We still must parse any buffered bytes. */
        peerClosed = 1;
    } else {
        c->inlen += (size_t)n;
        c->inbuf[c->inlen] = '\0';
    }

    /* Drain all complete commands from inbuf */
    size_t offset = 0;
    size_t parsedCmds = 0;
    while (offset < c->inlen && parsedCmds < NET_MAX_PARSE_CMDS_PER_READ) {
        Request req;
        size_t consumed = 0;
        int rc = protoParse(c->inbuf + offset, c->inlen - offset, &req, &consumed);
        if (rc == 0) break; /* need more data */
        if (rc < 0) {
            respErr(&c->out, "protocol error");
            const char *nl = memchr(c->inbuf + offset, '\n', c->inlen - offset);
            offset += nl ? (size_t)(nl - (c->inbuf + offset)) + 1 : c->inlen - offset;
            continue;
        }

        offset += consumed;
        int keep = cmdDispatch(srv, &req, &c->out);
        protoReqFree(&req);
        if (!keep) return 0;

        /* Try to drain replies immediately. This reduces a race where the
         * peer half-closes quickly and we would otherwise wait for a later
         * POLLOUT cycle to flush buffered output. */
        if (c->out.len > 0 && !clientWrite(c)) return 0;
        parsedCmds++;
    }

    /* Compact inbuf — shift unconsumed bytes to front */
    if (offset > 0 && offset < c->inlen) memmove(c->inbuf, c->inbuf + offset, c->inlen - offset);
    c->inlen -= offset;
    if (c->inlen < c->incap) c->inbuf[c->inlen] = '\0';

    if (peerClosed) {
        /* If peer has closed write-side, close once buffered replies are flushed.
         * Any leftover unread input is incomplete and cannot form a full command. */
        return c->out.len > 0 ? 1 : 0;
    }

    return 1;
}

/* Write pending response bytes to socket. Returns 0 to close. */
static int clientWrite(Client *c) {
    size_t budget = NET_MAX_WRITE_BYTES_PER_TICK;
    while (c->out.len > 0 && budget > 0) {
        size_t toWrite = c->out.len < budget ? c->out.len : budget;
        ssize_t n = write(c->fd, c->out.data, toWrite);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
            return 0;
        }
        if ((size_t)n < c->out.len) memmove(c->out.data, c->out.data + n, c->out.len - (size_t)n);
        c->out.len -= (size_t)n;
        budget -= (size_t)n;
    }
    return 1;
}

/* ── Main event loop ────────────────────────────────────────────────────── */

void netRun(Server *srv) {
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);
    signal(SIGPIPE, SIG_IGN);

    if (srv->snapshotPath) {
        int r = persistLoad(srv);
        if (r == 0) fprintf(stderr, "[persist] snapshot loaded\n");
        else if (r == 1) fprintf(stderr, "[persist] no snapshot found, starting fresh\n");
        else fprintf(stderr, "[persist] WARN: snapshot load failed\n");
    }

    int listenfd = tcpListen(srv->port);
    if (listenfd < 0) return;
    fprintf(stderr, "[net] listening on port %d\n", srv->port);

    Client *clients[NET_MAX_CLIENTS] = {0};
    struct pollfd pfds[NET_MAX_CLIENTS + 1];
    int nclient = 0;
    int64_t lastTickMs = nowMs();

    while (!gStop) {
        /* Build pollfd array: index 0 = listen socket, 1..n = clients */
        pfds[0].fd = listenfd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        int nfds = 1;
        for (int i = 0; i < nclient; i++) {
            pfds[nfds].fd = clients[i]->fd;
            pfds[nfds].events = POLLIN | (clients[i]->out.len ? POLLOUT : 0);
            pfds[nfds].revents = 0;
            nfds++;
        }

        int ready = poll(pfds, (nfds_t)nfds, NET_POLL_TIMEOUT_MS);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        /* Accept new connections */
        if (pfds[0].revents & POLLIN) {
            struct sockaddr_in caddr;
            socklen_t clen = sizeof(caddr);
            int cfd = accept(listenfd, (struct sockaddr *)&caddr, &clen);
            if (cfd >= 0) {
                setNonBlocking(cfd);
                const char *reject = NULL;
                if (nclient >= NET_MAX_CLIENTS) reject = "-ERR max clients reached\r\n";

                Client *nc = reject ? NULL : clientNew(cfd);
                if (!nc) {
                    const char *msg = reject ? reject : "-ERR server out of memory\r\n";
                    ssize_t ignoredWrite = write(cfd, msg, strlen(msg));
                    (void)ignoredWrite;
                    close(cfd);
                } else {
                    clients[nclient++] = nc;
                    fprintf(stderr, "[net] +client fd=%d  total=%d\n", cfd, nclient);
                }
            }
        }

        /* Service clients (backwards for safe swap-remove) */
        for (int i = nclient - 1; i >= 0; i--) {
            int pi = i + 1;
            int close = 0;

            if (pfds[pi].revents & POLLNVAL) { close = 1; }
            if (!close && (pfds[pi].revents & (POLLIN | POLLHUP | POLLERR)))
                if (!clientRead(srv, clients[i])) close = 1;
            if (!close && (pfds[pi].revents & POLLOUT))
                if (!clientWrite(clients[i])) close = 1;
            if (!close && (pfds[pi].revents & (POLLHUP | POLLERR)) && clients[i]->out.len == 0) close = 1;

            if (close) {
                fprintf(stderr, "[net] -client fd=%d\n", clients[i]->fd);
                clientFree(clients[i]);
                clients[i] = clients[--nclient];
                clients[nclient] = NULL;
            }
        }

        /* Periodic maintenance via serverTick — net.c stays ignorant of details */
        int64_t now = nowMs();
        if (now - lastTickMs >= NET_POLL_TIMEOUT_MS) {
            serverTick(srv);
            lastTickMs = now;
        }
    }

    fprintf(stderr, "\n[net] shutting down...\n");
    for (int i = 0; i < nclient; i++) clientFree(clients[i]);
    close(listenfd);
    if (srv->snapshotPath) {
        if (persistSave(srv) == 0) fprintf(stderr, "[persist] final snapshot saved\n");
    }
}