/* tests/TestProto.c – unit tests for the protocol parser and response builder
 *
 * Compile standalone:
 *   gcc -std=c11 -Wall -Wextra -I../src ../src/proto.c \
 *       testProto.c -o testProto && ./testProto
 */

#include "../headers/Protocol.h"
#include "../headers/Command.h"
#include "../headers/Server.h"
#include "../headers/Store.h"
#include "../headers/Object.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ── Parser tests ───────────────────────────────────────────────────────── */

static int testBasicCommand(void) {
    const char *line = "SET foo bar\r\n";
    Request req;
    size_t consumed;
    int r = protoParse(line, strlen(line), &req, &consumed);
    int ok = r == 1 && req.argc == 3 && strcmp(req.argv[0], "SET") == 0 && strcmp(req.argv[1], "foo") == 0 &&
             strcmp(req.argv[2], "bar") == 0 && consumed == strlen(line);
    protoReqFree(&req);
    return ok;
}

static int testLfOnly(void) {
    const char *line = "GET mykey\n";
    Request req;
    size_t consumed;
    int r = protoParse(line, strlen(line), &req, &consumed);
    int ok = r == 1 && req.argc == 2 && strcmp(req.argv[0], "GET") == 0;
    protoReqFree(&req);
    return ok;
}

static int testIncomplete(void) {
    const char *line = "GET mykey"; /* no newline */
    Request req;
    size_t consumed = 0;
    int r = protoParse(line, strlen(line), &req, &consumed);
    return r == 0 && consumed == 0;
}

static int testQuotedArg(void) {
    const char *line = "SET k \"hello world\"\r\n";
    Request req;
    size_t consumed;
    int r = protoParse(line, strlen(line), &req, &consumed);
    int ok = r == 1 && req.argc == 3 && strcmp(req.argv[2], "hello world") == 0;
    protoReqFree(&req);
    return ok;
}

static int testEmptyLine(void) {
    const char *line = "\r\n";
    Request req;
    size_t consumed;
    int r = protoParse(line, strlen(line), &req, &consumed);
    /* Empty line -> no command parsed (argc==0) -> returns 0 */
    return r == 0;
}

static int testTwoCommandsInBuffer(void) {
    const char *buf = "PING\r\nPING\r\n";
    size_t total = strlen(buf);
    Request req;
    size_t consumed = 0;

    int r1 = protoParse(buf, total, &req, &consumed);
    int ok = r1 == 1 && strcmp(req.argv[0], "PING") == 0;
    protoReqFree(&req);
    size_t off = consumed;

    int r2 = protoParse(buf + off, total - off, &req, &consumed);
    ok &= r2 == 1 && strcmp(req.argv[0], "PING") == 0;
    protoReqFree(&req);
    return ok;
}

static int testRespArrayCommand(void) {
    const char *buf = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
    Request req;
    size_t consumed = 0;

    int r = protoParse(buf, strlen(buf), &req, &consumed);
    int ok = r == 1 && req.argc == 3 && strcmp(req.argv[0], "SET") == 0 && strcmp(req.argv[1], "foo") == 0 &&
             strcmp(req.argv[2], "bar") == 0 && consumed == strlen(buf);
    protoReqFree(&req);
    return ok;
}

static int testRespArrayIncomplete(void) {
    const char *buf = "*2\r\n$3\r\nGET\r\n$3\r\nfo";
    Request req;
    size_t consumed = 0;

    int r = protoParse(buf, strlen(buf), &req, &consumed);
    return r == 0 && consumed == 0;
}

static int testRespPrefixIncomplete(void) {
    const char *buf = "*";
    Request req;
    size_t consumed = 0;

    int r = protoParse(buf, strlen(buf), &req, &consumed);
    return r == 0 && consumed == 0;
}

/* ── Response builder tests ─────────────────────────────────────────────── */

static int testRespOk(void) {
    RespBuf b = {0};
    respOk(&b);
    int ok = b.len == 5 && memcmp(b.data, "+OK\r\n", 5) == 0;
    respBufFree(&b);
    return ok;
}

static int testRespErr(void) {
    RespBuf b = {0};
    respErr(&b, "bad command");
    int ok = strncmp(b.data, "-ERR bad command\r\n", b.len) == 0;
    respBufFree(&b);
    return ok;
}

static int testRespBulk(void) {
    RespBuf b = {0};
    respBulk(&b, "hello", 5);
    int ok = memcmp(b.data, "$5\r\nhello\r\n", b.len) == 0;
    respBufFree(&b);
    return ok;
}

