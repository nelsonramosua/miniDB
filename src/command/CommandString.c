/* src/cmd_string.c – string, key-space, and numeric commands
 *
 * Commands: SET GET DEL EXISTS EXPIRE PEXPIRE TTL PTTL TYPE KEYS
 *           INCR DECR INCRBY DECRBY GETSET MSET MGET
 */

#define _POSIX_C_SOURCE 200809L

#include "../../headers/CommandInternal.h"

#include <ctype.h>
#include <fnmatch.h>
#include <stdio.h>
#include <strings.h> /* strcasecmp */

/* ── SET / GET / DEL / EXISTS ───────────────────────────────────────────── */

int cmdSet(Server *srv, const Request *req, RespBuf *buf) {
    if (req->argc < 3 || req->argc > 7) {
        respErr(buf, "wrong number of arguments");
        return 1;
    }

    const char *key = req->argv[1];
    long long ttlMs = -1;
    int nx = 0;
    int xx = 0;

    for (int i = 3; i < req->argc; i++) {
        if (strcasecmp(req->argv[i], "EX") == 0 || strcasecmp(req->argv[i], "PX") == 0) {
            long long ttl = 0;
            if (ttlMs != -1 || i + 1 >= req->argc || !parseI64Strict(req->argv[i + 1], &ttl) || ttl <= 0) {
                respErr(buf, "invalid expire time");
                return 1;
            }
            ttlMs = (strcasecmp(req->argv[i], "EX") == 0) ? ttl * 1000LL : ttl;
            i++;
            continue;
        }
        if (strcasecmp(req->argv[i], "NX") == 0) {
            if (nx || xx) {
                respErr(buf, "syntax error");
                return 1;
            }
            nx = 1;
            continue;
        }
        if (strcasecmp(req->argv[i], "XX") == 0) {
            if (xx || nx) {
                respErr(buf, "syntax error");
                return 1;
            }
            xx = 1;
            continue;
        }

        respErr(buf, "syntax error");
        return 1;
    }

    Object *existing = storeGet(srv->store, key);
    if (nx && existing) {
        respNull(buf);
        return 1;
    }
    if (xx && !existing) {
        respNull(buf);
        return 1;
    }

    Object *o = objStrNew(req->argv[2], strlen(req->argv[2]));
    if (!o) {
        respErr(buf, "OOM");
        return 1;
    }
    if (ttlMs > 0) o->expireMs = nowMs() + ttlMs;
    if (!storeSet(srv->store, key, o)) {
        objFree(o);
        respErr(buf, "OOM");
        return 1;
    }
    respOk(buf);
    return 1;
}

int cmdGet(Server *srv, const Request *req, RespBuf *buf) {
    Object *o = storeGet(srv->store, req->argv[1]);
    if (!o) {
        respNull(buf);
        return 1;
    }
    if (o->type != OBJ_STRING) {
        respWrongType(buf);
        return 1;
    }
    respBulk(buf, o->str.ptr, o->str.len);
    return 1;
}

int cmdDbsize(Server *srv, const Request *req, RespBuf *buf) {
    (void)req;
    respInt(buf, (long long)srv->store->size);
    return 1;
}

int cmdFlushall(Server *srv, const Request *req, RespBuf *buf) {
    (void)req;
    Store *neo = storeNew();
    if (!neo) {
        respErr(buf, "OOM");
        return 1;
    }
    Store *old = srv->store;
    srv->store = neo;
    storeFree(old);
    respOk(buf);
    return 1;
}

int cmdDel(Server *srv, const Request *req, RespBuf *buf) {
    long long deleted = 0;
    for (int i = 1; i < req->argc; i++) deleted += storeDel(srv->store, req->argv[i]);
    respInt(buf, deleted);
    return 1;
}

int cmdExists(Server *srv, const Request *req, RespBuf *buf) {
    long long found = 0;
    for (int i = 1; i < req->argc; i++) found += storeExists(srv->store, req->argv[i]);
    respInt(buf, found);
    return 1;
}

int cmdRename(Server *srv, const Request *req, RespBuf *buf) {
    const char *src = req->argv[1], *dst = req->argv[2];
    if (strcmp(src, dst) == 0) {
        if (!storeExists(srv->store, src)) {
            respErr(buf, "no such key");
            return 1;
        }
        respOk(buf);
        return 1;
    }
    Object *o = storeDetach(srv->store, src); // safe: no free
    if (!o) {
        respErr(buf, "no such key");
        return 1;
    }
    if (!storeSet(srv->store, dst, o)) {
        objFree(o); // OOM: we still own it, must free
        respErr(buf, "OOM");
        return 1;
    }
    respOk(buf);
    return 1;
}

