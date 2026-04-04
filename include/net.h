/* src/net.h – single-threaded event loop (poll-based)
 *
 * Architecture: one thread, one event loop, no locking.
 *
 *   WHY poll() OVER select():
 *     select() is limited by FD_SETSIZE (typically 1024) and requires
 *     rebuilding the fd_set on every call.  poll() uses a flat array of
 *     pollfd structs, has no fd-count limit, and is simpler to manage
 *     dynamically as clients connect/disconnect.
 *
 *   WHY NOT epoll/kqueue:
 *     epoll is Linux-only; kqueue is BSD/macOS.  poll() is POSIX and
 *     portable, which is the right choice for a project that will run in
 *     CI on whatever runner GitHub Actions provides.  For 100s of clients,
 *     poll() is perfectly sufficient.
 *
 * Each Client owns:
 *   - inbuf / inlen:  incomplete incoming data (commands may span reads)
 *   - outbuf / outlen / outcap:  pending response bytes to drain to socket
 *
 * The event loop:
 *   1. poll() — wait for events on all fds (timeout 100ms for TTL purge)
 *   2. If server fd readable: accept() new client
 *   3. For each readable client fd: read() -> parse commands -> dispatch
 *   4. For each writable client fd (with pending output): write() partial
 *   5. Every ~100ms: storePurgeExpired() + optional snapshot
 */
#ifndef NET_H
#define NET_H

#include "server.h"

#define NET_MAX_CLIENTS 1024
#define NET_INBUF_SIZE 4096
#define NET_INBUF_MAX (1024 * 1024)
#define NET_MAX_PARSE_CMDS_PER_READ 256
#define NET_MAX_WRITE_BYTES_PER_TICK (64 * 1024)
#define NET_POLL_TIMEOUT_MS 100

/* Run the event loop forever (until SIGINT/SIGTERM).
 * Calls persistLoad() on start, persistSave() on clean shutdown. */
void netRun(Server *srv);

#endif /* NET_H */