static int testRespNull(void) {
    RespBuf b = {0};
    respNull(&b);
    int ok = memcmp(b.data, "$-1\r\n", 5) == 0;
    respBufFree(&b);
    return ok;
}

static int testRespInt(void) {
    RespBuf b = {0};
    respInt(&b, 42LL);
    int ok = memcmp(b.data, ":42\r\n", b.len) == 0;
    respBufFree(&b);
    return ok;
}

static int testRespArr(void) {
    RespBuf b = {0};
    respArrHdr(&b, 2);
    respBulk(&b, "one", 3);
    respBulk(&b, "two", 3);
    const char *expected = "*2\r\n$3\r\none\r\n$3\r\ntwo\r\n";
    int ok = b.len == strlen(expected) && memcmp(b.data, expected, b.len) == 0;
    respBufFree(&b);
    return ok;
}

/* ── Command-level tests (use the real dispatch layer) ───────────────────── *
 *
 * These tests wire a Server+Store directly to cmdDispatch() so we can
 * verify end-to-end behaviour without a real TCP connection.
 */

/* Helper: dispatch a single inline command and return a heap-allocated copy
 * of the raw RESP response.  Caller frees. */
static char *dispatch(Server *srv, const char *line) {
    Request req;
    size_t consumed = 0;
    RespBuf buf = {0};

    int rc = protoParse(line, strlen(line), &req, &consumed);
    if (rc != 1) return NULL;
    cmdDispatch(srv, &req, &buf);
    protoReqFree(&req);
    /* NUL-terminate for strcmp convenience, transfer ownership to caller */
    char *nb = realloc(buf.data, buf.len + 1);
    if (!nb) {
        respBufFree(&buf);
        return NULL;
    }
    nb[buf.len] = '\0';
    return nb;
}

static Server *mkServer(void) {
    ServerConfig cfg = {0, NULL, 0};
    return serverNew(&cfg);
}

static int testSetOpsProtocol(void) {
    Server *srv = mkServer();
    char *r;
    int ok = 1;

    r = dispatch(srv, "SADD set str1 str2 str1\r\n");
    ok = ok && r && strncmp(r, ":2\r\n", 4) == 0;
    free(r);

    r = dispatch(srv, "SCARD set\r\n");
    ok = ok && r && strncmp(r, ":2\r\n", 4) == 0;
    free(r);

    r = dispatch(srv, "SISMEMBER set str1\r\n");
    ok = ok && r && strncmp(r, ":1\r\n", 4) == 0;
    free(r);

    r = dispatch(srv, "SISMEMBER set str3\r\n");
    ok = ok && r && strncmp(r, ":0\r\n", 4) == 0;
    free(r);

    r = dispatch(srv, "SADD set2 str2 str3\r\n");
    free(r);

    r = dispatch(srv, "SINTER set set2\r\n");
    ok = ok && r && strstr(r, "str2") != NULL && strstr(r, "str1") == NULL;
    free(r);

    r = dispatch(srv, "SDIFF set set2\r\n");
    ok = ok && r && strstr(r, "str1") != NULL && strstr(r, "str2") == NULL;
    free(r);

    serverFree(srv);
    return ok;
}

static int testWrongtypePrefix(void) {
    Server *srv = mkServer();
    char *r = dispatch(srv, "SET strkey hello\r\n");
    free(r);

    /* LPUSH on a string key must return -WRONGTYPE, not -ERR */
    r = dispatch(srv, "LPUSH strkey item\r\n");
    int ok = r && strncmp(r, "-WRONGTYPE", 10) == 0;
    free(r);
    serverFree(srv);
    return ok;
}

static int testIncrby(void) {
    Server *srv = mkServer();
    char *r;

    r = dispatch(srv, "SET counter 10\r\n");
    free(r);
    r = dispatch(srv, "INCRBY counter 5\r\n");
    int ok = r && strcmp(r, ":15\r\n") == 0;
    free(r);

    r = dispatch(srv, "DECRBY counter 3\r\n");
    ok &= r && strcmp(r, ":12\r\n") == 0;
    free(r);

    /* Bad delta */
    r = dispatch(srv, "INCRBY counter abc\r\n");
    ok &= r && strncmp(r, "-ERR", 4) == 0;
    free(r);

    serverFree(srv);
    return ok;
}

