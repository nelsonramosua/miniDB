/* src/Store.h – top-level key-value store
 *
 * The Store is a chained hash table mapping char* keys -> Object* values.
 *
 * Design: separate chaining (linked-list buckets).
 *
 *   WHY CHAINING:
 *     • Deletion is O(1) with no tombstones — crucial for EXPIRE/DEL.
 *     • Resize is straightforward: allocate new bucket array, re-hash
 *       all live entries, free old array.  No "deleted sentinel" logic.
 *     • Cache miss on collision is the same cost as open addressing after
 *       the first probe, and our load factor keeps chains short (avg < 1).
 *
 *   LOAD FACTOR: resize (double) when size / nbuckets > 0.75.
 *   HASH FUNCTION: FNV-1a 32-bit — fast, good distribution for short keys.
 */
#ifndef STORE_H
#define STORE_H

#include "Object.h"
#include <stddef.h>

#define STORE_INIT_BUCKETS 64u
#define STORE_MAX_LOAD 0.75

typedef struct StoreEntry {
    char *key;
    Object *val;
    struct StoreEntry *next;
} StoreEntry;

typedef struct Store {
    StoreEntry **buckets;
    size_t nbuckets;
    size_t size; /* number of live (non-expired) keys */
} Store;

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

Store *storeNew(void);
void storeFree(Store *s);

/* ── Core operations ────────────────────────────────────────────────────── */

/* Takes ownership of val on success (do not free val after). */
int storeSet(Store *s, const char *key, Object *val);

/* Returns NULL if key is absent or expired (lazy deletion performed). */
Object *storeGet(Store *s, const char *key);

/* Returns 1 if a key was deleted, 0 if not found. */
int storeDel(Store *s, const char *key);

/* Rename src key to dst key.
 * Returns: 1=renamed, 0=src missing. Destination is overwritten if present. */
int storeRename(Store *s, const char *src, const char *dst);

/* Unlink an entry and return ownership of its value without freeing it.
 * Returns NULL if not found or expired. */
Object *storeDetach(Store *s, const char *key);

/* Like storeGet but does not expose the object — cheaper for EXISTS. */
int storeExists(Store *s, const char *key);

/* Set expiry to expireMs (monotonic ms).  No-op if key does not exist. */
void storeSetExpire(Store *s, const char *key, int64_t expireMs);

/* ── Bulk operations ────────────────────────────────────────────────────── */

/* Returns heap-allocated array of heap-allocated key strings.
 * Caller must free each key and then the array.
 * Expired keys are skipped (and lazily deleted). */
char **storeKeys(Store *s, size_t *outCount);

/* Walk all buckets and delete entries whose TTL has elapsed.
 * Call this periodically from the event loop (~100ms). */
void storePurgeExpired(Store *s);

/* ── Iteration (used by persistence layer) ──────────────────────────────── */

/* Calls cb(key, val, userdata) for every live entry. */
typedef void (*StoreCb)(const char *key, const Object *val, void *ud);
void storeForeach(Store *s, StoreCb cb, void *userdata);

#endif /* STORE_H */