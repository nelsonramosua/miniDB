/* metrics.c — system metrics collector / viewer backed by miniDB
 *
 * Compile:   gcc -std=c11 -Wall -Wextra -O2 -o metrics metrics.c
 * Run:       ./metrics --daemon [--interval 2] [--history 60]
 *            ./metrics --view
 *            ./metrics --watch
 *
 * No dependencies outside of POSIX + Linux /proc.
 * Includes a hand-rolled RESP client over plain TCP.
 * 
 * Data model in miniDB
 * --------------------
 *   metrics:cpu:current     HASH   {user_pct, sys_pct, idle_pct, iowait_pct, timestamp}
 *   metrics:mem:current     HASH   {total_mb, used_mb, available_mb, used_pct, timestamp}
 *   metrics:cpu:history     LIST   "HH:MM:SS user sys iowait" (newest first, capped)
 *   metrics:mem:history     LIST   "HH:MM:SS used_mb used_pct" (newest first, capped)
 *   metrics:samples         INCR   total samples collected
 *
 * Architecture notes
 * ------------------
 *   CPU usage is derived by diffing two consecutive /proc/stat reads
 *   (same technique used by top / vmstat).  Memory is a direct read
 *   of /proc/meminfo.  Both are pushed to miniDB every --interval seconds.
 *   The --daemon mode is designed to run as a background process while
 *   --view and --watch read back and render what it stored.
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ── buffered TCP connection ────────────────────────────────────────────────── */

#define CONN_RBUF 8192

typedef struct {
    int fd;
    char buf[CONN_RBUF];
    size_t len;
    size_t pos;
} Conn;

static Conn *connOpen(const char *host, int port) {
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return NULL;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return NULL;
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd);
        freeaddrinfo(res);
        return NULL;
    }
    freeaddrinfo(res);

    Conn *c = calloc(1, sizeof(*c));
    if (!c) {
        close(fd);
        return NULL;
    }
    c->fd = fd;
    return c;
}

static void connClose(Conn *c) {
    if (!c) return;
    close(c->fd);
    free(c);
}

static int connRefill(Conn *c) {
    ssize_t n = read(c->fd, c->buf, CONN_RBUF);
    if (n <= 0) return (int)n;
    c->pos = 0;
    c->len = (size_t)n;
    return (int)n;
}

static int connReadbyte(Conn *c, char *out) {
    if (c->pos >= c->len && connRefill(c) <= 0) return -1;
    *out = c->buf[c->pos++];
    return 0;
}

/* Read one CRLF-terminated line into out (NUL-terminated, \r\n stripped). */
static int connReadline(Conn *c, char *out, size_t maxlen) {
    size_t i = 0;
    char ch;
    while (i + 1 < maxlen) {
        if (connReadbyte(c, &ch) < 0) return -1;
        if (ch == '\n') {
            out[i] = '\0';
            return (int)i;
        }
        if (ch == '\r') continue;
        out[i++] = ch;
    }
    return -1;
}

/* Read exactly n bytes into out. */
static int connReadn(Conn *c, char *out, size_t n) {
    size_t done = 0;
    while (done < n) {
        if (c->pos >= c->len && connRefill(c) <= 0) return -1;
        size_t take = c->len - c->pos;
        if (take > n - done) take = n - done;
        if (out) memcpy(out + done, c->buf + c->pos, take);
        c->pos += take;
        done += take;
    }
    return 0;
}

/* ── RESP writer ────────────────────────────────────────────────────────────── */

/*
 * Sends a RESP array command:
 *   *N\r\n
 *   $len(argv[0])\r\n argv[0]\r\n
 *   ...
 */
static int respSend(Conn *c, int argc, const char *argv[]) {
    /* Compute exact needed size */
    size_t sz = 32;
    for (int i = 0; i < argc; i++) sz += strlen(argv[i]) + 32;

    char *buf = malloc(sz);
    if (!buf) return -1;

    size_t off = (size_t)snprintf(buf, sz, "*%d\r\n", argc);
    for (int i = 0; i < argc; i++) {
        size_t arglen = strlen(argv[i]);
        off += (size_t)snprintf(buf + off, sz - off, "$%zu\r\n", arglen);
        memcpy(buf + off, argv[i], arglen);
        off += arglen;
        buf[off++] = '\r';
        buf[off++] = '\n';
    }

    ssize_t sent = write(c->fd, buf, off);
    free(buf);
    return sent == (ssize_t)off ? 0 : -1;
}