static int testPexpirePttl(void) {
    Server *srv = mkServer();
    char *r;

    r = dispatch(srv, "SET tmp data\r\n");
    free(r);

    /* PEXPIRE 5000ms -> should succeed */
    r = dispatch(srv, "PEXPIRE tmp 5000\r\n");
    int ok = r && strcmp(r, ":1\r\n") == 0;
    free(r);

    /* PTTL should return a value between 1 and 5000 */
    r = dispatch(srv, "PTTL tmp\r\n");
    ok &= r && r[0] == ':';
    long long ms = 0;
    if (r) { ms = strtoll(r + 1, NULL, 10); }
    ok &= (ms > 0 && ms <= 5000);
    free(r);

    /* PEXPIRE on missing key returns 0 */
    r = dispatch(srv, "PEXPIRE nokey 1000\r\n");
    ok &= r && strcmp(r, ":0\r\n") == 0;
    free(r);

    serverFree(srv);
    return ok;
}

static int testGetset(void) {
    Server *srv = mkServer();
    char *r;

    /* GETSET on missing key returns nil, then sets it */
    r = dispatch(srv, "GETSET k newval\r\n");
    int ok = r && strcmp(r, "$-1\r\n") == 0;
    free(r);

    r = dispatch(srv, "GET k\r\n");
    ok &= r && strcmp(r, "$6\r\nnewval\r\n") == 0;
    free(r);

    /* GETSET on existing key returns old value */
    r = dispatch(srv, "GETSET k updated\r\n");
    ok &= r && strcmp(r, "$6\r\nnewval\r\n") == 0;
    free(r);

    r = dispatch(srv, "GET k\r\n");
    ok &= r && strcmp(r, "$7\r\nupdated\r\n") == 0;
    free(r);

    serverFree(srv);
    return ok;
}

static int testMsetMget(void) {
    Server *srv = mkServer();
    char *r;

    r = dispatch(srv, "MSET a 1 b 2 c 3\r\n");
    int ok = r && strcmp(r, "+OK\r\n") == 0;
    free(r);

    /* MGET with a miss in the middle */
    r = dispatch(srv, "MGET a missing b\r\n");
    /* Expected: *3\r\n$1\r\n1\r\n$-1\r\n$1\r\n2\r\n */
    ok &= r && strncmp(r, "*3\r\n", 4) == 0;
    ok &= r && strstr(r, "$-1\r\n") != NULL; /* the miss */
    free(r);

    serverFree(srv);
    return ok;
}

static int testIncrOnMissingKey(void) {
    Server *srv = mkServer();
    char *r = dispatch(srv, "INCR newcounter\r\n");
    int ok = r && strcmp(r, ":1\r\n") == 0;
    free(r);
    r = dispatch(srv, "INCR newcounter\r\n");
    ok &= r && strcmp(r, ":2\r\n") == 0;
    free(r);
    serverFree(srv);
    return ok;
}

static int testOddArgPairsRejected(void) {
    Server *srv = mkServer();
    char *r;

    r = dispatch(srv, "MSET a 1 b\r\n");
    int ok = r && strcmp(r, "-ERR wrong number of arguments\r\n") == 0;
    free(r);

    r = dispatch(srv, "HSET h f1 v1 f2\r\n");
    ok &= r && strcmp(r, "-ERR wrong number of arguments\r\n") == 0;
    free(r);

    serverFree(srv);
    return ok;
}

static int testStrictExpireParsing(void) {
    Server *srv = mkServer();
    char *r;

    r = dispatch(srv, "SET k v EX abc\r\n");
    int ok = r && strcmp(r, "-ERR invalid expire time\r\n") == 0;
    free(r);

    r = dispatch(srv, "SET k v PX 0\r\n");
    ok &= r && strcmp(r, "-ERR invalid expire time\r\n") == 0;
    free(r);

    r = dispatch(srv, "SET k v\r\n");
    free(r);

    r = dispatch(srv, "EXPIRE k abc\r\n");
    ok &= r && strcmp(r, "-ERR invalid expire time\r\n") == 0;
    free(r);

    serverFree(srv);
    return ok;
}

static int testAppend(void) {
    Server *srv = mkServer();
    char *r;

    r = dispatch(srv, "APPEND k abc\r\n");
    int ok = r && strcmp(r, ":3\r\n") == 0;
    free(r);

    r = dispatch(srv, "APPEND k de\r\n");
    ok &= r && strcmp(r, ":5\r\n") == 0;
    free(r);

    r = dispatch(srv, "GET k\r\n");
    ok &= r && strcmp(r, "$5\r\nabcde\r\n") == 0;
    free(r);

    serverFree(srv);
    return ok;
}

