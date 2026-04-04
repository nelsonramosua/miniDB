/* src/object.c – implementations for the KVObject type system */

#define _POSIX_C_SOURCE 200809L

#include "../include/object.h"
#include "../include/hashutil.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Time ───────────────────────────────────────────────────────────────── */

int64_t nowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

int64_t wallClockMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/* ── Object lifecycle ───────────────────────────────────────────────────── */

Object *objStrNew(const char *s, size_t len) {
    Object *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->type = OBJ_STRING;
    o->expireMs = 0;
    o->str.ptr = malloc(len + 1);
    if (!o->str.ptr) {
        free(o);
        return NULL;
    }
    memcpy(o->str.ptr, s, len);
    o->str.ptr[len] = '\0';
    o->str.len = len;
    return o;
}

Object *objListNew(void) {
    Object *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->type = OBJ_LIST;
    /* list fields zero-initialised by calloc */
    return o;
}

Object *objHashNew(void) {
    Object *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->type = OBJ_HASH;
    o->hash.nbuckets = KVHASH_INIT_BUCKETS;
    o->hash.buckets = calloc(KVHASH_INIT_BUCKETS, sizeof(struct HEntry *));
    if (!o->hash.buckets) {
        free(o);
        return NULL;
    }
    return o;
}

Object *objSetNew(void) {
    Object *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->type = OBJ_SET;
    o->hash.nbuckets = KVHASH_INIT_BUCKETS;
    o->hash.buckets = calloc(KVHASH_INIT_BUCKETS, sizeof(struct HEntry *));
    if (!o->hash.buckets) {
        free(o);
        return NULL;
    }
    return o;
}

int objExpired(const Object *o) {
    if (!o || o->expireMs == 0) return 0;
    return nowMs() >= o->expireMs;
}

static void listFreeAll(KVList *l) {
    ListNode *n = l->head;
    while (n) {
        ListNode *next = n->next;
        free(n->data);
        free(n);
        n = next;
    }
}

static void hashFreeAll(KVHash *h) {
    for (size_t i = 0; i < h->nbuckets; i++) {
        struct HEntry *e = h->buckets[i];
        while (e) {
            struct HEntry *next = e->next;
            free(e->key);
            free(e->val);
            free(e);
            e = next;
        }
    }
    free(h->buckets);
}

void objFree(Object *o) {
    if (!o) return;
    switch (o->type) {
    case OBJ_STRING:
        free(o->str.ptr);
        break;
    case OBJ_LIST:
        listFreeAll(&o->list);
        break;
    case OBJ_HASH:
    case OBJ_SET:
        hashFreeAll(&o->hash);
        break;
    }
    free(o);
}

/* ── List operations ────────────────────────────────────────────────────── */

static ListNode *listNodeNew(const char *data, size_t len) {
    ListNode *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->data = malloc(len + 1);
    if (!n->data) {
        free(n);
        return NULL;
    }
    memcpy(n->data, data, len);
    n->data[len] = '\0';
    n->len = len;
    return n;
}

int listRpush(KVList *l, const char *data, size_t len) {
    ListNode *n = listNodeNew(data, len);
    if (!n) return 0;
    if (!l->tail) {
        l->head = l->tail = n;
    } else {
        n->prev = l->tail;
        l->tail->next = n;
        l->tail = n;
    }
    l->size++;
    return 1;
}

int listLpush(KVList *l, const char *data, size_t len) {
    ListNode *n = listNodeNew(data, len);
    if (!n) return 0;
    if (!l->head) {
        l->head = l->tail = n;
    } else {
        n->next = l->head;
        l->head->prev = n;
        l->head = n;
    }
    l->size++;
    return 1;
}

/* Pop from the head; returns heap-allocated copy (caller frees). */
char *listLpop(KVList *l, size_t *outLen) {
    if (!l->head) {
        if (outLen) *outLen = 0;
        return NULL;
    }
    ListNode *n = l->head;
    l->head = n->next;
    if (l->head) l->head->prev = NULL;
    else l->tail = NULL;
    l->size--;

    char *data = n->data; /* transfer ownership to caller */
    if (outLen) *outLen = n->len;
    free(n);
    return data;
}

char *listRpop(KVList *l, size_t *outLen) {
    if (!l->tail) {
        if (outLen) *outLen = 0;
        return NULL;
    }
    ListNode *n = l->tail;
    l->tail = n->prev;
    if (l->tail) l->tail->next = NULL;
    else l->head = NULL;
    l->size--;

    char *data = n->data;
    if (outLen) *outLen = n->len;
    free(n);
    return data;
}

/* ── Hash field operations ──────────────────────────────────────────────── */

/* Grow the bucket array when load factor exceeds KVHASH_MAX_LOAD.
 * Mirrors storeResize() in store.c — same chaining rehash strategy. */
static void hashResize(KVHash *h, size_t newN) {
    struct HEntry **newBuckets = calloc(newN, sizeof(struct HEntry *));
    if (!newBuckets) return; /* keep old table on OOM */

    for (size_t i = 0; i < h->nbuckets; i++) {
        struct HEntry *e = h->buckets[i];
        while (e) {
            struct HEntry *next = e->next;
            uint32_t idx = hashFnv1a32(e->key) % (uint32_t)newN;
            e->next = newBuckets[idx];
            newBuckets[idx] = e;
            e = next;
        }
    }
    free(h->buckets);
    h->buckets = newBuckets;
    h->nbuckets = newN;
}

int hashHset(KVHash *h, const char *key, const char *val, size_t vlen) {
    uint32_t idx = hashFnv1a32(key) % (uint32_t)h->nbuckets;

    /* Update existing entry if key already present */
    for (struct HEntry *e = h->buckets[idx]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            char *newval = malloc(vlen + 1);
            if (!newval) return -1;
            memcpy(newval, val, vlen);
            newval[vlen] = '\0';
            free(e->val);
            e->val = newval;
            e->vlen = vlen;
            return 0;
        }
    }

    /* Grow before insert if load factor would exceed threshold */
    if ((double)(h->size + 1) / (double)h->nbuckets > KVHASH_MAX_LOAD) hashResize(h, h->nbuckets * 2);

    /* Re-compute idx after potential resize */
    idx = hashFnv1a32(key) % (uint32_t)h->nbuckets;

    /* Insert new entry */
    struct HEntry *e = calloc(1, sizeof(*e));
    if (!e) return -1;
    e->key = strdup(key);
    e->val = malloc(vlen + 1);
    if (!e->key || !e->val) {
        free(e->key);
        free(e->val);
        free(e);
        return -1;
    }
    memcpy(e->val, val, vlen);
    e->val[vlen] = '\0';
    e->vlen = vlen;
    e->next = h->buckets[idx];
    h->buckets[idx] = e;
    h->size++;
    return 1;
}

char *hashHget(KVHash *h, const char *key, size_t *outLen) {
    uint32_t idx = hashFnv1a32(key) % (uint32_t)h->nbuckets;
    for (struct HEntry *e = h->buckets[idx]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            if (outLen) *outLen = e->vlen;
            return e->val;
        }
    }
    return NULL;
}

int hashHdel(KVHash *h, const char *key) {
    uint32_t idx = hashFnv1a32(key) % (uint32_t)h->nbuckets;
    struct HEntry **pp = &h->buckets[idx];
    while (*pp) {
        if (strcmp((*pp)->key, key) == 0) {
            struct HEntry *del = *pp;
            *pp = del->next;
            free(del->key);
            free(del->val);
            free(del);
            h->size--;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}