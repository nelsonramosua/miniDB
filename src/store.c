/* src/store.c – chained hash table implementation */

#define _POSIX_C_SOURCE 200809L

#include "../include/store.h"
#include "../include/hashutil.h"

#include <stdlib.h>
#include <string.h>

/* ── Internal helpers ───────────────────────────────────────────────────── */

static size_t bucketIdx(const Store *s, const char *key) {
    return (size_t)(hashFnv1a32(key) % (uint32_t)s->nbuckets);
}

/* Unlink and free one StoreEntry (but not val — caller decides). */
static void entryFree(StoreEntry *e, int freeVal) {
    free(e->key);
    if (freeVal) objFree(e->val);
    free(e);
}

/* Resize: allocate a new bucket array of size newN and rehash. */
static void storeResize(Store *s, size_t newN) {
    StoreEntry **newBuckets = calloc(newN, sizeof(StoreEntry *));
    if (!newBuckets) return; /* keep old table on OOM */

    for (size_t i = 0; i < s->nbuckets; i++) {
        StoreEntry *e = s->buckets[i];
        while (e) {
            StoreEntry *next = e->next;
            size_t idx = (size_t)(hashFnv1a32(e->key) % (uint32_t)newN);
            e->next = newBuckets[idx];
            newBuckets[idx] = e;
            e = next;
        }
    }
    free(s->buckets);
    s->buckets = newBuckets;
    s->nbuckets = newN;
}

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

Store *storeNew(void) {
    Store *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->nbuckets = STORE_INIT_BUCKETS;
    s->buckets = calloc(s->nbuckets, sizeof(StoreEntry *));
    if (!s->buckets) {
        free(s);
        return NULL;
    }
    return s;
}

void storeFree(Store *s) {
    if (!s) return;
    for (size_t i = 0; i < s->nbuckets; i++) {
        StoreEntry *e = s->buckets[i];
        while (e) {
            StoreEntry *next = e->next;
            entryFree(e, 1);
            e = next;
        }
    }
    free(s->buckets);
    free(s);
}

/* ── Core operations ────────────────────────────────────────────────────── */

int storeSet(Store *s, const char *key, Object *val) {
    /* Fast path: update existing key without affecting load factor. */
    size_t idx = bucketIdx(s, key);
    for (StoreEntry *e = s->buckets[idx]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            objFree(e->val);
            e->val = val;
            return 1;
        }
    }

    /* Insert path only: grow before adding a new key. */
    if ((double)(s->size + 1) / (double)s->nbuckets > STORE_MAX_LOAD) {
        storeResize(s, s->nbuckets * 2);
        idx = bucketIdx(s, key);
    }

    /* Insert new entry at head of chain */
    StoreEntry *e = calloc(1, sizeof(*e));
    if (!e) return 0;
    e->key = strdup(key);
    if (!e->key) {
        free(e);
        return 0;
    }
    e->val = val;
    e->next = s->buckets[idx];
    s->buckets[idx] = e;
    s->size++;
    return 1;
}

/* Lazy expiry: delete the entry and return NULL if it has expired. */
Object *storeGet(Store *s, const char *key) {
    size_t idx = bucketIdx(s, key);
    StoreEntry **pp = &s->buckets[idx];

    while (*pp) {
        if (strcmp((*pp)->key, key) == 0) {
            StoreEntry *e = *pp;
            if (objExpired(e->val)) {
                /* Lazy delete */
                *pp = e->next;
                entryFree(e, 1);
                s->size--;
                return NULL;
            }
            return e->val;
        }
        pp = &(*pp)->next;
    }
    return NULL;
}

int storeDel(Store *s, const char *key) {
    size_t idx = bucketIdx(s, key);
    StoreEntry **pp = &s->buckets[idx];

    while (*pp) {
        if (strcmp((*pp)->key, key) == 0) {
            StoreEntry *del = *pp;
            *pp = del->next;
            entryFree(del, 1);
            s->size--;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

Object *storeDetach(Store *s, const char *key) {
    size_t idx = bucketIdx(s, key);
    StoreEntry **pp = &s->buckets[idx];

    while (*pp) {
        if (strcmp((*pp)->key, key) == 0) {
            StoreEntry *del = *pp;
            *pp = del->next;
            Object *o = del->val;
            free(del->key);
            free(del);
            s->size--;
            if (objExpired(o)) {
                objFree(o);
                return NULL;
            }
            return o;
        }
        pp = &(*pp)->next;
    }
    return NULL;
}


int storeExists(Store *s, const char *key) {
    return storeGet(s, key) != NULL;
}

void storeSetExpire(Store *s, const char *key, int64_t expireMs) {
    Object *o = storeGet(s, key);
    if (o) o->expireMs = expireMs;
}

/* ── Bulk operations ────────────────────────────────────────────────────── */

char **storeKeys(Store *s, size_t *outCount) {
    /* Over-allocate: at most s->size entries */
    char **arr = malloc(s->size * sizeof(char *));
    if (!arr && s->size) {
        *outCount = 0;
        return NULL;
    }

    size_t n = 0;
    for (size_t i = 0; i < s->nbuckets; i++) {
        StoreEntry **pp = &s->buckets[i];
        while (*pp) {
            if (objExpired((*pp)->val)) {
                /* Lazy delete expired entries */
                StoreEntry *del = *pp;
                *pp = del->next;
                entryFree(del, 1);
                s->size--;
            } else {
                arr[n++] = strdup((*pp)->key);
                pp = &(*pp)->next;
            }
        }
    }
    *outCount = n;
    return arr;
}

void storePurgeExpired(Store *s) {
    for (size_t i = 0; i < s->nbuckets; i++) {
        StoreEntry **pp = &s->buckets[i];
        while (*pp) {
            if (objExpired((*pp)->val)) {
                StoreEntry *del = *pp;
                *pp = del->next;
                entryFree(del, 1);
                s->size--;
            } else {
                pp = &(*pp)->next;
            }
        }
    }
}

void storeForeach(Store *s, StoreCb cb, void *userdata) {
    for (size_t i = 0; i < s->nbuckets; i++) {
        for (StoreEntry *e = s->buckets[i]; e; e = e->next) {
            if (!objExpired(e->val)) cb(e->key, e->val, userdata);
        }
    }
}