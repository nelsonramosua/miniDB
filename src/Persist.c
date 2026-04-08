/* src/Persist.c – snapshot persistence (save + load) */

#define _POSIX_C_SOURCE 200809L

#include "../headers/Persist.h"
#include "../headers/Store.h"
#include "../headers/Object.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PERSIST_MAX_BLOB_SIZE (16u * 1024u * 1024u)
#define PERSIST_MAX_ENTRIES (1000000u)
#define PERSIST_MAX_COLLECTION_ENTRIES (1000000u)

static int snapshotPathIsSafe(const char *path) {
    if (!path || path[0] == '\0') return 0;

    size_t len = strlen(path);
    if (len == 0 || len >= PATH_MAX - 5) return 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)path[i];
        if (ch < 32 || ch == 127) return 0;
    }

    /* Reject parent-directory traversal segments. */
    if (strstr(path, "../") || strstr(path, "/..") || strcmp(path, "..") == 0) return 0;

    return 1;
}

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
    if (len > PERSIST_MAX_BLOB_SIZE) return -1;
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

static int saveStringPayload(FILE *f, const Object *o) {
    return writeBlob(f, o->str.ptr, (uint32_t)o->str.len);
}

static int saveListPayload(FILE *f, const Object *o) {
    if (writeU32(f, (uint32_t)o->list.size)) return -1;
    for (ListNode *n = o->list.head; n; n = n->next)
        if (writeBlob(f, n->data, (uint32_t)n->len)) return -1;
    return 0;
}

static int saveHashPayload(FILE *f, const Object *o) {
    const KVHash *h = &o->hash;
    if (writeU32(f, (uint32_t)h->size)) return -1;
    for (size_t i = 0; i < h->nbuckets; i++) {
        for (struct HEntry *e = h->buckets[i]; e; e = e->next) {
            if (writeBlob(f, e->key, (uint32_t)strlen(e->key))) return -1;
            if (writeBlob(f, e->val, (uint32_t)e->vlen)) return -1;
        }
    }
    return 0;
}

static int saveObjectPayload(FILE *f, const Object *o) {
    if (o->type == OBJ_STRING) return saveStringPayload(f, o);
    if (o->type == OBJ_LIST) return saveListPayload(f, o);
    if (o->type == OBJ_HASH || o->type == OBJ_SET) return saveHashPayload(f, o);
    return -1;
}

static void saveEntry(const char *key, const Object *o, void *ud) {
    SaveCtx *ctx = ud;
    if (ctx->err) return;

    if (writeU8(ctx->f, (uint8_t)o->type)) {
        ctx->err = 1;
        return;
    }
    if (writeBlob(ctx->f, key, (uint32_t)strlen(key))) {
        ctx->err = 1;
        return;
    }

    int64_t expireOnDisk = 0;
    if (o->expireMs > 0) expireOnDisk = o->expireMs + ctx->wallOffsetMs;
    if (writeI64(ctx->f, expireOnDisk)) {
        ctx->err = 1;
        return;
    }

    if (saveObjectPayload(ctx->f, o)) {
        ctx->err = 1;
        return;
    }

    ctx->count++;
}

static Object *loadStringObject(FILE *f) {
    char *val = NULL;
    uint32_t vlen = 0;
    if (readBlob(f, &val, &vlen)) return NULL;
    Object *o = objStrNew(val, vlen);
    free(val);
    return o;
}

static Object *loadListObject(FILE *f) {
    uint32_t n = 0;
    if (readU32(f, &n) || n > PERSIST_MAX_COLLECTION_ENTRIES) return NULL;

    Object *o = objListNew();
    if (!o) return NULL;

    for (uint32_t j = 0; j < n; j++) {
        char *elem = NULL;
        uint32_t elen = 0;
        if (readBlob(f, &elem, &elen)) {
            objFree(o);
            return NULL;
        }
        if (!listRpush(&o->list, elem, elen)) {
            free(elem);
            objFree(o);
            return NULL;
        }
        free(elem);
    }

    return o;
}

static Object *loadHashObject(FILE *f, ObjType type) {
    uint32_t n = 0;
    if (readU32(f, &n) || n > PERSIST_MAX_COLLECTION_ENTRIES) return NULL;

    Object *o = (type == OBJ_HASH) ? objHashNew() : objSetNew();
    if (!o) return NULL;

    for (uint32_t j = 0; j < n; j++) {
        char *hk = NULL;
        char *hv = NULL;
        uint32_t hkl = 0;
        uint32_t hvl = 0;
        if (readBlob(f, &hk, &hkl) || readBlob(f, &hv, &hvl)) {
            free(hk);
            free(hv);
            objFree(o);
            return NULL;
        }
        if (hashHset(&o->hash, hk, hv, hvl) < 0) {
            free(hk);
            free(hv);
            objFree(o);
            return NULL;
        }
        free(hk);
        free(hv);
    }

    return o;
}

static Object *loadObjectPayload(FILE *f, uint8_t type) {
    if (type == OBJ_STRING) return loadStringObject(f);
    if (type == OBJ_LIST) return loadListObject(f);
    if (type == OBJ_HASH) return loadHashObject(f, OBJ_HASH);
    if (type == OBJ_SET) return loadHashObject(f, OBJ_SET);
    return NULL;
}

int persistSave(const Server *srv) {
    if (!srv->snapshotPath) return 0;
    if (!snapshotPathIsSafe(srv->snapshotPath)) {
        errno = EINVAL;
        return -1;
    }

    /* Write to tmp file first, then rename for atomicity */
    size_t tmplen = strlen(srv->snapshotPath) + 5;
    char *tmppath = malloc(tmplen);
    if (!tmppath) return -1;
    snprintf(tmppath, tmplen, "%s.tmp", srv->snapshotPath);

    int tfd = open(tmppath, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (tfd < 0) {
        free(tmppath);
        return -1;
    }

    FILE *f = fdopen(tfd, "wb");
    if (!f) {
        close(tfd);
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
    if (!snapshotPathIsSafe(srv->snapshotPath)) {
        errno = EINVAL;
        return -1;
    }

    FILE *f = fopen(srv->snapshotPath, "rb");
    if (!f) { return (errno == ENOENT) ? 1 : -1; }

    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, SNAPSHOT_MAGIC, 4) != 0) goto err;

    uint32_t ver;
    if (readU32(f, &ver) || ver != SNAPSHOT_VERSION) goto err;

    uint64_t count;
    if (readU64(f, &count)) goto err;
    if (count > PERSIST_MAX_ENTRIES) goto err;

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

        o = loadObjectPayload(f, type);
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