static int testSetnx(void) {
    Server *srv = mkServer();
    char *r;

    r = dispatch(srv, "SETNX lock 1\r\n");
    int ok = r && strcmp(r, ":1\r\n") == 0;
    free(r);

    r = dispatch(srv, "SETNX lock 2\r\n");
    ok &= r && strcmp(r, ":0\r\n") == 0;
    free(r);

    r = dispatch(srv, "GET lock\r\n");
    ok &= r && strcmp(r, "$1\r\n1\r\n") == 0;
    free(r);

    serverFree(srv);
    return ok;
}

static int testPersist(void) {
    Server *srv = mkServer();
    char *r;

    r = dispatch(srv, "SET temp x PX 5000\r\n");
    free(r);

    r = dispatch(srv, "PERSIST temp\r\n");
    int ok = r && strcmp(r, ":1\r\n") == 0;
    free(r);

    r = dispatch(srv, "TTL temp\r\n");
    ok &= r && strcmp(r, ":-1\r\n") == 0;
    free(r);

    r = dispatch(srv, "PERSIST temp\r\n");
    ok &= r && strcmp(r, ":0\r\n") == 0;
    free(r);

    serverFree(srv);
    return ok;
}

static int testLrange(void) {
    Server *srv = mkServer();
    char *r;

    r = dispatch(srv, "RPUSH q a b c d\r\n");
    free(r);

    r = dispatch(srv, "LRANGE q 1 2\r\n");
    int ok = r && strcmp(r, "*2\r\n$1\r\nb\r\n$1\r\nc\r\n") == 0;
    free(r);

    r = dispatch(srv, "LRANGE q -2 -1\r\n");
    ok &= r && strcmp(r, "*2\r\n$1\r\nc\r\n$1\r\nd\r\n") == 0;
    free(r);

    serverFree(srv);
    return ok;
}

static int testInfo(void) {
    Server *srv = mkServer();
    char *r = dispatch(srv, "INFO\r\n");
    int ok =
        r && r[0] == '$' && strstr(r, "# Server\r\n") && strstr(r, "uptime_in_seconds:") && strstr(r, "# Keyspace\r\n");
    free(r);
    serverFree(srv);
    return ok;
}

static int testStrlenGetdel(void) {
    Server *srv = mkServer();
    char *r;

    r = dispatch(srv, "SET s abc\r\n");
    free(r);

    r = dispatch(srv, "STRLEN s\r\n");
    int ok = r && strcmp(r, ":3\r\n") == 0;
    free(r);

    r = dispatch(srv, "GETDEL s\r\n");
    ok &= r && strcmp(r, "$3\r\nabc\r\n") == 0;
    free(r);

    r = dispatch(srv, "GET s\r\n");
    ok &= r && strcmp(r, "$-1\r\n") == 0;
    free(r);

    r = dispatch(srv, "STRLEN s\r\n");
    ok &= r && strcmp(r, ":0\r\n") == 0;
    free(r);

    serverFree(srv);
    return ok;
}

static int testHkeysHvals(void) {
    Server *srv = mkServer();
    char *r;

    r = dispatch(srv, "HSET h a 1 b 2\r\n");
    free(r);

    r = dispatch(srv, "HKEYS h\r\n");
    int ok = r && strncmp(r, "*2\r\n", 4) == 0 && strstr(r, "$1\r\na\r\n") && strstr(r, "$1\r\nb\r\n");
    free(r);

    r = dispatch(srv, "HVALS h\r\n");
    ok &= r && strncmp(r, "*2\r\n", 4) == 0 && strstr(r, "$1\r\n1\r\n") && strstr(r, "$1\r\n2\r\n");
    free(r);

    r = dispatch(srv, "HKEYS missing\r\n");
    ok &= r && strcmp(r, "*0\r\n") == 0;
    free(r);

    serverFree(srv);
    return ok;
}

static int testInfoCommandCounter(void) {
    Server *srv = mkServer();
    char *r;

    r = dispatch(srv, "PING\r\n");
    free(r);

    r = dispatch(srv, "INFO\r\n");
    int ok = r && strstr(r, "total_commands_processed:");
    free(r);

    serverFree(srv);
    return ok;
}

