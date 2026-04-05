/* src/cmd_conn.c – connection and server-management commands
 *
 * Commands: PING QUIT CLIENT CONFIG COMMAND
 *
 * CONFIG, CLIENT, and COMMAND are stub implementations that satisfy
 * redis-cli and redis-benchmark preflight probes without implementing
 * the full Redis semantics.
 */

#define _POSIX_C_SOURCE 200809L

#include "../../include/commandInternal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

int cmdPing(Server *srv, const Request *req, RespBuf *buf) {
    (void)srv;
    if (req->argc == 2) {
        respBulk(buf, req->argv[1], strlen(req->argv[1]));
        return 1;
    }
    respRaw(buf, "+PONG\r\n", 7);
    return 1;
}

int cmdHello(Server *srv, const Request *req, RespBuf *buf) {
    (void)srv;
    // HELLO with no args or HELLO 2 -> we can tolerate, respond with inline OK
    if (req->argc == 1 || (req->argc >= 2 && req->argv[1][0] == '2')) {
        // Minimal RESP2 HELLO response that satisfies redis-cli 8
        respArrHdr(buf, 0);
        return 1;
    }
    // HELLO 3 or unknown version -> reject and close
    respErr(buf, "NOPROTO unsupported protocol version");
    return 0;   // <- return 0 = close connection, not 1
}

/* Returns 0 to signal the event loop to close this connection. */
int cmdQuit(Server *srv, const Request *req, RespBuf *buf) {
    (void)srv;
    (void)req;
    respOk(buf);
    return 0;
}

int cmdClient(Server *srv, const Request *req, RespBuf *buf) {
    (void)srv;
    (void)req;
    respOk(buf);
    return 1;
}

int cmdCommand(Server *srv, const Request *req, RespBuf *buf) {
    (void)srv;
    (void)req;
    /* Return empty array: we don't expose full command metadata. */
    respArrHdr(buf, 0);
    return 1;
}

int cmdConfig(Server *srv, const Request *req, RespBuf *buf) {
    (void)srv;
    if (req->argc < 2) {
        respOk(buf);
        return 1;
    }

    /* CONFIG GET param … -> return param/empty-value pairs (benchmark probe) */
    if (strcasecmp(req->argv[1], "GET") == 0 && req->argc >= 3) {
        size_t pairs = (size_t)(req->argc - 2);
        respArrHdr(buf, pairs * 2);
        for (int i = 2; i < req->argc; i++) {
            respBulk(buf, req->argv[i], strlen(req->argv[i]));
            respBulk(buf, "", 0);
        }
        return 1;
    }

    /* CONFIG RESETSTAT and anything else: be permissive */
    respOk(buf);
    return 1;
}

int cmdInfo(Server *srv, const Request *req, RespBuf *buf) {
    (void)req;

    RespBuf info = {0};
    char line[128];
    double load = 0.0;
    if (srv->store->nbuckets > 0) load = (double)srv->store->size / (double)srv->store->nbuckets;

    respRaw(&info, "# Server\r\n", 10);
    respRaw(&info, "version:0.1.0\r\n", 15);
    int n = snprintf(line, sizeof(line), "uptime_in_seconds:%lld\r\n", (long long)((nowMs() - srv->startMs) / 1000));
    if (n <= 0 || n >= (int)sizeof(line)) {
        respBufFree(&info);
        respErr(buf, "internal error");
        return 1;
    }
    respRaw(&info, line, (size_t)n);

    n = snprintf(line, sizeof(line), "total_commands_processed:%lld\r\n", (long long)srv->totalCommands);
    if (n <= 0 || n >= (int)sizeof(line)) {
        respBufFree(&info);
        respErr(buf, "internal error");
        return 1;
    }
    respRaw(&info, line, (size_t)n);

    respRaw(&info, "\r\n# Keyspace\r\n", 14);
    n = snprintf(line, sizeof(line), "keys:%zu\r\n", srv->store->size);
    if (n <= 0 || n >= (int)sizeof(line)) {
        respBufFree(&info);
        respErr(buf, "internal error");
        return 1;
    }
    respRaw(&info, line, (size_t)n);

    n = snprintf(line, sizeof(line), "buckets:%zu\r\n", srv->store->nbuckets);
    if (n <= 0 || n >= (int)sizeof(line)) {
        respBufFree(&info);
        respErr(buf, "internal error");
        return 1;
    }
    respRaw(&info, line, (size_t)n);

    n = snprintf(line, sizeof(line), "load_factor:%.2f\r\n", load);
    if (n <= 0 || n >= (int)sizeof(line)) {
        respBufFree(&info);
        respErr(buf, "internal error");
        return 1;
    }
    respRaw(&info, line, (size_t)n);

    respBulk(buf, info.data, info.len);
    respBufFree(&info);
    return 1;
}