/* tests/TestStore.c – unit tests for the hash table and object layer
 *
 * Compile standalone:
 *   gcc -std=c11 -Wall -Wextra -I../src ../src/Store.c ../src/Object.c \
 *       TestStore.c -o testStore && ./testStore
 */

#include "../headers/Store.h"
#include "../headers/Object.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Test harness ───────────────────────────────────────────────────────── */

static int gTests = 0, gPass = 0;

#define TEST(name)                                                                                                     \
    do {                                                                                                               \
        gTests++;                                                                                                      \
        int _ok = (name);                                                                                              \
        if (_ok) {                                                                                                     \
            gPass++;                                                                                                   \
            printf("  PASS  %s\n", #name);                                                                             \
        } else printf("  FAIL  %s  (line %d)\n", #name, __LINE__);                                                     \
    } while (0)

/* ── Helpers ────────────────────────────────────────────────────────────── */

static Object *mkstr(const char *s) {
    return objStrNew(s, strlen(s));
}

/* ── Tests ──────────────────────────────────────────────────────────────── */

static int testSetGetBasic(void) {
    Store *s = storeNew();
    storeSet(s, "foo", mkstr("bar"));
    Object *o = storeGet(s, "foo");
    int ok = o && o->type == OBJ_STRING && strcmp(o->str.ptr, "bar") == 0;
    storeFree(s);
    return ok;
}

static int testOverwrite(void) {
    Store *s = storeNew();
    storeSet(s, "k", mkstr("first"));
    storeSet(s, "k", mkstr("second"));
    Object *o = storeGet(s, "k");
    int ok = o && strcmp(o->str.ptr, "second") == 0 && s->size == 1;
    storeFree(s);
    return ok;
}

static int testDel(void) {
    Store *s = storeNew();
    storeSet(s, "a", mkstr("1"));
    storeSet(s, "b", mkstr("2"));
    int deleted = storeDel(s, "a");
    int ok = deleted == 1 && storeGet(s, "a") == NULL && s->size == 1;
    storeFree(s);
    return ok;
}

static int testDelMiss(void) {
    Store *s = storeNew();
    int r = storeDel(s, "nonexistent");
    storeFree(s);
    return r == 0;
}

static int testExists(void) {
    Store *s = storeNew();
    storeSet(s, "x", mkstr("v"));
    int ok = storeExists(s, "x") == 1 && storeExists(s, "y") == 0;
    storeFree(s);
    return ok;
}

static int testExpireLazy(void) {
    Store *s = storeNew();
    storeSet(s, "temp", mkstr("data"));
    /* Set expire to 1ms in the past */
    storeSetExpire(s, "temp", nowMs() - 1);
    Object *o = storeGet(s, "temp"); /* lazy delete triggers here */
    int ok = o == NULL && s->size == 0;
    storeFree(s);
    return ok;
}

static int testExpirePurge(void) {
    Store *s = storeNew();
    storeSet(s, "k1", mkstr("v1"));
    storeSet(s, "k2", mkstr("v2"));
    storeSetExpire(s, "k1", nowMs() - 1);
    storePurgeExpired(s);
    int ok = s->size == 1 && storeExists(s, "k2");
    storeFree(s);
    return ok;
}

static int testKeys(void) {
    Store *s = storeNew();
    storeSet(s, "alpha", mkstr("1"));
    storeSet(s, "beta", mkstr("2"));
    storeSet(s, "gamma", mkstr("3"));
    size_t n;
    char **keys = storeKeys(s, &n);
    int ok = (n == 3);
    for (size_t i = 0; i < n; i++) free(keys[i]);
    free(keys);
    storeFree(s);
    return ok;
}

static int testResize(void) {
    /* Insert enough keys to trigger at least one resize */
    Store *s = storeNew();
    char key[32];
    for (int i = 0; i < 200; i++) {
        snprintf(key, sizeof(key), "key%d", i);
        storeSet(s, key, mkstr("val"));
    }
    int ok = (s->size == 200);
    /* Spot-check a few */
    snprintf(key, sizeof(key), "key%d", 0);
    ok &= storeExists(s, key);
    snprintf(key, sizeof(key), "key%d", 199);
    ok &= storeExists(s, key);
    storeFree(s);
    return ok;
}

static int testListOps(void) {
    Object *o = objListNew();
    listRpush(&o->list, "a", 1);
    listRpush(&o->list, "b", 1);
    listLpush(&o->list, "z", 1);
    /* list: z a b */
    size_t len;
    char *popped = listLpop(&o->list, &len);
    int ok = popped && strcmp(popped, "z") == 0 && o->list.size == 2;
    free(popped);
    popped = listRpop(&o->list, &len);
    ok &= popped && strcmp(popped, "b") == 0;
    free(popped);
    objFree(o);
    return ok;
}

static int testHashOps(void) {
    Object *o = objHashNew();
    hashHset(&o->hash, "name", "alice", 5);
    hashHset(&o->hash, "age", "30", 2);
    size_t vlen;
    char *v = hashHget(&o->hash, "name", &vlen);
    int ok = v && strcmp(v, "alice") == 0 && o->hash.size == 2;
    hashHdel(&o->hash, "age");
    ok &= (o->hash.size == 1);
    objFree(o);
    return ok;
}

static int testSetOps(void) {
    Object *o = objSetNew();
    hashHset(&o->hash, "alice", "", 0);
    hashHset(&o->hash, "bob", "", 0);
    size_t vlen;
    char *v = hashHget(&o->hash, "alice", &vlen);
    int ok = v && o->hash.size == 2;
    hashHdel(&o->hash, "bob");
    ok &= (o->hash.size == 1);
    objFree(o);
    return ok;
}

static int testHashResize(void) {
    /* Insert enough fields to cross the 0.75 load factor at 8 buckets (>6).
     * Before fix: all 100 fields crammed into 8 buckets (avg chain 12.5).
     * After fix: hash resizes automatically, keeping chains short. */
    Object *o = objHashNew();
    char key[32], val[32];
    for (int i = 0; i < 100; i++) {
        snprintf(key, sizeof(key), "field%d", i);
        snprintf(val, sizeof(val), "value%d", i);
        hashHset(&o->hash, key, val, strlen(val));
    }
    int ok = (o->hash.size == 100);
    /* Load factor must be <= threshold after resize */
    ok &= ((double)o->hash.size / (double)o->hash.nbuckets <= KVHASH_MAX_LOAD + 0.01);
    /* Spot-check correctness after resize */
    size_t vlen;
    char *got = hashHget(&o->hash, "field0", &vlen);
    ok &= got && strcmp(got, "value0") == 0;
    got = hashHget(&o->hash, "field99", &vlen);
    ok &= got && strcmp(got, "value99") == 0;
    objFree(o);
    return ok;
}

static int testPexpire(void) {
    Store *s = storeNew();
    storeSet(s, "k", mkstr("v"));
    /* Set expiry 50ms into the future */
    storeSetExpire(s, "k", nowMs() + 50);
    int ok = storeExists(s, "k"); /* should still be alive */
    /* Force-expire by setting to past */
    storeSetExpire(s, "k", nowMs() - 1);
    ok &= !storeExists(s, "k"); /* should be gone now */
    storeFree(s);
    return ok;
}

typedef struct {
    size_t count;
    int sawA;
    int sawB;
} ForeachCtx;

static void foreachCb(const char *key, const Object *val, void *ud) {
    ForeachCtx *ctx = (ForeachCtx *)ud;
    ctx->count++;
    if (strcmp(key, "a") == 0 && val->type == OBJ_STRING) ctx->sawA = 1;
    if (strcmp(key, "b") == 0 && val->type == OBJ_STRING) ctx->sawB = 1;
}

static int testStoreForeach(void) {
    Store *s = storeNew();
    storeSet(s, "a", mkstr("1"));
    storeSet(s, "b", mkstr("2"));
    storeSet(s, "expired", mkstr("x"));
    storeSetExpire(s, "expired", nowMs() - 1);

    ForeachCtx ctx = {0};
    storeForeach(s, foreachCb, &ctx);

    int ok = (ctx.count == 2) && ctx.sawA && ctx.sawB;
    storeFree(s);
    return ok;
}

int main(void) {
    printf("=== store / object tests ===\n");
    TEST(testSetGetBasic());
    TEST(testOverwrite());
    TEST(testDel());
    TEST(testDelMiss());
    TEST(testExists());
    TEST(testExpireLazy());
    TEST(testExpirePurge());
    TEST(testKeys());
    TEST(testResize());
    TEST(testListOps());
    TEST(testHashOps());
    TEST(testSetOps());
    TEST(testHashResize());
    TEST(testPexpire());
    TEST(testStoreForeach());
    printf("\n%d/%d tests passed\n", gPass, gTests);
    return gPass == gTests ? 0 : 1;
}