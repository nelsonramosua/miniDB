# C Integration Examples

This folder contains native C applications integrating with miniDB via raw TCP sockets and hand-rolled RESP clients.

## Metrics

Located in `Metrics/`, this is a system metrics collector and viewer for Linux. It reads CPU (`/proc/stat`) and Memory (`/proc/meminfo`) usage and pushes them to miniDB as HASH and LIST structures.

### Quick start

```bash
cd Metrics
gcc -std=c11 -Wall -Wextra -O2 -o metrics Metrics.c
./metrics --daemon
```

In another terminal:

```bash
./metrics --watch
```

### Data model in miniDB

* `metrics:cpu:current`: HASH
* `metrics:mem:current`: HASH
* `metrics:cpu:history`: LIST
* `metrics:mem:history`: LIST
* `metrics:samples`: INCR counter