/* ── RESP reader ────────────────────────────────────────────────────────────── */

/*
 * Reads one bulk string value (the $N\r\n header was already consumed by
 * the caller via conn_readline).  Reads exactly len data bytes + CRLF.
 * Writes to out if out != NULL and len < outlen.
 */
static int readBulkBody(Conn *c, long len, char *out, size_t outlen) {
    if (len < 0) return 0;               /* $-1 null bulk — nothing to read */
    char *tmp = malloc((size_t)len + 2); /* +2 for trailing CRLF             */
    if (!tmp) return -1;
    if (connReadn(c, tmp, (size_t)len + 2) < 0) {
        free(tmp);
        return -1;
    }
    if (out && (size_t)len < outlen) {
        memcpy(out, tmp, (size_t)len);
        out[len] = '\0';
    }
    free(tmp);
    return 0;
}

/*
 * Reads and drains one complete RESP value (any type).
 * If out/outlen are non-zero, attempts to copy string value there.
 */
static int respRead(Conn *c, char *out, size_t outlen) {
    char line[512];
    if (connReadline(c, line, sizeof(line)) < 0) return -1;
    char type = line[0];
    const char *rest = line + 1;
    switch (type) {
    case '+': /* simple string */
    case ':': /* integer      */
        if (out) {
            size_t n = strlen(rest);
            if (n >= outlen) n = outlen - 1;
            memcpy(out, rest, n);
            out[n] = '\0';
        }
        return 0;
    case '-': /* error */
        fprintf(stderr, "[resp] server error: %s\n", rest);
        return -1;
    case '$': { /* bulk string */
        long len = strtol(rest, NULL, 10);
        return readBulkBody(c, len, out, outlen);
    }
    case '*': { /* array — drain all sub-elements */
        long count = strtol(rest, NULL, 10);
        for (long i = 0; i < count; i++)
            if (respRead(c, NULL, 0) < 0) return -1;
        return 0;
    }
    default:
        return -1;
    }
}

/* Send command and drain the response. */
static int cmd_skip(Conn *c, int argc, const char *argv[]) {
    if (respSend(c, argc, argv) < 0) return -1;
    return respRead(c, NULL, 0);
}

/*
 * Reads a HGETALL response (array of alternating field/value bulk strings).
 * Fills keys[i] (max 64 bytes) and vals[i] (max 256 bytes).
 * Returns number of pairs, or -1 on error.
 */
static int respHgetall(Conn *c, char keys[][64], char vals[][256], int max) {
    char line[512];
    if (connReadline(c, line, sizeof(line)) < 0) return -1;
    if (line[0] != '*') return -1;

    long count = strtol(line + 1, NULL, 10);
    int pairs = (int)(count / 2);
    if (pairs > max) pairs = max;

    for (int i = 0; i < pairs; i++) {
        /* key */
        if (connReadline(c, line, sizeof(line)) < 0) return -1;
        readBulkBody(c, strtol(line + 1, NULL, 10), keys[i], 64);
        /* value */
        if (connReadline(c, line, sizeof(line)) < 0) return -1;
        readBulkBody(c, strtol(line + 1, NULL, 10), vals[i], 256);
    }
    /* drain any surplus elements */
    for (long i = (long)pairs * 2; i < count; i++) respRead(c, NULL, 0);
    return pairs;
}

/* ── CPU statistics ─────────────────────────────────────────────────────────── */

typedef struct {
    long long user, nice, sys, idle, iowait, irq, softirq, steal;
} CpuRaw;

typedef struct {
    double userPct, sysPct, iowaitPct, idlePct;
} CpuPct;

