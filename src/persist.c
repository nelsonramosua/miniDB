/* src/persist.c – snapshot persistence (save + load) */

#define _POSIX_C_SOURCE 200809L

#include "../include/persist.h"
#include "../include/store.h"
#include "../include/object.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Portable little-endian I/O helpers ─────────────────────────────────── */

static int writeU8(FILE *f, uint8_t v) {
    return fwrite(&v, 1, 1, f) == 1 ? 0 : -1;
}
static int writeU32(FILE *f, uint32_t v) {
    uint8_t b[4] = {v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF};
    return fwrite(b, 1, 4, f) == 4 ? 0 : -1;
}
static int writeU64(FILE *f, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (i * 8));
    return fwrite(b, 1, 8, f) == 8 ? 0 : -1;
}
static int writeI64(FILE *f, int64_t v) {
    return writeU64(f, (uint64_t)v);
}
static int writeBlob(FILE *f, const char *data, uint32_t len) {
    if (writeU32(f, len)) return -1;
    if (len && fwrite(data, 1, len, f) != len) return -1;
    return 0;
}

static int readU8(FILE *f, uint8_t *v) {
    return fread(v, 1, 1, f) == 1 ? 0 : -1;
}
static int readU32(FILE *f, uint32_t *v) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return -1;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}
static int readU64(FILE *f, uint64_t *v) {
    uint8_t b[8];
    if (fread(b, 1, 8, f) != 8) return -1;
    *v = 0;
    for (int i = 0; i < 8; i++) *v |= ((uint64_t)b[i]) << (i * 8);
    return 0;
}
static int readI64(FILE *f, int64_t *v) {
    uint64_t u;
    if (readU64(f, &u)) return -1;
    *v = (int64_t)u;
    return 0;
}
/* Reads blob: allocates *out (caller frees), sets *outLen. */
static int readBlob(FILE *f, char **out, uint32_t *outLen) {
    uint32_t len;
    if (readU32(f, &len)) return -1;
    *out = malloc(len + 1);
    if (!*out) return -1;
    if (len && fread(*out, 1, len, f) != len) {
        free(*out);
        *out = NULL;
        return -1;
    }
    (*out)[len] = '\0';
    *outLen = len;
    return 0;
}

/* ── Save ───────────────────────────────────────────────────────────────── */

typedef struct {
    FILE *f;
    int err;
    size_t count;
    int64_t wallOffsetMs;
} SaveCtx;

