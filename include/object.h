/* src/object.h – value-type system
 *
 * Every value stored in the KV store is an Object. An Object carries:
 *   - A type tag  (string | list | hash)
 *   - An optional expiry (Unix ms; 0 = never)
 *   - A type-specific payload union
 *
 * Memory model: all payloads are heap-allocated; objFree() recursively
 * frees everything.  Callers transfer ownership to the store on set.
 */
#ifndef OBJECT_H
#define OBJECT_H

#include <stddef.h>
#include <stdint.h>

/* ── Type tag ───────────────────────────────────────────────────────────── */

typedef enum {
    OBJ_STRING = 0,
    OBJ_LIST = 1,
    OBJ_HASH = 2,
} ObjType;

/* ── KVList: intrusive doubly-linked list ───────────────────────────────── */

typedef struct ListNode {
    char *data;
    size_t len;
    struct ListNode *prev;
    struct ListNode *next;
} ListNode;

typedef struct {
    ListNode *head;
    ListNode *tail;
    size_t size;
} KVList;

/* ── KVHash: chained hash table used as a nested map ───────────────────── */

struct HEntry {
    char *key;
    char *val;
    size_t vlen;
    struct HEntry *next;
};

#define KVHASH_INIT_BUCKETS 8u
#define KVHASH_MAX_LOAD 0.75

typedef struct {
    struct HEntry **buckets;
    size_t nbuckets;
    size_t size;
} KVHash;

/* ── Tagged Object ──────────────────────────────────────────────────────── */

typedef struct Object {
    ObjType type;
    int64_t expireMs; /* 0 = no expiry; else monotonic timestamp in milliseconds */
    union {
        struct {
            char *ptr;
            size_t len;
        } str;
        KVList list;
        KVHash hash;
    };
} Object;

/* ── Object lifecycle ───────────────────────────────────────────────────── */

Object *objStrNew(const char *s, size_t len); /* copies s */
Object *objListNew(void);
Object *objHashNew(void);
void objFree(Object *o);
int objExpired(const Object *o); /* 1 if TTL has elapsed, 0 otherwise */

/* ── List operations ────────────────────────────────────────────────────── */

int listRpush(KVList *l, const char *data, size_t len); /* 1=ok, 0=OOM */
int listLpush(KVList *l, const char *data, size_t len); /* 1=ok, 0=OOM */
char *listLpop(KVList *l, size_t *outLen);              /* heap-alloc; caller frees */
char *listRpop(KVList *l, size_t *outLen);

/* ── Hash field operations ──────────────────────────────────────────────── */

/* Returns: 1=new field, 0=updated field, -1=OOM */
int hashHset(KVHash *h, const char *key, const char *val, size_t vlen);
char *hashHget(KVHash *h, const char *key, size_t *outLen); /* points into store */
int hashHdel(KVHash *h, const char *key);                   /* 1=deleted, 0=miss */

/* ── Time helpers ───────────────────────────────────────────────────────── */

int64_t nowMs(void);       /* monotonic clock: CLOCK_MONOTONIC in ms */
int64_t wallClockMs(void); /* wall clock: CLOCK_REALTIME epoch in ms */

#endif /* OBJECT_H */