/* src/command/CommandSet.c – set commands
 *
 * Commands: SADD SMEMBERS SISMEMBER
 */

#define _POSIX_C_SOURCE 200809L

#include "../../headers/CommandInternal.h"
#include <string.h>

int cmdSadd(Server *srv, const Request *req, RespBuf *buf) {
    Object *o = getOrCreateSet(srv, req->argv[1], buf);
    if (!o) return 1;

    long long added = 0;
    for (int i = 2; i < req->argc; i++) {
        /* We just use hashHset with an empty value to represent set membership */
        int rc = hashHset(&o->hash, req->argv[i], "", 0);
        if (rc < 0) {
            respErr(buf, "OOM");
            return 1;
        }
        added += rc;
    }
    respInt(buf, added);
    return 1;
}

int cmdSmembers(Server *srv, const Request *req, RespBuf *buf) {
    Object *o = storeGet(srv->store, req->argv[1]);
    if (!o) {
        respArrHdr(buf, 0);
        return 1;
    }
    if (o->type != OBJ_SET) {
        respWrongType(buf);
        return 1;
    }

    respArrHdr(buf, o->hash.size);
    for (size_t i = 0; i < o->hash.nbuckets; i++) {
        for (struct HEntry *e = o->hash.buckets[i]; e; e = e->next) { respBulk(buf, e->key, strlen(e->key)); }
    }
    return 1;
}

int cmdSismember(Server *srv, const Request *req, RespBuf *buf) {
    Object *o = storeGet(srv->store, req->argv[1]);
    if (!o) {
        respInt(buf, 0);
        return 1;
    }
    if (o->type != OBJ_SET) {
        respWrongType(buf);
        return 1;
    }

    size_t ignoredLen;
    char *val = hashHget(&o->hash, req->argv[2], &ignoredLen);
    respInt(buf, val != NULL ? 1 : 0);
    return 1;
}

int cmdSrem(Server *srv, const Request *req, RespBuf *buf) {
    Object *o = storeGet(srv->store, req->argv[1]);
    if (!o) {
        respInt(buf, 0);
        return 1;
    }
    if (o->type != OBJ_SET) {
        respWrongType(buf);
        return 1;
    }
    long long removed = 0;
    for (int i = 2; i < req->argc; i++) { removed += hashHdel(&o->hash, req->argv[i]); }
    /* if empty, potentially delete the key, standard redis behavior */
    if (o->hash.size == 0) { storeDel(srv->store, req->argv[1]); }
    respInt(buf, removed);
    return 1;
}

int cmdScard(Server *srv, const Request *req, RespBuf *buf) {
    Object *o = storeGet(srv->store, req->argv[1]);
    if (!o) {
        respInt(buf, 0);
        return 1;
    }
    if (o->type != OBJ_SET) {
        respWrongType(buf);
        return 1;
    }
    respInt(buf, (long long)o->hash.size);
    return 1;
}

int cmdSunion(Server *srv, const Request *req, RespBuf *buf) {
    for (int i = 1; i < req->argc; i++) {
        Object *o = storeGet(srv->store, req->argv[i]);
        if (o && o->type != OBJ_SET) {
            respWrongType(buf);
            return 1;
        }
    }
    Object *tempSet = objSetNew();
    if (!tempSet) {
        respErr(buf, "OOM");
        return 1;
    }
    for (int i = 1; i < req->argc; i++) {
        Object *o = storeGet(srv->store, req->argv[i]);
        if (!o) continue;
        for (size_t bucket = 0; bucket < o->hash.nbuckets; bucket++) {
            for (struct HEntry *e = o->hash.buckets[bucket]; e; e = e->next) {
                if (hashHset(&tempSet->hash, e->key, "", 0) < 0) {
                    objFree(tempSet);
                    respErr(buf, "OOM");
                    return 1;
                }
            }
        }
    }
    respArrHdr(buf, tempSet->hash.size);
    for (size_t bucket = 0; bucket < tempSet->hash.nbuckets; bucket++) {
        for (struct HEntry *e = tempSet->hash.buckets[bucket]; e; e = e->next) {
            respBulk(buf, e->key, strlen(e->key));
        }
    }
    objFree(tempSet);
    return 1;
}

int cmdSinter(Server *srv, const Request *req, RespBuf *buf) {
    Object *base = storeGet(srv->store, req->argv[1]);
    if (!base) {
        respArrHdr(buf, 0);
        return 1;
    }
    if (base->type != OBJ_SET) {
        respWrongType(buf);
        return 1;
    }
    for (int i = 2; i < req->argc; i++) {
        Object *o = storeGet(srv->store, req->argv[i]);
        if (o && o->type != OBJ_SET) {
            respWrongType(buf);
            return 1;
        }
        if (!o) {
            respArrHdr(buf, 0);
            return 1;
        }
    }
    size_t count = 0;
    for (size_t bucket = 0; bucket < base->hash.nbuckets; bucket++) {
        for (struct HEntry *e = base->hash.buckets[bucket]; e; e = e->next) {
            int inAll = 1;
            for (int i = 2; i < req->argc; i++) {
                Object *o = storeGet(srv->store, req->argv[i]);
                size_t ignored;
                if (hashHget(&o->hash, e->key, &ignored) == NULL) {
                    inAll = 0;
                    break;
                }
            }
            if (inAll) count++;
        }
    }
    respArrHdr(buf, count);
    for (size_t bucket = 0; bucket < base->hash.nbuckets; bucket++) {
        for (struct HEntry *e = base->hash.buckets[bucket]; e; e = e->next) {
            int inAll = 1;
            for (int i = 2; i < req->argc; i++) {
                Object *o = storeGet(srv->store, req->argv[i]);
                size_t ignored;
                if (hashHget(&o->hash, e->key, &ignored) == NULL) {
                    inAll = 0;
                    break;
                }
            }
            if (inAll) respBulk(buf, e->key, strlen(e->key));
        }
    }
    return 1;
}

int cmdSdiff(Server *srv, const Request *req, RespBuf *buf) {
    Object *base = storeGet(srv->store, req->argv[1]);
    if (!base) {
        respArrHdr(buf, 0);
        return 1;
    }
    if (base->type != OBJ_SET) {
        respWrongType(buf);
        return 1;
    }
    for (int i = 2; i < req->argc; i++) {
        Object *o = storeGet(srv->store, req->argv[i]);
        if (o && o->type != OBJ_SET) {
            respWrongType(buf);
            return 1;
        }
    }
    size_t count = 0;
    for (size_t bucket = 0; bucket < base->hash.nbuckets; bucket++) {
        for (struct HEntry *e = base->hash.buckets[bucket]; e; e = e->next) {
            int inNone = 1;
            for (int i = 2; i < req->argc; i++) {
                Object *o = storeGet(srv->store, req->argv[i]);
                if (!o) continue;
                size_t ignored;
                if (hashHget(&o->hash, e->key, &ignored) != NULL) {
                    inNone = 0;
                    break;
                }
            }
            if (inNone) count++;
        }
    }
    respArrHdr(buf, count);
    for (size_t bucket = 0; bucket < base->hash.nbuckets; bucket++) {
        for (struct HEntry *e = base->hash.buckets[bucket]; e; e = e->next) {
            int inNone = 1;
            for (int i = 2; i < req->argc; i++) {
                Object *o = storeGet(srv->store, req->argv[i]);
                if (!o) continue;
                size_t ignored;
                if (hashHget(&o->hash, e->key, &ignored) != NULL) {
                    inNone = 0;
                    break;
                }
            }
            if (inNone) respBulk(buf, e->key, strlen(e->key));
        }
    }
    return 1;
}
