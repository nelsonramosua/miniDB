/* src/protocol.c – request parsing and RESP response generation */

#define _POSIX_C_SOURCE 200809L

#include "../include/protocol.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── RespBuf ────────────────────────────────────────────────────────────── */

#define RESP_GROW_MIN 256

void respBufFree(RespBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void respBufReset(RespBuf *b) {
    b->len = 0;
}

void respRaw(RespBuf *b, const char *data, size_t dlen) {
    if (b->len + dlen > b->cap) {
        size_t newcap = b->cap * 2 + RESP_GROW_MIN + dlen;
        char *nb = realloc(b->data, newcap);
        if (!nb) return;
        b->data = nb;
        b->cap = newcap;
    }
    memcpy(b->data + b->len, data, dlen);
    b->len += dlen;
}

static void respPrintf(RespBuf *b, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) respRaw(b, tmp, (size_t)n);
}

void respOk(RespBuf *b) {
    respRaw(b, "+OK\r\n", 5);
}
void respNull(RespBuf *b) {
    respRaw(b, "$-1\r\n", 5);
}
void respInt(RespBuf *b, long long n) {
    respPrintf(b, ":%lld\r\n", n);
}
void respArrHdr(RespBuf *b, size_t count) {
    respPrintf(b, "*%zu\r\n", count);
}

void respErr(RespBuf *b, const char *msg) {
    respRaw(b, "-ERR ", 5);
    respRaw(b, msg, strlen(msg));
    respRaw(b, "\r\n", 2);
}

void respBulk(RespBuf *b, const char *s, size_t slen) {
    respPrintf(b, "$%zu\r\n", slen);
    respRaw(b, s, slen);
    respRaw(b, "\r\n", 2);
}

/* ── Request parser ─────────────────────────────────────────────────────── */

static const char *skipSpaces(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

static const char *findLf(const char *buf, size_t len) {
    return memchr(buf, '\n', len);
}

static int parseLongStrict(const char *begin, const char *end, long *out) {
    if (begin >= end) return 0;
    size_t n = (size_t)(end - begin);
    char tmp[64];
    if (n >= sizeof(tmp)) return 0;
    memcpy(tmp, begin, n);
    tmp[n] = '\0';
    errno = 0;
    char *tail = NULL;
    long v = strtol(tmp, &tail, 10);
    if (errno == ERANGE || tail == tmp || *tail != '\0') return 0;
    *out = v;
    return 1;
}

static int parseRespRequest(const char *buf, size_t len, Request *req, size_t *consumed) {
    if (buf[0] != '*') return -1;
    if (len < 4) return 0;

    Request tmp;
    memset(&tmp, 0, sizeof(tmp));

    const char *nl = findLf(buf, len);
    if (!nl) return 0;

    const char *lineEnd = (nl > buf && *(nl - 1) == '\r') ? nl - 1 : nl;
    long argcL = 0;
    if (!parseLongStrict(buf + 1, lineEnd, &argcL) || argcL <= 0 || argcL > PROTO_MAX_ARGS) return -1;

    size_t off = (size_t)(nl - buf) + 1;

    for (long i = 0; i < argcL; i++) {
        if (off >= len || buf[off] != '$') {
            protoReqFree(&tmp);
            return off >= len ? 0 : -1;
        }

        const char *bnl = findLf(buf + off, len - off);
        if (!bnl) {
            protoReqFree(&tmp);
            return 0;
        }

        const char *bEnd = (bnl > buf + off && *(bnl - 1) == '\r') ? bnl - 1 : bnl;
        long blenL = 0;
        if (!parseLongStrict(buf + off + 1, bEnd, &blenL) || blenL < 0 || blenL > PROTO_MAX_ARG_LEN) {
            protoReqFree(&tmp);
            return -1;
        }

        size_t blen = (size_t)blenL;
        off = (size_t)(bnl - buf) + 1;

        if (len - off < blen + 1) {
            protoReqFree(&tmp);
            return 0;
        }

        tmp.argv[tmp.argc] = malloc(blen + 1);
        if (!tmp.argv[tmp.argc]) {
            protoReqFree(&tmp);
            return -1;
        }
        memcpy(tmp.argv[tmp.argc], buf + off, blen);
        tmp.argv[tmp.argc][blen] = '\0';
        tmp.argc++;
        off += blen;

        if (off < len && buf[off] == '\r') {
            if (off + 1 >= len) {
                protoReqFree(&tmp);
                return 0;
            }
            if (buf[off + 1] != '\n') {
                protoReqFree(&tmp);
                return -1;
            }
            off += 2;
        } else if (off < len && buf[off] == '\n') {
            off += 1;
        } else {
            protoReqFree(&tmp);
            return -1;
        }
    }

    *req = tmp;
    *consumed = off;
    return 1;
}

int protoParse(const char *buf, size_t len, Request *req, size_t *consumed) {
    if (!len) return 0;
    if (buf[0] == '*') return parseRespRequest(buf, len, req, consumed);

    const char *nl = findLf(buf, len);
    if (!nl) return 0;

    *consumed = (size_t)(nl - buf) + 1;
    const char *end = (nl > buf && *(nl - 1) == '\r') ? nl - 1 : nl;

    memset(req, 0, sizeof(*req));
    const char *p = skipSpaces(buf, end);

    while (p < end && req->argc < PROTO_MAX_ARGS) {
        char tok[PROTO_MAX_ARG_LEN + 1];
        size_t tlen = 0;

        if (*p == '"') {
            p++;
            while (p < end && *p != '"' && tlen < PROTO_MAX_ARG_LEN) {
                if (*p == '\\' && p + 1 < end) p++;
                tok[tlen++] = *p++;
            }
            if (p < end && *p == '"') p++;
        } else {
            while (p < end && !isspace((unsigned char)*p) && tlen < PROTO_MAX_ARG_LEN) tok[tlen++] = *p++;
        }

        tok[tlen] = '\0';
        if (tlen == 0) {
            p = skipSpaces(p, end);
            continue;
        }

        req->argv[req->argc] = malloc(tlen + 1);
        if (!req->argv[req->argc]) {
            protoReqFree(req);
            return -1;
        }
        memcpy(req->argv[req->argc], tok, tlen + 1);
        req->argc++;
        p = skipSpaces(p, end);
    }

    return req->argc > 0 ? 1 : 0;
}

void protoReqFree(Request *req) {
    for (int i = 0; i < req->argc; i++) {
        free(req->argv[i]);
        req->argv[i] = NULL;
    }
    req->argc = 0;
}