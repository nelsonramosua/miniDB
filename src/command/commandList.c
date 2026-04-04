/* src/cmd_list.c – list commands
 *
 * Commands: LPUSH RPUSH LPOP RPOP LLEN
 */

#define _POSIX_C_SOURCE 200809L

#include "../../include/commandInternal.h"
#include <stdlib.h>

/* ── Push ────────────────────────────────────────────────────────────────── */

int cmdLpush(Server *srv, const Request *req, RespBuf *buf) {
    Object *o = getOrCreateList(srv, req->argv[1], buf);
    if (!o) return 1;
    for (int i = 2; i < req->argc; i++) {
        if (!listLpush(&o->list, req->argv[i], strlen(req->argv[i]))) {
            respErr(buf, "OOM");
            return 1;
        }
    }
    respInt(buf, (long long)o->list.size);
    return 1;
}

int cmdRpush(Server *srv, const Request *req, RespBuf *buf) {
    Object *o = getOrCreateList(srv, req->argv[1], buf);
    if (!o) return 1;
    for (int i = 2; i < req->argc; i++) {
        if (!listRpush(&o->list, req->argv[i], strlen(req->argv[i]))) {
            respErr(buf, "OOM");
            return 1;
        }
    }
    respInt(buf, (long long)o->list.size);
    return 1;
}

/* ── Pop (shared helper) ─────────────────────────────────────────────────── *
 *
 * LPOP and RPOP differ only in which end they pop from.
 * popFn is either listLpop or listRpop. */

typedef char *(*PopFn)(KVList *, size_t *);

static int listPop(Server *srv, const Request *req, RespBuf *buf, PopFn popFn) {
    Object *o = storeGet(srv->store, req->argv[1]);
    if (!o) {
        respNull(buf);
        return 1;
    }
    if (o->type != OBJ_LIST) {
        respWrongType(buf);
        return 1;
    }

    size_t len = 0;
    char *data = popFn(&o->list, &len);
    if (!data) {
        respNull(buf);
        return 1;
    }

    respBulk(buf, data, len);
    free(data);

    /* Redis removes the key when a list becomes empty */
    if (o->list.size == 0) storeDel(srv->store, req->argv[1]);
    return 1;
}

int cmdLpop(Server *srv, const Request *req, RespBuf *buf) {
    return listPop(srv, req, buf, listLpop);
}

int cmdRpop(Server *srv, const Request *req, RespBuf *buf) {
    return listPop(srv, req, buf, listRpop);
}

/* ── LLEN ────────────────────────────────────────────────────────────────── */

int cmdLlen(Server *srv, const Request *req, RespBuf *buf) {
    Object *o = storeGet(srv->store, req->argv[1]);
    if (!o) {
        respInt(buf, 0);
        return 1;
    }
    if (o->type != OBJ_LIST) {
        respWrongType(buf);
        return 1;
    }
    respInt(buf, (long long)o->list.size);
    return 1;
}

int cmdLrange(Server *srv, const Request *req, RespBuf *buf) {
    Object *o = storeGet(srv->store, req->argv[1]);
    if (!o) {
        respArrHdr(buf, 0);
        return 1;
    }
    if (o->type != OBJ_LIST) {
        respWrongType(buf);
        return 1;
    }

    long long size = (long long)o->list.size;
    long long start = 0;
    long long stop = 0;
    if (!parseI64Strict(req->argv[2], &start) || !parseI64Strict(req->argv[3], &stop)) {
        respErr(buf, "value is not an integer or out of range");
        return 1;
    }

    if (start < 0) start += size;
    if (stop < 0) stop += size;
    if (start < 0) start = 0;

    if (size == 0 || start > (size - 1) || start > stop) {
        respArrHdr(buf, 0);
        return 1;
    }
    if (stop > (size - 1)) stop = size - 1;

    long long count = stop - start + 1;
    respArrHdr(buf, (size_t)count);

    ListNode *n = o->list.head;
    for (long long i = 0; i < start && n; i++) n = n->next;
    for (long long i = 0; i < count && n; i++, n = n->next) respBulk(buf, n->data, n->len);

    return 1;
}