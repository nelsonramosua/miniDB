/* headers/Server.h – server context
 *
 * Server is the top-level object that owns the Store and all configuration.
 * It is passed to every command handler so they can reach the store without
 * global state.
 *
 * serverTick() is the single hook the event loop calls on its maintenance
 * interval (~100 ms).  This keeps net.c free of any knowledge about
 * persistence or TTL expiry — it just calls one function and moves on.
 */
#ifndef SERVER_H
#define SERVER_H

#include "Store.h"
#include <stdint.h>
#include <stddef.h>

typedef struct Server {
    Store *store;
    int port;
    char *snapshotPath;
    int snapshotSecs;
    int64_t totalCommands;  /* count of dispatched valid commands */
    int64_t startMs;        /* monotonic start time for uptime */
    int64_t lastSnapshotMs; /* tracks interval internally */
} Server;

typedef struct {
    int port;
    const char *snapshotPath;
    int snapshotSecs;
} ServerConfig;

typedef enum {
    SERVER_CONFIG_OK = 0,
    SERVER_CONFIG_HELP = 1,
    SERVER_CONFIG_ERROR = 2,
} ServerConfigParseResult;

/* Fill cfg with defaults and parse argv.
 * Returns parse status: OK, HELP shown, or ERROR (already printed). */
ServerConfigParseResult serverConfigParse(int argc, char **argv, ServerConfig *cfg);

Server *serverNew(const ServerConfig *cfg);
void serverFree(Server *s);

/* Called by the event loop every ~100 ms.
 * Handles: TTL purge, periodic snapshot.
 * net.c need not include Persist.h or Store.h for maintenance purposes. */
void serverTick(Server *s);

#endif /* SERVER_H */