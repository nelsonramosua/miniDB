/* src/cmd_hash.c – hash commands
 *
 * Commands: HSET HGET HDEL HLEN HGETALL
 */

#define _POSIX_C_SOURCE 200809L

#include "../../include/commandInternal.h"
#include <string.h>

int cmdHset(Server *srv, const Request *req, RespBuf *buf) {
    if ((req->argc - 2) % 2 != 0) {
        respErr(buf, "wrong number of arguments");
        return 1;
    }

    Object *o = getOrCreateHash(srv, req->argv[1], buf);
    if (!o) return 1;
    long long added = 0;
    for (int i = 2; i < req->argc; i += 2) {
        int rc = hashHset(&o->hash, req->argv[i], req->argv[i + 1], strlen(req->argv[i + 1]));
        if (rc < 0) {
            respErr(buf, "OOM");
            return 1;
        }
        added += rc;
    }
    respInt(buf, added);
    return 1;
}

int cmdHget(Server *srv, const Request *req, RespBuf *buf) {
    Object *o = storeGet(srv->store, req->argv[1]);
    if (!o) {
        respNull(buf);
        return 1;
    }
    if (o->type != OBJ_HASH) {
        respWrongType(buf);
        return 1;
    }
    size_t vlen = 0;
    char *val = hashHget(&o->hash, req->argv[2], &vlen);
    if (!val) {
        respNull(buf);
        return 1;
    }
    respBulk(buf, val, vlen);
    return 1;
}

int cmdHdel(Server *srv, const Request *req, RespBuf *buf) {
    Object *o = storeGet(srv->store, req->argv[1]);
    if (!o) {
        respInt(buf, 0);
        return 1;
    }
    if (o->type != OBJ_HASH) {
        respWrongType(buf);
        return 1;
    }
    long long removed = 0;
    for (int i = 2; i < req->argc; i++) removed += hashHdel(&o->hash, req->argv[i]);
    respInt(buf, removed);
    return 1;
}

int cmdHlen(Server *srv, const Request *req, RespBuf *buf) {
    Object *o = storeGet(srv->store, req->argv[1]);
    if (!o) {
        respInt(buf, 0);
        return 1;
    }
    if (o->type != OBJ_HASH) {
        respWrongType(buf);
        return 1;
    }
    respInt(buf, (long long)o->hash.size);
    return 1;
}

int cmdHgetall(Server *srv, const Request *req, RespBuf *buf) {
    Object *o = storeGet(srv->store, req->argv[1]);
    if (!o) {
        respArrHdr(buf, 0);
        return 1;
    }
    if (o->type != OBJ_HASH) {
        respWrongType(buf);
        return 1;
    }
    KVHash *h = &o->hash;
    respArrHdr(buf, h->size * 2);
    for (size_t i = 0; i < h->nbuckets; i++)
        for (struct HEntry *e = h->buckets[i]; e; e = e->next) {
            respBulk(buf, e->key, strlen(e->key));
            respBulk(buf, e->val, e->vlen);
        }
    return 1;
}

static int emitHashItems(Server *srv, const Request *req, RespBuf *buf, int emitKeys) {
    Object *o = storeGet(srv->store, req->argv[1]);
    if (!o) {
        respArrHdr(buf, 0);
        return 1;
    }
    if (o->type != OBJ_HASH) {
        respWrongType(buf);
        return 1;
    }

    KVHash *h = &o->hash;
    respArrHdr(buf, h->size);
    for (size_t i = 0; i < h->nbuckets; i++) {
        for (struct HEntry *e = h->buckets[i]; e; e = e->next) {
            if (emitKeys) respBulk(buf, e->key, strlen(e->key));
            else respBulk(buf, e->val, e->vlen);
        }
    }
    return 1;
}

int cmdHkeys(Server *srv, const Request *req, RespBuf *buf) {
    return emitHashItems(srv, req, buf, 1);
}

int cmdHvals(Server *srv, const Request *req, RespBuf *buf) {
    return emitHashItems(srv, req, buf, 0);
}