static int readCpuRaw(CpuRaw *out) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    /* "cpu" aggregate line is always the first */
    int r = fscanf(f, "cpu %lld %lld %lld %lld %lld %lld %lld %lld", &out->user, &out->nice, &out->sys, &out->idle,
                   &out->iowait, &out->irq, &out->softirq, &out->steal);
    fclose(f);
    return r == 8 ? 0 : -1;
}

/*
 * Compute per-category percentages from two consecutive /proc/stat snapshots.
 * This is the standard technique used by top(1): delta_category / delta_total.
 */
static CpuPct cpuDiff(const CpuRaw *a, const CpuRaw *b) {
    CpuPct p = {0};
    long long dUser = (b->user + b->nice) - (a->user + a->nice);
    long long dSys = b->sys - a->sys;
    long long dIdle = b->idle - a->idle;
    long long dIowait = b->iowait - a->iowait;
    long long dIrq = (b->irq + b->softirq) - (a->irq + a->softirq);
    long long total = dUser + dSys + dIdle + dIowait + dIrq;
    if (total <= 0) return p;
    p.userPct = 100.0 * (double)dUser / (double)total;
    p.sysPct = 100.0 * (double)dSys / (double)total;
    p.idlePct = 100.0 * (double)dIdle / (double)total;
    p.iowaitPct = 100.0 * (double)dIowait / (double)total;
    return p;
}

/* ── memory statistics ─────────────────────────────────────────────────────── */

typedef struct {
    long long totalKb, availableKb;
} MemStat;

static int readMem(MemStat *out) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    out->totalKb = out->availableKb = 0;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        long long v;
        if (sscanf(line, "MemTotal: %lld kB", &v) == 1) out->totalKb = v;
        else if (sscanf(line, "MemAvailable: %lld kB", &v) == 1) out->availableKb = v;
        if (out->totalKb && out->availableKb) break;
    }
    fclose(f);
    return out->totalKb > 0 ? 0 : -1;
}

/* ── miniDB store ───────────────────────────────────────────────────────────── */

/*
 * Push one sample to miniDB.
 *
 * Each call issues:
 *   5× HSET metrics:cpu:current  (overwrites the "current" snapshot)
 *   5× HSET metrics:mem:current
 *   1× LPUSH + LTRIM  metrics:cpu:history
 *   1× LPUSH + LTRIM  metrics:mem:history
 *   1× INCR  metrics:samples
 *
 * The LPUSH+LTRIM pattern is the standard rolling-window idiom:
 * push to head, trim tail — O(1) amortised, bounded memory.
 */
static void storeMetrics(Conn *db, const CpuPct *cpu, const MemStat *mem, int historyLen) {
    char buf[64];
    char ts[16];
    time_t t = time(NULL);
    strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));

#define HSET4(key, field, val)                                                                                         \
    do {                                                                                                               \
        const char *_a[] = {"HSET", key, field, val};                                                                  \
        cmd_skip(db, 4, _a);                                                                                           \
    } while (0)

    /* CPU current */
    snprintf(buf, sizeof(buf), "%.1f", cpu->userPct);
    HSET4("metrics:cpu:current", "user_pct", buf);
    snprintf(buf, sizeof(buf), "%.1f", cpu->sysPct);
    HSET4("metrics:cpu:current", "sys_pct", buf);
    snprintf(buf, sizeof(buf), "%.1f", cpu->idlePct);
    HSET4("metrics:cpu:current", "idle_pct", buf);
    snprintf(buf, sizeof(buf), "%.1f", cpu->iowaitPct);
    HSET4("metrics:cpu:current", "iowait_pct", buf);
    HSET4("metrics:cpu:current", "timestamp", ts);

    /* Memory current */
    long long usedKb = mem->totalKb - mem->availableKb;
    double usedPct = 100.0 * (double)usedKb / (double)mem->totalKb;

    snprintf(buf, sizeof(buf), "%lld", mem->totalKb / 1024);
    HSET4("metrics:mem:current", "total_mb", buf);
    snprintf(buf, sizeof(buf), "%lld", usedKb / 1024);
    HSET4("metrics:mem:current", "used_mb", buf);
    snprintf(buf, sizeof(buf), "%lld", mem->availableKb / 1024);
    HSET4("metrics:mem:current", "available_mb", buf);
    snprintf(buf, sizeof(buf), "%.1f", usedPct);
    HSET4("metrics:mem:current", "used_pct", buf);
    HSET4("metrics:mem:current", "timestamp", ts);

