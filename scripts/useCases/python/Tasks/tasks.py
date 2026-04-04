#!/usr/bin/env python3
"""
tasks.py — terminal task manager backed by miniDB (or any Redis-compatible server)

Install:  pip install redis
Run:      python tasks.py --help

Data model in miniDB
--------------------
  meta:seq              INCR  auto-increment task ID counter
  meta:total_added      INCR  lifetime tasks created
  meta:total_done       INCR  lifetime tasks completed
  task:<id>             HASH  {id, title, priority, tag, status, created, [completed]}
  idx:pending:p<1-3>    LIST  task IDs by priority (p3=high, p2=normal, p1=low)
  idx:done              LIST  completed task IDs, most recent last

Usage examples
--------------
  python3 tasks.py add "Write report" --priority high --tag work
  python3 tasks.py add "Buy milk" --tag shopping --due 7200
  python3 tasks.py list
  python3 tasks.py list --tag work
  python3 tasks.py done 2
  python3 tasks.py info 1
  python3 tasks.py rm 3
  python3 tasks.py stats
"""

import argparse
import sys
from datetime import datetime

import redis

# ── connection ────────────────────────────────────────────────────────────────

R = redis.Redis(host="localhost", port=6380, decode_responses=True)

# ── constants ─────────────────────────────────────────────────────────────────

PRIO_VAL  = {"low": 1, "normal": 2, "high": 3}
PRIO_NAME = {v: k for k, v in PRIO_VAL.items()}
PRIO_MARK = {1: " ", 2: "!", 3: "★"}   # visual weight in list output

# ── helpers ───────────────────────────────────────────────────────────────────

def now() -> str:
    return datetime.now().strftime("%Y-%m-%d %H:%M")

def task_key(tid: int) -> str:
    return f"task:{tid}"

def prio_list(p: int) -> str:
    return f"idx:pending:p{p}"

def remove_from_list(key: str, member: str) -> None:
    items = [item for item in R.lrange(key, 0, -1) if item != member]
    R.delete(key)
    for item in items:
        R.rpush(key, item)

def die(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)

# ── commands ──────────────────────────────────────────────────────────────────

def do_add(args: argparse.Namespace) -> None:
    tid = R.incr("meta:seq")
    p   = PRIO_VAL.get(args.priority, 2)
    R.hset(task_key(tid), mapping={
        "id":       tid,
        "title":    args.title,
        "priority": p,
        "tag":      args.tag or "",
        "status":   "pending",
        "created":  now(),
    })
    R.rpush(prio_list(p), tid)
    R.incr("meta:total_added")

    if args.due:
        R.expire(task_key(tid), args.due)

    print(f"  added  #{tid}  {args.title}", end="")
    print(f"  (expires in {args.due}s)" if args.due else "")


def do_list(args: argparse.Namespace) -> None:
    rows = []
    for p in (3, 2, 1):                     # highest priority first
        for tid in R.lrange(prio_list(p), 0, -1):
            k = task_key(int(tid))
            if not R.exists(k):
                remove_from_list(prio_list(p), tid) # lazily remove expired keys
                continue
            t = R.hgetall(k)
            if t.get("status") != "pending":
                continue
            if args.tag and t.get("tag") != args.tag:
                continue
            t["_ttl"] = R.ttl(k)
            rows.append(t)

    if not rows:
        print("  no pending tasks.")
        return

    W = 36
    print()
    print(f"  {'#':<5} {'P':<3} {'Tag':<12} {'Title':<{W}} {'Created':<16}  TTL")
    print("  " + "─" * (5 + 3 + 12 + W + 16 + 8))
    for t in rows:
        p       = int(t.get("priority", 2))
        tid     = t["id"]
        tag     = (t.get("tag") or "")[:11]
        title   = (t.get("title") or "")[:W - 1]
        created = t.get("created", "")
        ttl     = t["_ttl"]
        ttl_str = f"{ttl}s" if ttl >= 0 else "—"
        print(f"  #{tid:<4} {PRIO_MARK[p]:<3} {tag:<12} {title:<{W}} {created:<16}  {ttl_str}")
    print()


def do_done(args: argparse.Namespace) -> None:
    k = task_key(args.id)
    t = R.hgetall(k)
    if not t:
        die(f"task #{args.id} not found")

    p = int(t.get("priority", 2))
    R.hset(k, mapping={"status": "done", "completed": now()})
    R.persist(k)                               # remove TTL if any
    remove_from_list(prio_list(p), str(args.id))
    R.rpush("idx:done", args.id)
    R.incr("meta:total_done")
    print(f"  done   #{args.id}  {t['title']}")


def do_rm(args: argparse.Namespace) -> None:
    k = task_key(args.id)
    t = R.hgetall(k)
    if not t:
        die(f"task #{args.id} not found")

    p = int(t.get("priority", 2))
    R.delete(k)
    remove_from_list(prio_list(p), str(args.id))
    remove_from_list("idx:done", str(args.id))
    print(f"  removed #{args.id}  {t['title']}")


def do_info(args: argparse.Namespace) -> None:
    k = task_key(args.id)
    t = R.hgetall(k)
    if not t:
        die(f"task #{args.id} not found")

    ttl = R.ttl(k)
    print(f"\n  Task #{args.id}")
    print(f"  title     {t.get('title')}")
    print(f"  status    {t.get('status')}")
    print(f"  priority  {PRIO_NAME.get(int(t.get('priority', 2)))}")
    print(f"  tag       {t.get('tag') or '—'}")
    print(f"  created   {t.get('created')}")
    if t.get("completed"):
        print(f"  completed {t.get('completed')}")
    if ttl >= 0:
        print(f"  expires   in {ttl}s")
    print()


def do_stats(_args: argparse.Namespace) -> None:
    added   = int(R.get("meta:total_added") or 0)
    done    = int(R.get("meta:total_done")  or 0)
    pending = added - done
    print()
    print(f"  total added   {added}")
    print(f"  completed     {done}")
    print(f"  pending       {pending}")
    for p in (3, 2, 1):
        n = R.llen(prio_list(p))
        print(f"    {PRIO_NAME[p]:<8}  {n}")
    print()

# ── CLI ───────────────────────────────────────────────────────────────────────

def main() -> None:
    ap = argparse.ArgumentParser(
        prog="tasks",
        description="miniDB task manager",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_add = sub.add_parser("add", help="add a new task")
    p_add.add_argument("title")
    p_add.add_argument("-p", "--priority",
                       choices=["low", "normal", "high"], default="normal")
    p_add.add_argument("-t", "--tag", default="", metavar="TAG")
    p_add.add_argument("--due", type=int, metavar="SECS",
                       help="auto-expire task in N seconds")

    p_list = sub.add_parser("list", help="list pending tasks")
    p_list.add_argument("-t", "--tag", default="", help="filter by tag")

    p_done = sub.add_parser("done", help="mark a task as done")
    p_done.add_argument("id", type=int)

    p_rm = sub.add_parser("rm", help="delete a task")
    p_rm.add_argument("id", type=int)

    p_info = sub.add_parser("info", help="show full task details")
    p_info.add_argument("id", type=int)

    sub.add_parser("stats", help="summary statistics")

    args = ap.parse_args()

    try:
        R.ping()
    except redis.exceptions.ConnectionError:
        die("cannot connect to miniDB on localhost:6380\n"
            "       start:  ./miniDB --port 6380")

    dispatch = {
        "add":   do_add,
        "list":  do_list,
        "done":  do_done,
        "rm":    do_rm,
        "info":  do_info,
        "stats": do_stats,
    }
    dispatch[args.cmd](args)


if __name__ == "__main__":
    main()