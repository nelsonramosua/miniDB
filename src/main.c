/* src/main.c – entry point
 *
 * Usage: kvstore [--port PORT] [--snapshot PATH] [--save-interval SECS]
 *
 * Defaults: port=6380 (avoid conflict with real Redis)
 *           snapshot=kvstore.snap
 *           save-interval=300 (5 minutes)
 */

#define _POSIX_C_SOURCE 200809L

#include "../include/server.h"
#include "../include/net.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            ServerConfig cfg;
            serverConfigParse(argc, argv, &cfg);
            return 0;
        }
    }

    ServerConfig cfg;
    if (serverConfigParse(argc, argv, &cfg) != 0) return 1;

    Server *srv = serverNew(&cfg);
    if (!srv) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }

    netRun(srv);
    serverFree(srv);
    return 0;
}