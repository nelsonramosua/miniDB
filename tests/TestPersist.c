/* tests/TestPersist.c – unit tests for snapshot persistence */

#include "../headers/Object.h"
#include "../headers/Persist.h"
#include "../headers/Server.h"
#include "../headers/Store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int gTests = 0;
static int gPass = 0;

#define TEST(name)                                                                                                     \
    do {                                                                                                               \
        gTests++;                                                                                                      \
        int _ok = (name);                                                                                              \
        if (_ok) {                                                                                                     \
            gPass++;                                                                                                   \
            printf("  PASS  %s\n", #name);                                                                             \
        } else printf("  FAIL  %s  (line %d)\n", #name, __LINE__);                                                     \
    } while (0)

static Object *mkStr(const char *s) {
    return objStrNew(s, strlen(s));
}

static int testRoundTrip(void) {
    char tmpl[128];
    snprintf(tmpl, sizeof(tmpl), "/tmp/minidb_test_%ld_%lld.snap", (long)getpid(), (long long)wallClockMs());
    unlink(tmpl);

    ServerConfig cfg = {0, tmpl, 0};
    Server *srv = serverNew(&cfg);
    if (!srv) {
        unlink(tmpl);
        return 0;
    }

    storeSet(srv->store, "k", mkStr("hello"));

    Object *lst = objListNew();
    if (!lst) {
        serverFree(srv);
        unlink(tmpl);
        return 0;
    }
    if (!listRpush(&lst->list, "a", 1) || !listRpush(&lst->list, "b", 1)) {
        objFree(lst);
        serverFree(srv);
        unlink(tmpl);
        return 0;
    }
    storeSet(srv->store, "lst", lst);

    Object *h = objHashNew();
    if (!h) {
        serverFree(srv);
        unlink(tmpl);
        return 0;
    }
    if (hashHset(&h->hash, "f", "v", 1) < 0) {
        objFree(h);
        serverFree(srv);
        unlink(tmpl);
        return 0;
    }
    storeSet(srv->store, "h", h);
    Object *st = objSetNew();
    hashHset(&st->hash, "m1", "", 0);
    storeSet(srv->store, "st", st);

    storeSet(srv->store, "ttl", mkStr("x"));
    storeSetExpire(srv->store, "ttl", nowMs() + 5000);

    if (persistSave(srv) != 0) {
        serverFree(srv);
        unlink(tmpl);
        return 0;
    }

    storeFree(srv->store);
    srv->store = storeNew();
    if (!srv->store) {
        serverFree(srv);
        unlink(tmpl);
        return 0;
    }

    if (persistLoad(srv) != 0) {
        serverFree(srv);
        unlink(tmpl);
        return 0;
    }

    Object *okStr = storeGet(srv->store, "k");
    Object *okList = storeGet(srv->store, "lst");
    Object *okHash = storeGet(srv->store, "h");
    Object *okSet = storeGet(srv->store, "st");
    Object *okTtl = storeGet(srv->store, "ttl");

    int ok = okStr && okStr->type == OBJ_STRING && strcmp(okStr->str.ptr, "hello") == 0;
    ok &= okList && okList->type == OBJ_LIST && okList->list.size == 2;
    ok &= okHash && okHash->type == OBJ_HASH;
    ok &= okSet && okSet->type == OBJ_SET && okSet->hash.size == 1;
    ok &= okTtl && okTtl->expireMs > nowMs();

    serverFree(srv);
    unlink(tmpl);
    return ok;
}

int main(void) {
    printf("=== persistence tests ===\n");
    TEST(testRoundTrip());
    printf("\n%d/%d tests passed\n", gPass, gTests);
    return gPass == gTests ? 0 : 1;
}
