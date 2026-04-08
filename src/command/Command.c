/* src/command.c – command dispatch table and lookup
 *
 * This file has exactly one job: map command names to handler functions.
 * All implementations live in commandString.c, commandList.c, commandHash.c,
 * commandConn.c.  To add a new command:
 *   1. Implement it in the appropriate cmd*.c file.
 *   2. Add a forward declaration to headers/Command.h.
 *   3. Add one row to CMD_TABLE[] below.
 */

#define _POSIX_C_SOURCE 200809L

#include "../../headers/Command.h"
#include "../../headers/Protocol.h"
#include "../../headers/Server.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ── Dispatch table ─────────────────────────────────────────────────────── *
 *
 * Columns: name (upper-case) | min argc | max argc (-1 = variadic) | handler
 *
 * argc counts include the command name itself:
 *   "GET key" -> argc 2, so minargs=2, maxargs=2.
 *   "DEL k1 k2 …" -> argc ≥ 2, so minargs=2, maxargs=-1.
 */
static const CmdEntry CMD_TABLE[] = {
    /* ── Connection ───────────────────────────── */
    {"PING", 1, 2, cmdPing},
    {"QUIT", 1, 1, cmdQuit},
    {"CLIENT", 1, -1, cmdClient},
    {"COMMAND", 1, -1, cmdCommand},
    {"CONFIG", 2, -1, cmdConfig},
    {"INFO", 1, 1, cmdInfo},
    {"HELLO", 1, -1, cmdHello},

    /* ── String / key-space ────────────────────── */
    {"SET", 3, 7, cmdSet},
    {"GET", 2, 2, cmdGet},
    {"DBSIZE", 1, 1, cmdDbsize},
    {"FLUSHALL", 1, 1, cmdFlushall},
    {"DEL", 2, -1, cmdDel},
    {"RENAME", 3, 3, cmdRename},
    {"EXISTS", 2, -1, cmdExists},
    {"TYPE", 2, 2, cmdType},
    {"KEYS", 2, 2, cmdKeys},
    {"SCAN", 2, 6, cmdScan},
    {"MSET", 3, -1, cmdMset},
    {"MGET", 2, -1, cmdMget},
    {"GETSET", 3, 3, cmdGetset},
    {"INCR", 2, 2, cmdIncr},
    {"DECR", 2, 2, cmdDecr},
    {"INCRBY", 3, 3, cmdIncrby},
    {"DECRBY", 3, 3, cmdDecrby},
    {"SETNX", 3, 3, cmdSetnx},
    {"APPEND", 3, 3, cmdAppend},
    {"STRLEN", 2, 2, cmdStrlen},
    {"GETDEL", 2, 2, cmdGetdel},

    /* ── TTL ───────────────────────────────────── */
    {"EXPIRE", 3, 3, cmdExpire},
    {"PEXPIRE", 3, 3, cmdPexpire},
    {"TTL", 2, 2, cmdTtl},
    {"PTTL", 2, 2, cmdPttl},
    {"PERSIST", 2, 2, cmdPersist},

    /* ── List ──────────────────────────────────── */
    {"LPUSH", 3, -1, cmdLpush},
    {"RPUSH", 3, -1, cmdRpush},
    {"LPOP", 2, 2, cmdLpop},
    {"RPOP", 2, 2, cmdRpop},
    {"LLEN", 2, 2, cmdLlen},
    {"LRANGE", 4, 4, cmdLrange},
    {"LTRIM", 4, 4, cmdLtrim},

    /* ── Hash ──────────────────────────────────── */
    {"HSET", 4, -1, cmdHset},
    {"HGET", 3, 3, cmdHget},
    {"HDEL", 3, -1, cmdHdel},
    {"HLEN", 2, 2, cmdHlen},
    {"HGETALL", 2, 2, cmdHgetall},
    {"HKEYS", 2, 2, cmdHkeys},
    {"HVALS", 2, 2, cmdHvals},

    /* ── Set ───────────────────────────────────── */
    {"SADD", 3, -1, cmdSadd},
    {"SMEMBERS", 2, 2, cmdSmembers},
    {"SISMEMBER", 3, 3, cmdSismember},
    {"SREM", 3, -1, cmdSrem},
    {"SCARD", 2, 2, cmdScard},
    {"SUNION", 2, -1, cmdSunion},
    {"SINTER", 2, -1, cmdSinter},
    {"SDIFF", 2, -1, cmdSdiff},

    {NULL, 0, 0, NULL},
};

/* ── Lookup ─────────────────────────────────────────────────────────────── */

const CmdEntry *cmdLookup(const char *name) {
    char upper[64];
    size_t i;
    for (i = 0; name[i] && i < sizeof(upper) - 1; i++) upper[i] = (char)toupper((unsigned char)name[i]);
    upper[i] = '\0';

    for (const CmdEntry *c = CMD_TABLE; c->name; c++)
        if (strcmp(c->name, upper) == 0) return c;
    return NULL;
}

/* ── Dispatch ────────────────────────────────────────────────────────────── */

int cmdDispatch(Server *srv, const Request *req, RespBuf *buf) {
    if (req->argc == 0) return 1;

    const CmdEntry *c = cmdLookup(req->argv[0]);
    if (!c) {
        char msg[128];
        snprintf(msg, sizeof(msg), "unknown command '%s'", req->argv[0]);
        respErr(buf, msg);
        return 1;
    }
    if (req->argc < c->minargs || (c->maxargs != -1 && req->argc > c->maxargs)) {
        respErr(buf, "wrong number of arguments");
        return 1;
    }
    srv->totalCommands++;
    return c->fn(srv, req, buf);
}