/* ── TTL / EXPIRE ────────────────────────────────────────────────────────── *
 *
 * The four expire/ttl commands share one implementation each; the only
 * difference is whether the user-supplied duration is in seconds or
 * milliseconds, and whether the remaining time is returned in seconds or ms.
 */

/* Set expiry on key to absoluteMs. Emits :1 on success, :0 if key missing. */
static int applyExpire(Server *srv, const char *key, int64_t absoluteMs, RespBuf *buf) {
    Object *o = storeGet(srv->store, key);
    if (!o) {
        respInt(buf, 0);
        return 1;
    }
    o->expireMs = absoluteMs;
    respInt(buf, 1);
    return 1;
}

/* Return remaining TTL. divisor=1000 -> seconds, divisor=1 -> milliseconds. */
static int queryTtl(Server *srv, const char *key, int64_t divisor, RespBuf *buf) {
    Object *o = storeGet(srv->store, key);
    if (!o) {
        respInt(buf, -2);
        return 1;
    }
    if (o->expireMs == 0) {
        respInt(buf, -1);
        return 1;
    }
    long long rem = o->expireMs - nowMs();
    respInt(buf, rem > 0 ? rem / divisor : 0);
    return 1;
}

int cmdExpire(Server *srv, const Request *req, RespBuf *buf) {
    long long secs = 0;
    if (!parseI64Strict(req->argv[2], &secs) || secs <= 0) {
        respErr(buf, "invalid expire time");
        return 1;
    }
    return applyExpire(srv, req->argv[1], nowMs() + secs * 1000LL, buf);
}

int cmdPexpire(Server *srv, const Request *req, RespBuf *buf) {
    long long ms = 0;
    if (!parseI64Strict(req->argv[2], &ms) || ms <= 0) {
        respErr(buf, "invalid expire time");
        return 1;
    }
    return applyExpire(srv, req->argv[1], nowMs() + ms, buf);
}

int cmdTtl(Server *srv, const Request *req, RespBuf *buf) {
    return queryTtl(srv, req->argv[1], 1000, buf);
}
int cmdPttl(Server *srv, const Request *req, RespBuf *buf) {
    return queryTtl(srv, req->argv[1], 1, buf);
}

int cmdPersist(Server *srv, const Request *req, RespBuf *buf) {
    Object *o = storeGet(srv->store, req->argv[1]);
    if (!o || o->expireMs == 0) {
        respInt(buf, 0);
        return 1;
    }
    storeSetExpire(srv->store, req->argv[1], 0);
    respInt(buf, 1);
    return 1;
}

/* ── TYPE / KEYS ─────────────────────────────────────────────────────────── */

int cmdType(Server *srv, const Request *req, RespBuf *buf) {
    static const char *names[] = {"+string\r\n", "+list\r\n", "+hash\r\n"};
    static const size_t LENS[] = {sizeof("+string\r\n") - 1, sizeof("+list\r\n") - 1, sizeof("+hash\r\n") - 1};
    Object *o = storeGet(srv->store, req->argv[1]);
    if (!o) {
        respRaw(buf, "+none\r\n", 7);
        return 1;
    }
    respRaw(buf, names[o->type], LENS[o->type]);
    return 1;
}

int cmdKeys(Server *srv, const Request *req, RespBuf *buf) {
    const char *pattern = req->argv[1];
    size_t n = 0;
    char **keys = storeKeys(srv->store, &n);

    if (n == 0) {
        respArrHdr(buf, 0);
        free(keys);
        return 1;
    }

    /* Single-pass: collect match indices to avoid calling fnmatch twice. */
    size_t *matches = malloc(n * sizeof(*matches));
    size_t nmatch = 0;
    if (matches) {
        for (size_t i = 0; i < n; i++)
            if (fnmatch(pattern, keys[i], 0) == 0) matches[nmatch++] = i;
    }

    respArrHdr(buf, nmatch);
    for (size_t i = 0; i < nmatch; i++) respBulk(buf, keys[matches[i]], strlen(keys[matches[i]]));

    free(matches);
    for (size_t i = 0; i < n; i++) free(keys[i]);
    free(keys);
    return 1;
}

