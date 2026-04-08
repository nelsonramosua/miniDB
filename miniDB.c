/* miniDB.c – entry point
 *
 * Usage: miniDB [--port PORT] [--snapshot PATH] [--save-interval SECS]
 *
 * Defaults: port=6380 (avoid conflict with real Redis)
 *           snapshot=miniDB.snap
 *           save-interval=300 (5 minutes)
 */

#define _POSIX_C_SOURCE 200809L

#include "headers/Server.h"
#include "headers/Net.h"

#include <signal.h>
#include <stdio.h>

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    ServerConfig cfg;
    ServerConfigParseResult parseResult = serverConfigParse(argc, argv, &cfg);
    if (parseResult == SERVER_CONFIG_HELP) return 0;
    if (parseResult == SERVER_CONFIG_ERROR) return 1;

    Server *srv = serverNew(&cfg);
    if (!srv) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }

    netRun(srv);
    serverFree(srv);
    return 0;
}