static void saveEntry(const char *key, const Object *o, void *ud) {
    SaveCtx *ctx = ud;
    if (ctx->err) return;

#define CHK(expr)                                                                                                      \
    do {                                                                                                               \
        if ((expr)) {                                                                                                  \
            ctx->err = 1;                                                                                              \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

    CHK(writeU8(ctx->f, (uint8_t)o->type));
    CHK(writeBlob(ctx->f, key, (uint32_t)strlen(key)));
    int64_t expireOnDisk = 0;
    if (o->expireMs > 0) expireOnDisk = o->expireMs + ctx->wallOffsetMs;
    CHK(writeI64(ctx->f, expireOnDisk));

    switch (o->type) {
    case OBJ_STRING:
        CHK(writeBlob(ctx->f, o->str.ptr, (uint32_t)o->str.len));
        break;

    case OBJ_LIST: {
        CHK(writeU32(ctx->f, (uint32_t)o->list.size));
        for (ListNode *n = o->list.head; n; n = n->next) CHK(writeBlob(ctx->f, n->data, (uint32_t)n->len));
        break;
    }

    case OBJ_HASH: {
        KVHash *h = (KVHash *)&o->hash;
        CHK(writeU32(ctx->f, (uint32_t)h->size));
        for (size_t i = 0; i < h->nbuckets; i++)
            for (struct HEntry *e = h->buckets[i]; e; e = e->next) {
                CHK(writeBlob(ctx->f, e->key, (uint32_t)strlen(e->key)));
                CHK(writeBlob(ctx->f, e->val, (uint32_t)e->vlen));
            }
        break;
    }
    }
#undef CHK
    ctx->count++;
}

int persistSave(const Server *srv) {
    if (!srv->snapshotPath) return 0;

    /* Write to tmp file first, then rename for atomicity */
    size_t tmplen = strlen(srv->snapshotPath) + 5;
    char *tmppath = malloc(tmplen);
    if (!tmppath) return -1;
    snprintf(tmppath, tmplen, "%s.tmp", srv->snapshotPath);

    FILE *f = fopen(tmppath, "wb");
    if (!f) {
        free(tmppath);
        return -1;
    }

    /* Write header (entry count placeholder) */
    if (fwrite(SNAPSHOT_MAGIC, 1, 4, f) != 4) goto err;
    uint32_t ver = SNAPSHOT_VERSION;
    uint8_t vb[4] = {ver & 0xFF, (ver >> 8) & 0xFF, (ver >> 16) & 0xFF, (ver >> 24) & 0xFF};
    if (fwrite(vb, 1, 4, f) != 4) goto err;

    /* Reserve 8 bytes for entry count; we'll seek back to fill it. */
    long countPos = ftell(f);
    if (countPos < 0) goto err;
    if (writeU64(f, 0)) goto err;

    /* Serialize all entries */
    int64_t wallNow = wallClockMs();
    int64_t monoNow = nowMs();
    SaveCtx ctx = {f, 0, 0, wallNow - monoNow};
    storeForeach(srv->store, saveEntry, &ctx);
    if (ctx.err) goto err;

    /* Patch count */
    if (fseek(f, countPos, SEEK_SET)) goto err;
    if (writeU64(f, (uint64_t)ctx.count)) goto err;
    if (fseek(f, 0, SEEK_END)) goto err;

    if (fflush(f)) goto err;
    /* fdatasync for durability before rename */
    if (fdatasync(fileno(f))) goto err;
    if (fclose(f)) {
        f = NULL;
        goto err;
    }
    f = NULL;

    if (rename(tmppath, srv->snapshotPath)) goto err;
    free(tmppath);
    return 0;

err:
    if (f) fclose(f);
    remove(tmppath);
    free(tmppath);
    return -1;
}

/* ── Load ───────────────────────────────────────────────────────────────── */

int persistLoad(Server *srv) {
    if (!srv->snapshotPath) return 0;

    FILE *f = fopen(srv->snapshotPath, "rb");
    if (!f) { return (errno == ENOENT) ? 1 : -1; }

    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, SNAPSHOT_MAGIC, 4) != 0) goto err;

    uint32_t ver;
    if (readU32(f, &ver) || ver != SNAPSHOT_VERSION) goto err;

    uint64_t count;
    if (readU64(f, &count)) goto err;

    int64_t wallNow = wallClockMs();
    int64_t monoNow = nowMs();

    for (uint64_t i = 0; i < count; i++) {
        uint8_t type;
        char *key = NULL;
        uint32_t klen = 0;
        int64_t exp = 0;
        Object *o = NULL;

        if (readU8(f, &type)) goto err;
        if (readBlob(f, &key, &klen)) goto err;
        if (readI64(f, &exp)) {
            free(key);
            goto err;
        }

        switch ((ObjType)type) {
        case OBJ_STRING: {
            char *val;
            uint32_t vlen;
            if (readBlob(f, &val, &vlen)) {
                free(key);
                goto err;
            }
            o = objStrNew(val, vlen);
            free(val);
            break;
        }
        case OBJ_LIST: {
            uint32_t n;
            if (readU32(f, &n)) {
                free(key);
                goto err;
            }
            o = objListNew();
            if (!o) {
                free(key);
                goto err;
            }
            for (uint32_t j = 0; j < n; j++) {
                char *elem;
                uint32_t elen;
                if (readBlob(f, &elem, &elen)) {
                    free(key);
                    objFree(o);
                    goto err;
                }
                if (!listRpush(&o->list, elem, elen)) {
                    free(elem);
                    free(key);
                    objFree(o);
                    goto err;
                }
                free(elem);
            }
            break;
        }
        case OBJ_HASH: {
            uint32_t n;
            if (readU32(f, &n)) {
                free(key);
                goto err;
            }
            o = objHashNew();
            if (!o) {
                free(key);
                goto err;
            }
            for (uint32_t j = 0; j < n; j++) {
                char *hk, *hv;
                uint32_t hkl, hvl;
                if (readBlob(f, &hk, &hkl)) {
                    free(key);
                    objFree(o);
                    goto err;
                }
                if (readBlob(f, &hv, &hvl)) {
                    free(key);
                    free(hk);
                    objFree(o);
                    goto err;
                }
                if (hashHset(&o->hash, hk, hv, hvl) < 0) {
                    free(key);
                    free(hk);
                    free(hv);
                    objFree(o);
                    goto err;
                }
                free(hk);
                free(hv);
            }
            break;
        }
        default:
            free(key);
            goto err;
        }

        if (!o) {
            free(key);
            goto err;
        }
        if (exp > 0) o->expireMs = exp - wallNow + monoNow;
        else o->expireMs = 0;
        /* Skip expired entries on load — don't pollute the store. */
        if (objExpired(o)) {
            free(key);
            objFree(o);
            continue;
        }
        storeSet(srv->store, key, o);
        free(key);
    }

    fclose(f);
    return 0;
err:
    fclose(f);
    return -1;
}