int cmdScan(Server *srv, const Request *req, RespBuf *buf) {
    long long cursor = 0;
    const char *pattern = "*";
    long long count = 10;

    if (!parseI64Strict(req->argv[1], &cursor) || cursor < 0) {
        respErr(buf, "invalid cursor");
        return 1;
    }

    for (int i = 2; i < req->argc; i++) {
        if (strcasecmp(req->argv[i], "MATCH") == 0) {
            if (i + 1 >= req->argc) {
                respErr(buf, "syntax error");
                return 1;
            }
            pattern = req->argv[++i];
            continue;
        }
        if (strcasecmp(req->argv[i], "COUNT") == 0) {
            if (i + 1 >= req->argc || !parseI64Strict(req->argv[i + 1], &count) || count <= 0) {
                respErr(buf, "syntax error");
                return 1;
            }
            i++;
            continue;
        }
        respErr(buf, "syntax error");
        return 1;
    }

    size_t n = 0;
    char **keys = storeKeys(srv->store, &n);
    if (!keys && n == 0 && srv->store->size > 0) {
        respErr(buf, "OOM");
        return 1;
    }

    size_t idx = (size_t)cursor;
    if (idx > n) idx = n;

    size_t maxOut = (size_t)count;
    size_t *matches = malloc(maxOut * sizeof(*matches));
    if (!matches && maxOut > 0) {
        for (size_t i = 0; i < n; i++) free(keys[i]);
        free(keys);
        respErr(buf, "OOM");
        return 1;
    }

    size_t out = 0;
    while (idx < n && out < maxOut) {
        if (fnmatch(pattern, keys[idx], 0) == 0) matches[out++] = idx;
        idx++;
    }

    size_t nextCursor = (idx < n) ? idx : 0;
    char cursorBuf[32];
    int cursorLen = snprintf(cursorBuf, sizeof(cursorBuf), "%zu", nextCursor);

    respArrHdr(buf, 2);
    respBulk(buf, cursorBuf, (size_t)cursorLen);
    respArrHdr(buf, out);
    for (size_t i = 0; i < out; i++) respBulk(buf, keys[matches[i]], strlen(keys[matches[i]]));

    free(matches);
    for (size_t i = 0; i < n; i++) free(keys[i]);
    free(keys);
    return 1;
}

/* ── INCR / DECR / INCRBY / DECRBY ─────────────────────────────────────── */

static int incrDecrBy(Server *srv, const Request *req, long long delta, RespBuf *buf) {
    const char *key = req->argv[1];
    Object *o = storeGet(srv->store, key);

    if (!o) {
        /* Key absent: treat as zero, set to delta */
        char tmp[32];
        int n = snprintf(tmp, sizeof(tmp), "%lld", delta);
        Object *neo = objStrNew(tmp, (size_t)n);
        if (!neo) {
            respErr(buf, "OOM");
            return 1;
        }
        storeSet(srv->store, key, neo);
        respInt(buf, delta);
        return 1;
    }

    if (o->type != OBJ_STRING) {
        respWrongType(buf);
        return 1;
    }

    long long cur = 0;
    if (!parseI64Strict(o->str.ptr, &cur)) {
        respErr(buf, "value is not an integer or out of range");
        return 1;
    }
    if ((delta > 0 && cur > LLONG_MAX - delta) || (delta < 0 && cur < LLONG_MIN - delta)) {
        respErr(buf, "value is not an integer or out of range");
        return 1;
    }

    long long next = cur + delta;
    if (!setStringFromI64(o, next)) {
        respErr(buf, "OOM");
        return 1;
    }
    respInt(buf, next);
    return 1;
}

int cmdIncr(Server *srv, const Request *req, RespBuf *buf) {
    return incrDecrBy(srv, req, 1, buf);
}
int cmdDecr(Server *srv, const Request *req, RespBuf *buf) {
    return incrDecrBy(srv, req, -1, buf);
}

int cmdIncrby(Server *srv, const Request *req, RespBuf *buf) {
    long long delta = 0;
    if (!parseI64Strict(req->argv[2], &delta)) {
        respErr(buf, "value is not an integer or out of range");
        return 1;
    }
    return incrDecrBy(srv, req, delta, buf);
}

