/* include/cmd_internal.h – shared utilities for cmd_*.c files
 *
 * This header is NOT part of the public API.  It is included only by the
 * command implementation files (cmd_string.c, cmd_list.c, cmd_hash.c,
 * cmd_conn.c) and never by net.c, persist.c, or tests.
 *
 * It provides:
 *   - The canonical CmdFn typedef (same as in command.h)
 *   - WRONGTYPE response helper
 *   - getOrCreate* helpers (shared by list and hash push commands)
 *   - parseI64Strict / setStringFromI64 (used by string INCR/DECR)
 */
#ifndef COMMAND_INTERNAL_H
#define COMMAND_INTERNAL_H

#include "command.h" /* CmdFn, CmdEntry */
#include "server.h"
#include "store.h"
#include "object.h"
#include "protocol.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── WRONGTYPE error ─────────────────────────────────────────────────────── *
 *
 * The prefix MUST be "-WRONGTYPE", not "-ERR", so client libraries
 * (redis-py, Jedis, …) can raise a distinct WrongTypeError. */
#define RESP_WRONGTYPE_STR "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"

static inline void respWrongType(RespBuf *buf) {
    respRaw(buf, RESP_WRONGTYPE_STR, sizeof(RESP_WRONGTYPE_STR) - 1);
}

/* ── get-or-create helpers ───────────────────────────────────────────────── *
 *
 * Returns the existing object of the expected type, creates a new empty one
 * if the key is absent, or emits WRONGTYPE / OOM and returns NULL on error.
 * The caller simply checks for NULL and returns 1 (keep connection). */

static inline Object *getOrCreateList(Server *srv, const char *key, RespBuf *buf) {
    Object *o = storeGet(srv->store, key);
    if (!o) {
        o = objListNew();
        if (!o) {
            respErr(buf, "OOM");
            return NULL;
        }
        storeSet(srv->store, key, o);
    } else if (o->type != OBJ_LIST) {
        respWrongType(buf);
        return NULL;
    }
    return o;
}

static inline Object *getOrCreateHash(Server *srv, const char *key, RespBuf *buf) {
    Object *o = storeGet(srv->store, key);
    if (!o) {
        o = objHashNew();
        if (!o) {
            respErr(buf, "OOM");
            return NULL;
        }
        storeSet(srv->store, key, o);
    } else if (o->type != OBJ_HASH) {
        respWrongType(buf);
        return NULL;
    }
    return o;
}

static inline Object *getOrCreateSet(Server *srv, const char *key, RespBuf *buf) {
    Object *o = storeGet(srv->store, key);
    if (!o) {
        o = objSetNew();
        if (!o) {
            respErr(buf, "OOM");
            return NULL;
        }
        storeSet(srv->store, key, o);
    } else if (o->type != OBJ_SET) {
        respWrongType(buf);
        return NULL;
    }
    return o;
}

/* ── Integer parsing helpers ─────────────────────────────────────────────── */

/* Strict strtoll: fails if any non-numeric chars, overflow, or empty string. */
static inline int parseI64Strict(const char *s, long long *out) {
    char *end = NULL;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno == ERANGE || end == s || (end && *end != '\0')) return 0;
    *out = v;
    return 1;
}

/* Overwrite o->str.ptr with the decimal representation of v.
 * Uses realloc so no extra allocation if new string fits. */
static inline int setStringFromI64(Object *o, long long v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", v);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return 0;
    char *p = (char *)realloc(o->str.ptr, (size_t)n + 1);
    if (!p) return 0;
    memcpy(p, buf, (size_t)n + 1);
    o->str.ptr = p;
    o->str.len = (size_t)n;
    return 1;
}

#endif /* COMMAND_INTERNAL_H */