#undef HSET4

    /* Rolling histories */
    char entry[80];
    char hlim[16];
    snprintf(hlim, sizeof(hlim), "%d", historyLen - 1);

    snprintf(entry, sizeof(entry), "%s %.1f %.1f %.1f", ts, cpu->userPct, cpu->sysPct, cpu->iowaitPct);
    {
        const char *a[] = {"LPUSH", "metrics:cpu:history", entry};
        cmd_skip(db, 3, a);
    }
    {
        const char *a[] = {"LTRIM", "metrics:cpu:history", "0", hlim};
        cmd_skip(db, 4, a);
    }

    snprintf(entry, sizeof(entry), "%s %lld %.1f", ts, usedKb / 1024, usedPct);
    {
        const char *a[] = {"LPUSH", "metrics:mem:history", entry};
        cmd_skip(db, 3, a);
    }
    {
        const char *a[] = {"LTRIM", "metrics:mem:history", "0", hlim};
        cmd_skip(db, 4, a);
    }

    /* Lifetime counter */
    {
        const char *a[] = {"INCR", "metrics:samples"};
        cmd_skip(db, 2, a);
    }
}

/* ── terminal rendering ─────────────────────────────────────────────────────── */

#define BAR_WIDTH 36

static void printBar(double pct, const char *label) {
    int filled = (int)(pct * BAR_WIDTH / 100.0 + 0.5);
    if (filled > BAR_WIDTH) filled = BAR_WIDTH;
    if (filled < 0) filled = 0;
    printf("  %-10s [", label);
    for (int i = 0; i < BAR_WIDTH; i++) putchar(i < filled ? '#' : ' ');
    printf("] %5.1f%%\n", pct);
}

static void viewMetrics(Conn *db) {
    char keys[12][64], vals[12][256];

    /* ── fetch CPU ── */
    {
        const char *a[] = {"HGETALL", "metrics:cpu:current"};
        respSend(db, 2, a);
    }
    int cp = respHgetall(db, keys, vals, 12);

    double cpuUser = 0, cpuSys = 0, cpuIo = 0, cpuIdle = 0;
    char cpuTs[32] = "—";
    for (int i = 0; i < cp; i++) {
        if (strcmp(keys[i], "user_pct") == 0) cpuUser = atof(vals[i]);
        if (strcmp(keys[i], "sys_pct") == 0) cpuSys = atof(vals[i]);
        if (strcmp(keys[i], "iowait_pct") == 0) cpuIo = atof(vals[i]);
        if (strcmp(keys[i], "idle_pct") == 0) cpuIdle = atof(vals[i]);
        if (strcmp(keys[i], "timestamp") == 0) snprintf(cpuTs, sizeof(cpuTs), "%.31s", vals[i]);
    }

    /* ── fetch memory ── */
    {
        const char *a[] = {"HGETALL", "metrics:mem:current"};
        respSend(db, 2, a);
    }
    int mp = respHgetall(db, keys, vals, 12);

    long long totalMb = 0, usedMb = 0, availMb = 0;
    double memPct = 0;
    char memTs[32] = "—";
    for (int i = 0; i < mp; i++) {
        if (strcmp(keys[i], "total_mb") == 0) totalMb = atoll(vals[i]);
        if (strcmp(keys[i], "used_mb") == 0) usedMb = atoll(vals[i]);
        if (strcmp(keys[i], "available_mb") == 0) availMb = atoll(vals[i]);
        if (strcmp(keys[i], "used_pct") == 0) memPct = atof(vals[i]);
        if (strcmp(keys[i], "timestamp") == 0) snprintf(memTs, sizeof(memTs), "%.31s", vals[i]);
    }

    /* ── fetch sample counter ── */
    char samples[24] = "0";
    {
        const char *a[] = {"GET", "metrics:samples"};
        respSend(db, 2, a);
    }
    respRead(db, samples, sizeof(samples));

    /* ── render ── */
    printf("\n");
    printf("  ┌── CPU  @ %s ──────────────────────────────────────────┐\n", cpuTs);
    printBar(cpuUser, "user");
    printBar(cpuSys, "system");
    printBar(cpuIo, "iowait");
    printBar(cpuIdle, "idle");
    printf("\n");
    printf("  ├── Memory  @ %s ─────────────────────────────────────────┤\n", memTs);
    printBar(memPct, "used");
    printf("  %lld MB used  /  %lld MB free  /  %lld MB total\n", usedMb, availMb, totalMb);
    printf("\n");
    printf("  samples collected: %s\n\n", samples);
}

