/* src/Persist.h – snapshot-based persistence
 *
 * WHY SNAPSHOTS OVER WAL:
 *
 *   A Write-Ahead Log is correct under crash, but forces you to implement:
 *     1. Durable append-only writes (O_DSYNC or fdatasync per write)
 *     2. Log truncation / compaction
 *     3. Recovery: replay from last checkpoint + tail of log
 *
 *   Snapshot (fork-and-dump style in production; atomic rename here):
 *     1. Serialize entire in-memory state to a temp file
 *     2. fdatasync the temp file
 *     3. rename(tmp, final) — atomic on POSIX filesystems
 *     4. On startup, load the file if it exists; missing = empty store
 *
 *   Worst case on crash: lose writes since last snapshot (configurable
 *   interval).  Acceptable for a learning project.
 *
 * BINARY FORMAT  (little-endian, no external deps):
 *
 *   Header:   "KVDB" (4 bytes) | version u32 | entryCount u64
 *   Per entry: type u8 | keyLen u32 | key | expireEpochMs i64 | payload...
 *
 *   String payload:  valLen u32 | valBytes
 *   expireEpochMs uses wall-clock epoch milliseconds on disk.
 *   In memory, Object.expireMs remains monotonic milliseconds.
 *
 *   List payload:    elemCount u32 | (elemLen u32 | elemBytes)*
 *   Hash payload:    fieldCount u32 | (kLen u32 | k | vLen u32 | v)*
 */
#ifndef PERSIST_H
#define PERSIST_H

#include "Server.h"

#define SNAPSHOT_MAGIC "KVDB"
#define SNAPSHOT_VERSION 1u

/* Save the entire store to path (atomic via tmp+rename).
 * Returns 0 on success, -1 on error (errno set). */
int persistSave(const Server *srv);

/* Load a snapshot into srv->store (replaces current contents).
 * Returns 0 on success, 1 if file doesn't exist (not an error),
 * -1 on parse error. */
int persistLoad(Server *srv);

#endif /* PERSIST_H */