int cmdDecrby(Server *srv, const Request *req, RespBuf *buf) {
    long long delta = 0;
    if (!parseI64Strict(req->argv[2], &delta)) {
        respErr(buf, "value is not an integer or out of range");
        return 1;
    }
    if (delta == LLONG_MIN) {
        respErr(buf, "value is not an integer or out of range");
        return 1;
    }
    return incrDecrBy(srv, req, -delta, buf);
}

int cmdSetnx(Server *srv, const Request *req, RespBuf *buf) {
    if (storeExists(srv->store, req->argv[1])) {
        respInt(buf, 0);
        return 1;
    }

    Object *o = objStrNew(req->argv[2], strlen(req->argv[2]));
    if (!o) {
        respErr(buf, "OOM");
        return 1;
    }

    if (!storeSet(srv->store, req->argv[1], o)) {
        objFree(o);
        respErr(buf, "OOM");
        return 1;
    }

    respInt(buf, 1);
    return 1;
}

int cmdAppend(Server *srv, const Request *req, RespBuf *buf) {
    const char *suffix = req->argv[2];
    size_t slen = strlen(suffix);
    Object *o = storeGet(srv->store, req->argv[1]);

    if (!o) {
        Object *neo = objStrNew(suffix, slen);
        if (!neo) {
            respErr(buf, "OOM");
            return 1;
        }
        if (!storeSet(srv->store, req->argv[1], neo)) {
            objFree(neo);
            respErr(buf, "OOM");
            return 1;
        }
        respInt(buf, (long long)slen);
        return 1;
    }

    if (o->type != OBJ_STRING) {
        respWrongType(buf);
        return 1;
    }

    char *newptr = realloc(o->str.ptr, o->str.len + slen + 1);
    if (!newptr) {
        respErr(buf, "OOM");
        return 1;
    }

    memcpy(newptr + o->str.len, suffix, slen + 1);
    o->str.ptr = newptr;
    o->str.len += slen;
    respInt(buf, (long long)o->str.len);
    return 1;
}

int cmdStrlen(Server *srv, const Request *req, RespBuf *buf) {
    Object *o = storeGet(srv->store, req->argv[1]);
    if (!o) {
        respInt(buf, 0);
        return 1;
    }
    if (o->type != OBJ_STRING) {
        respWrongType(buf);
        return 1;
    }
    respInt(buf, (long long)o->str.len);
    return 1;
}

int cmdGetdel(Server *srv, const Request *req, RespBuf *buf) {
    Object *o = storeGet(srv->store, req->argv[1]);
    if (!o) {
        respNull(buf);
        return 1;
    }
    if (o->type != OBJ_STRING) {
        respWrongType(buf);
        return 1;
    }
    respBulk(buf, o->str.ptr, o->str.len);
    storeDel(srv->store, req->argv[1]);
    return 1;
}

/* ── GETSET / MSET / MGET ───────────────────────────────────────────────── */

int cmdGetset(Server *srv, const Request *req, RespBuf *buf) {
    Object *old = storeGet(srv->store, req->argv[1]);
    if (old && old->type != OBJ_STRING) {
        respWrongType(buf);
        return 1;
    }
    Object *neo = objStrNew(req->argv[2], strlen(req->argv[2]));
    if (!neo) {
        respErr(buf, "OOM");
        return 1;
    }

    if (old) respBulk(buf, old->str.ptr, old->str.len);
    else respNull(buf);

    if (!storeSet(srv->store, req->argv[1], neo)) objFree(neo);
    return 1;
}

int cmdMset(Server *srv, const Request *req, RespBuf *buf) {
    if ((req->argc - 1) % 2 != 0) {
        respErr(buf, "wrong number of arguments");
        return 1;
    }
    for (int i = 1; i < req->argc; i += 2) {
        Object *o = objStrNew(req->argv[i + 1], strlen(req->argv[i + 1]));
        if (!o) {
            respErr(buf, "OOM");
            return 1;
        }
        storeSet(srv->store, req->argv[i], o);
    }
    respOk(buf);
    return 1;
}

int cmdMget(Server *srv, const Request *req, RespBuf *buf) {
    respArrHdr(buf, (size_t)(req->argc - 1));
    for (int i = 1; i < req->argc; i++) {
        Object *o = storeGet(srv->store, req->argv[i]);
        if (!o || o->type != OBJ_STRING) respNull(buf);
        else respBulk(buf, o->str.ptr, o->str.len);
    }
    return 1;
}