/* include/command.h – public command interface
 *
 * Exposes:
 *   - CmdFn / CmdEntry types
 *   - cmdLookup / cmdDispatch (used by net.c)
 *   - Forward declarations for every handler (used by command.c)
 */
#ifndef COMMAND_H
#define COMMAND_H

#include "protocol.h" /* RespBuf, Request */
#include <stddef.h>

/* Forward declarations */
typedef struct Server Server;

/* Handler signature: writes RESP into buf, returns 1 to keep connection,
 * 0 to close (QUIT). */
typedef int (*CmdFn)(Server *srv, const Request *req, RespBuf *buf);

typedef struct {
    const char *name; /* upper-case, NUL-terminated */
    int minargs;      /* minimum argc including command name */
    int maxargs;      /* -1 = variadic                       */
    CmdFn fn;
} CmdEntry;

/* ── Public API ─────────────────────────────────────────────────────────── */

const CmdEntry *cmdLookup(const char *name);
int cmdDispatch(Server *srv, const Request *req, RespBuf *buf);

/* ── Handler forward declarations ───────────────────────────────────────── */

/* Connection */
int cmdPing(Server *, const Request *, RespBuf *);
int cmdQuit(Server *, const Request *, RespBuf *);
int cmdClient(Server *, const Request *, RespBuf *);
int cmdCommand(Server *, const Request *, RespBuf *);
int cmdConfig(Server *, const Request *, RespBuf *);
int cmdInfo(Server *, const Request *, RespBuf *);

/* String / key-space */
int cmdSet(Server *, const Request *, RespBuf *);
int cmdGet(Server *, const Request *, RespBuf *);
int cmdDel(Server *, const Request *, RespBuf *);
int cmdExists(Server *, const Request *, RespBuf *);
int cmdType(Server *, const Request *, RespBuf *);
int cmdKeys(Server *, const Request *, RespBuf *);
int cmdScan(Server *, const Request *, RespBuf *);
int cmdMset(Server *, const Request *, RespBuf *);
int cmdMget(Server *, const Request *, RespBuf *);
int cmdGetset(Server *, const Request *, RespBuf *);
int cmdIncr(Server *, const Request *, RespBuf *);
int cmdDecr(Server *, const Request *, RespBuf *);
int cmdIncrby(Server *, const Request *, RespBuf *);
int cmdDecrby(Server *, const Request *, RespBuf *);
int cmdSetnx(Server *, const Request *, RespBuf *);
int cmdAppend(Server *, const Request *, RespBuf *);
int cmdStrlen(Server *, const Request *, RespBuf *);
int cmdGetdel(Server *, const Request *, RespBuf *);
int cmdDbsize(Server *, const Request *, RespBuf *);
int cmdFlushall(Server *, const Request *, RespBuf *);
int cmdRename(Server *, const Request *, RespBuf *);

/* TTL */
int cmdExpire(Server *, const Request *, RespBuf *);
int cmdPexpire(Server *, const Request *, RespBuf *);
int cmdTtl(Server *, const Request *, RespBuf *);
int cmdPttl(Server *, const Request *, RespBuf *);
int cmdPersist(Server *, const Request *, RespBuf *);

/* List */
int cmdLpush(Server *, const Request *, RespBuf *);
int cmdRpush(Server *, const Request *, RespBuf *);
int cmdLpop(Server *, const Request *, RespBuf *);
int cmdRpop(Server *, const Request *, RespBuf *);
int cmdLlen(Server *, const Request *, RespBuf *);
int cmdLrange(Server *, const Request *, RespBuf *);
int cmdLtrim(Server *, const Request *, RespBuf *);

/* Hash */
int cmdHset(Server *, const Request *, RespBuf *);
int cmdHget(Server *, const Request *, RespBuf *);
int cmdHdel(Server *, const Request *, RespBuf *);
int cmdHlen(Server *, const Request *, RespBuf *);
int cmdHgetall(Server *, const Request *, RespBuf *);
int cmdHkeys(Server *, const Request *, RespBuf *);
int cmdHvals(Server *, const Request *, RespBuf *);

/* Set */
int cmdSadd(Server *, const Request *, RespBuf *);
int cmdSmembers(Server *, const Request *, RespBuf *);
int cmdSismember(Server *, const Request *, RespBuf *);
int cmdSrem(Server *, const Request *, RespBuf *);
int cmdScard(Server *, const Request *, RespBuf *);
int cmdSunion(Server *, const Request *, RespBuf *);
int cmdSinter(Server *, const Request *, RespBuf *);
int cmdSdiff(Server *, const Request *, RespBuf *);

#endif /* COMMAND_H */
