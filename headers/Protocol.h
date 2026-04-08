/* headers/Protocol.h – minimal text protocol (RESP-compatible)
 *
 * WIRE FORMAT
 * ───────────
 * Requests: inline text OR RESP array bulk format.
 *   SET mykey hello\r\n
 *   *3\r\n$3\r\nSET\r\n$5\r\nmykey\r\n$5\r\nhello\r\n
 *
 * Responses: RESP subset compatible with redis-cli:
 *   +OK\r\n          simple string
 *   -ERR msg\r\n     error
 *   :42\r\n          integer
 *   $6\r\nhello!\r\n  bulk string
 *   $-1\r\n          null bulk (key not found)
 *   *3\r\n...         array
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>

#define PROTO_MAX_ARGS 64
#define PROTO_MAX_ARG_LEN 4096
#define PROTO_MAX_LINE_LEN (PROTO_MAX_ARGS * (PROTO_MAX_ARG_LEN + 1))

/* ── Parsed request ─────────────────────────────────────────────────────── */

typedef struct {
    int argc;
    char *argv[PROTO_MAX_ARGS]; /* heap-allocated; free with protoReqFree() */
} Request;

/* Parse one CRLF/LF-terminated line (inline) or a RESP array (*N\r\n…).
 * Returns  1 on success (req populated),
 *          0 if no complete command yet (need more data),
 *         -1 on parse error.
 * *consumed is set to bytes consumed from buf. */
int protoParse(const char *buf, size_t len, Request *req, size_t *consumed);
void protoReqFree(Request *req);

/* ── Response output buffer ─────────────────────────────────────────────── *
 *
 * RespBuf owns a heap-allocated byte array that grows on demand.
 * Every resp* function appends to it; net.c drains it to the socket.
 *
 * WHY A STRUCT:
 *   The previous API passed (char **data, size_t *len, size_t *cap) to
 *   every handler — three parameters for a single concept, repeated 30+
 *   times.  One struct pointer cuts each signature by two parameters and
 *   makes the ownership model explicit.
 *
 * Initialise with: RespBuf buf = {0};
 * Caller must call: respBufFree(&buf) when done.
 */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} RespBuf;

void respBufFree(RespBuf *b);  /* frees data, zeroes struct    */
void respBufReset(RespBuf *b); /* len=0, keeps allocation       */

/* ── Response builders ──────────────────────────────────────────────────── */

void respOk(RespBuf *b);
void respErr(RespBuf *b, const char *msg);
void respInt(RespBuf *b, long long n);
void respBulk(RespBuf *b, const char *s, size_t slen);
void respNull(RespBuf *b);
void respArrHdr(RespBuf *b, size_t count);
void respRaw(RespBuf *b, const char *data, size_t dlen); /* append raw bytes */

#endif /* PROTOCOL_H */