static int testDbsizeRenameFlushall(void) {
    Server *srv = mkServer();
    char *r;

    r = dispatch(srv, "DBSIZE\r\n");
    int ok = r && strcmp(r, ":0\r\n") == 0;
    free(r);

    r = dispatch(srv, "SET oldkey value\r\n");
    free(r);

    r = dispatch(srv, "DBSIZE\r\n");
    ok &= r && strcmp(r, ":1\r\n") == 0;
    free(r);

    r = dispatch(srv, "RENAME oldkey newkey\r\n");
    ok &= r && strcmp(r, "+OK\r\n") == 0;
    free(r);

    r = dispatch(srv, "GET newkey\r\n");
    ok &= r && strcmp(r, "$5\r\nvalue\r\n") == 0;
    free(r);

    r = dispatch(srv, "GET oldkey\r\n");
    ok &= r && strcmp(r, "$-1\r\n") == 0;
    free(r);

    r = dispatch(srv, "FLUSHALL\r\n");
    ok &= r && strcmp(r, "+OK\r\n") == 0;
    free(r);

    r = dispatch(srv, "DBSIZE\r\n");
    ok &= r && strcmp(r, ":0\r\n") == 0;
    free(r);

    r = dispatch(srv, "RENAME missing whatever\r\n");
    ok &= r && strncmp(r, "-ERR", 4) == 0;
    free(r);

    serverFree(srv);
    return ok;
}

static int testSetNxXxOptions(void) {
    Server *srv = mkServer();
    char *r;

    r = dispatch(srv, "SET k v NX\r\n");
    int ok = r && strcmp(r, "+OK\r\n") == 0;
    free(r);

    r = dispatch(srv, "SET k changed NX\r\n");
    ok &= r && strcmp(r, "$-1\r\n") == 0;
    free(r);

    r = dispatch(srv, "SET missing v XX\r\n");
    ok &= r && strcmp(r, "$-1\r\n") == 0;
    free(r);

    r = dispatch(srv, "SET k changed XX\r\n");
    ok &= r && strcmp(r, "+OK\r\n") == 0;
    free(r);

    r = dispatch(srv, "GET k\r\n");
    ok &= r && strcmp(r, "$7\r\nchanged\r\n") == 0;
    free(r);

    r = dispatch(srv, "SET z one NX PX 2000\r\n");
    ok &= r && strcmp(r, "+OK\r\n") == 0;
    free(r);

    r = dispatch(srv, "PTTL z\r\n");
    ok &= r && r[0] == ':';
    free(r);

    r = dispatch(srv, "SET bad value NX XX\r\n");
    ok &= r && strncmp(r, "-ERR", 4) == 0;
    free(r);

    serverFree(srv);
    return ok;
}

static int testScan(void) {
    Server *srv = mkServer();
    char *r;

    r = dispatch(srv, "MSET scan:a 1 scan:b 2 scan:c 3 no:match 4\r\n");
    free(r);

    r = dispatch(srv, "SCAN 0 MATCH scan:* COUNT 2\r\n");
    int ok = r && strncmp(r, "*2\r\n", 4) == 0 && strstr(r, "scan:");
    free(r);

    r = dispatch(srv, "SCAN 0 MATCH scan:* COUNT 100\r\n");
    ok &= r && strstr(r, "$1\r\n0\r\n") && strstr(r, "scan:a") && strstr(r, "scan:b") && strstr(r, "scan:c");
    free(r);

    r = dispatch(srv, "SCAN badcursor\r\n");
    ok &= r && strncmp(r, "-ERR invalid cursor\r\n", 21) == 0;
    free(r);

    serverFree(srv);
    return ok;
}

int main(void) {
    printf("=== protocol tests ===\n");
    TEST(testBasicCommand());
    TEST(testLfOnly());
    TEST(testIncomplete());
    TEST(testQuotedArg());
    TEST(testEmptyLine());
    TEST(testTwoCommandsInBuffer());
    TEST(testRespArrayCommand());
    TEST(testRespArrayIncomplete());
    TEST(testRespPrefixIncomplete());
    TEST(testRespOk());
    TEST(testRespErr());
    TEST(testRespBulk());
    TEST(testRespNull());
    TEST(testRespInt());
    TEST(testRespArr());
    printf("\n=== command dispatch tests ===\n");
    TEST(testWrongtypePrefix());
    TEST(testIncrby());
    TEST(testPexpirePttl());
    TEST(testGetset());
    TEST(testMsetMget());
    TEST(testIncrOnMissingKey());
    TEST(testOddArgPairsRejected());
    TEST(testStrictExpireParsing());
    TEST(testAppend());
    TEST(testSetnx());
    TEST(testPersist());
    TEST(testLrange());
    TEST(testInfo());
    TEST(testStrlenGetdel());
    TEST(testHkeysHvals());
    TEST(testInfoCommandCounter());
    TEST(testDbsizeRenameFlushall());
    TEST(testSetNxXxOptions());
    TEST(testScan());
    TEST(testSetOpsProtocol());
    printf("\n%d/%d tests passed\n", gPass, gTests);
    return gPass == gTests ? 0 : 1;
}