/* ── signal handling ────────────────────────────────────────────────────────── */

static volatile int gStop = 0;
static void onSignal(int sig) {
    (void)sig;
    gStop = 1;
}

/* ── usage ──────────────────────────────────────────────────────────────────── */

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "Modes (default: --daemon):\n"
            "  --daemon        collect and store metrics in the background\n"
            "  --view          fetch and print current metrics once, then exit\n"
            "  --watch         collect + render continuously (clears terminal each tick)\n"
            "\n"
            "Options:\n"
            "  --host HOST       miniDB host          (default: localhost)\n"
            "  --port PORT       miniDB port          (default: 6380)\n"
            "  --interval SECS   collection interval  (default: 2)\n"
            "  --history N       rolling window size  (default: 60 samples)\n"
            "  --help\n",
            prog);
}

/* ── main ───────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    const char *host = "localhost";
    int port = 6380;
    int interval = 2;
    int historyLen = 60;
    enum { MODE_DAEMON, MODE_VIEW, MODE_WATCH } mode = MODE_DAEMON;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--daemon")) mode = MODE_DAEMON;
        else if (!strcmp(argv[i], "--view")) mode = MODE_VIEW;
        else if (!strcmp(argv[i], "--watch")) mode = MODE_WATCH;
        else if (!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--history") && i + 1 < argc) historyLen = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    Conn *db = connOpen(host, port);
    if (!db) {
        fprintf(stderr,
                "cannot connect to miniDB at %s:%d\n"
                "start with: ./kvstore --port %d\n",
                host, port, port);
        return 1;
    }

    /* Verify the connection with PING before doing any real work */
    {
        const char *a[] = {"PING"};
        respSend(db, 1, a);
    }
    char pong[16] = {0};
    if (respRead(db, pong, sizeof(pong)) < 0) {
        fprintf(stderr, "miniDB did not respond to PING\n");
        connClose(db);
        return 1;
    }

    /* --view: one-shot read + render, then exit */
    if (mode == MODE_VIEW) {
        viewMetrics(db);
        connClose(db);
        return 0;
    }

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    /*
     * Take an initial CPU snapshot, then sleep one interval before the
     * first delta is meaningful.  This mirrors top(1) behaviour.
     */
    CpuRaw prev = {0}, curr = {0};
    if (readCpuRaw(&prev) < 0) {
        fprintf(stderr, "cannot read /proc/stat\n");
        connClose(db);
        return 1;
    }
    sleep((unsigned)interval);

    if (mode == MODE_DAEMON)
        fprintf(stderr,
                "[metrics] collecting every %ds  (history %d samples)  "
                "— Ctrl-C to stop\n",
                interval, historyLen);

    while (!gStop) {
        if (readCpuRaw(&curr) < 0) {
            sleep(1);
            continue;
        }
        CpuPct cpu = cpuDiff(&prev, &curr);
        prev = curr;

        MemStat mem = {0};
        if (readMem(&mem) < 0) {
            sleep(1);
            continue;
        }

        storeMetrics(db, &cpu, &mem, historyLen);

        if (mode == MODE_WATCH) {
            printf("\033[H\033[J"); /* ANSI: move to top-left + clear screen */
            viewMetrics(db);
            fflush(stdout);
        }

        sleep((unsigned)interval);
    }

    if (mode == MODE_DAEMON) fprintf(stderr, "\n[metrics] stopped\n");
    connClose(db);
    return 0;
}