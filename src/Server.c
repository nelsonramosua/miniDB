/* src/Server.c – server context and periodic maintenance */

#define _POSIX_C_SOURCE 200809L

#include "../headers/Server.h"
#include "../headers/Persist.h" /* persistSave */
#include "../headers/Store.h"   /* storePurgeExpired */
#include "../headers/Object.h"  /* nowMs */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parseIntArg(const char *s, int min, int max, int *out) {
    char *end = NULL;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno == ERANGE || end == s || (end && *end != '\0')) return 0;
    if (v < min || v > max) return 0;
    *out = (int)v;
    return 1;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --port PORT            TCP port to listen on    (default: 6380)\n"
            "  --snapshot PATH        Snapshot file path       (default: miniDB.snap)\n"
            "  --save-interval SECS   Snapshot interval, 0=off (default: 300)\n"
            "  --no-persist           Disable persistence entirely\n"
            "  --help                 Show this message\n",
            prog);
}

ServerConfigParseResult serverConfigParse(int argc, char **argv, ServerConfig *cfg) {
    if (!cfg) return SERVER_CONFIG_ERROR;

    cfg->port = 6380;
    cfg->snapshotPath = "miniDB.snap";
    cfg->snapshotSecs = 300;

    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Unknown option: %s\n", argv[i]);
                usage(argv[0]);
                return SERVER_CONFIG_ERROR;
            }
            if (!parseIntArg(argv[i + 1], 1, 65535, &cfg->port)) {
                fprintf(stderr, "Invalid --port: %s\n", argv[i + 1]);
                return SERVER_CONFIG_ERROR;
            }
            i += 2;
            continue;
        }
        if (strcmp(argv[i], "--snapshot") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Unknown option: %s\n", argv[i]);
                usage(argv[0]);
                return SERVER_CONFIG_ERROR;
            }
            cfg->snapshotPath = argv[i + 1];
            i += 2;
            continue;
        }
        if (strcmp(argv[i], "--save-interval") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Unknown option: %s\n", argv[i]);
                usage(argv[0]);
                return SERVER_CONFIG_ERROR;
            }
            if (!parseIntArg(argv[i + 1], 0, INT_MAX, &cfg->snapshotSecs)) {
                fprintf(stderr, "Invalid --save-interval: %s\n", argv[i + 1]);
                return SERVER_CONFIG_ERROR;
            }
            i += 2;
            continue;
        }
        if (strcmp(argv[i], "--no-persist") == 0) {
            cfg->snapshotPath = NULL;
            cfg->snapshotSecs = 0;
            i++;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return SERVER_CONFIG_HELP;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        usage(argv[0]);
        return SERVER_CONFIG_ERROR;
    }

    return SERVER_CONFIG_OK;
}

Server *serverNew(const ServerConfig *cfg) {
    if (!cfg) return NULL;

    Server *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->store = storeNew();
    if (!s->store) {
        free(s);
        return NULL;
    }

    s->port = cfg->port;
    if (cfg->snapshotPath) {
        s->snapshotPath = strdup(cfg->snapshotPath);
        if (!s->snapshotPath) {
            storeFree(s->store);
            free(s);
            return NULL;
        }
    }

    s->snapshotSecs = cfg->snapshotSecs;
    s->totalCommands = 0;
    s->startMs = nowMs();
    s->lastSnapshotMs = s->startMs;
    return s;
}

void serverFree(Server *s) {
    if (!s) return;
    storeFree(s->store);
    free(s->snapshotPath);
    free(s);
}

/* serverTick — called by the event loop every ~100 ms.
 *
 * Two jobs:
 *   1. Purge expired keys (active expiry, complements lazy expiry in storeGet).
 *   2. Save a snapshot if the configured interval has elapsed.
 *
 * By owning this logic here, net.c knows nothing about persistence or TTLs.
 * Adding a new periodic task means editing this file only. */
void serverTick(Server *s) {
    storePurgeExpired(s->store);

    if (s->snapshotSecs <= 0 || !s->snapshotPath) return;

    int64_t now = nowMs();
    if (now - s->lastSnapshotMs < (int64_t)s->snapshotSecs * 1000) return;

    if (persistSave(s) == 0) fprintf(stderr, "[persist] snapshot saved (%zu keys)\n", s->store->size);
    else fprintf(stderr, "[persist] WARN: snapshot save failed\n");

    s->lastSnapshotMs = now;
}