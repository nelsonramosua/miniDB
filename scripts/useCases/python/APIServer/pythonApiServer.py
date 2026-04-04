#!/usr/bin/env python3
"""FastAPI service backed by miniDB.

Run:
  uvicorn scripts.tools.pythonApiServer:app --host 0.0.0.0 --port 8080 --reload

Environment variables:
  REDIS_HOST (default: 127.0.0.1)
  REDIS_PORT (default: 6380)
"""

from __future__ import annotations

import os
import secrets
import time
from typing import Dict, List, Optional

import redis
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, Field


SESSION_TTL_SECONDS = 3600
RATE_LIMIT_WINDOW_SECONDS = 60
RATE_LIMIT_MAX_ATTEMPTS = 10

REDIS_HOST = os.getenv("REDIS_HOST", "127.0.0.1")
REDIS_PORT = int(os.getenv("REDIS_PORT", "6380"))

app = FastAPI(title="miniDB Real API", version="1.0.0")
r = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, decode_responses=True)


class UserCreate(BaseModel):
    user_id: str = Field(min_length=1, max_length=64)
    name: str = Field(min_length=1, max_length=120)
    role: str = Field(min_length=1, max_length=64)


class LoginRequest(BaseModel):
    user_id: str = Field(min_length=1, max_length=64)
    client_ip: str = Field(min_length=3, max_length=64)


class EnqueueEmailRequest(BaseModel):
    user_id: str = Field(min_length=1, max_length=64)
    template: str = Field(min_length=1, max_length=80)


class UserOut(BaseModel):
    id: str
    name: str
    role: str


def user_key(user_id: str) -> str:
    return f"user:{user_id}"


def session_key(token: str) -> str:
    return f"session:{token}"


def rate_limit_key(client_ip: str) -> str:
    return f"ratelimit:login:{client_ip}"


def _redis_unavailable(exc: Exception) -> HTTPException:
    return HTTPException(status_code=503, detail=f"miniDB unavailable: {exc}")


def _read_user(user_id: str) -> Optional[Dict[str, str]]:
    try:
        data = r.hgetall(user_key(user_id))
    except redis.RedisError as exc:
        raise _redis_unavailable(exc)
    return data or None


def _allow_login_attempt(client_ip: str) -> bool:
    key = rate_limit_key(client_ip)
    try:
        attempts = r.incr(key)
        if attempts == 1:
            r.expire(key, RATE_LIMIT_WINDOW_SECONDS)
    except redis.RedisError as exc:
        raise _redis_unavailable(exc)
    return attempts <= RATE_LIMIT_MAX_ATTEMPTS


@app.get("/health")
def health() -> Dict[str, str]:
    try:
        r.ping()
    except redis.RedisError as exc:
        raise HTTPException(status_code=503, detail=f"miniDB unavailable: {exc}")
    return {"status": "ok"}


@app.post("/users")
def create_user(payload: UserCreate) -> Dict[str, str]:
    try:
        r.hset(user_key(payload.user_id), mapping={"id": payload.user_id, "name": payload.name, "role": payload.role})
    except redis.RedisError as exc:
        raise _redis_unavailable(exc)
    return {"status": "created", "key": user_key(payload.user_id)}


@app.get("/users/{user_id}", response_model=UserOut)
def get_user(user_id: str) -> UserOut:
    user = _read_user(user_id)
    if not user:
        raise HTTPException(status_code=404, detail="user not found")
    return UserOut(id=user["id"], name=user["name"], role=user["role"])


@app.post("/login")
def login(payload: LoginRequest) -> Dict[str, object]:
    if not _allow_login_attempt(payload.client_ip):
        raise HTTPException(status_code=429, detail="too many login attempts")

    user = _read_user(payload.user_id)
    if not user:
        raise HTTPException(status_code=404, detail="user not found")

    token = secrets.token_hex(16)
    try:
        r.set(session_key(token), payload.user_id, ex=SESSION_TTL_SECONDS)
    except redis.RedisError as exc:
        raise _redis_unavailable(exc)
    return {"session_token": token, "ttl_seconds": SESSION_TTL_SECONDS}


@app.get("/whoami/{token}")
def whoami(token: str) -> Dict[str, object]:
    try:
        user_id = r.get(session_key(token))
    except redis.RedisError as exc:
        raise _redis_unavailable(exc)
    if not user_id:
        raise HTTPException(status_code=401, detail="session expired or invalid")

    user = _read_user(user_id)
    if not user:
        raise HTTPException(status_code=404, detail="session valid but user missing")

    return {"user": {"id": user["id"], "name": user["name"], "role": user["role"]}}


@app.post("/jobs/email")
def enqueue_email_job(payload: EnqueueEmailRequest) -> Dict[str, object]:
    if not _read_user(payload.user_id):
        raise HTTPException(status_code=404, detail="user not found")

    job = f"send_email|user={payload.user_id}|template={payload.template}|ts={int(time.time())}"
    try:
        size = r.rpush("queue:email", job)
    except redis.RedisError as exc:
        raise _redis_unavailable(exc)
    return {"status": "queued", "pending": size, "job": job}


@app.get("/jobs/email")
def queue_status() -> Dict[str, object]:
    try:
        size = r.llen("queue:email")
        preview = r.lrange("queue:email", 0, 9)
    except redis.RedisError as exc:
        raise _redis_unavailable(exc)
    return {"pending": size, "preview": preview}


@app.post("/jobs/email/worker-once")
def worker_once() -> Dict[str, object]:
    try:
        job = r.lpop("queue:email")
    except redis.RedisError as exc:
        raise _redis_unavailable(exc)
    if not job:
        return {"status": "empty"}
    return {"status": "processed", "job": job}


@app.get("/metrics")
def metrics() -> Dict[str, object]:
    try:
        info = r.info()
    except redis.RedisError as exc:
        raise _redis_unavailable(exc)
    return {
        "version": info.get("version"),
        "uptime_in_seconds": info.get("uptime_in_seconds"),
        "keys": info.get("keys"),
        "load_factor": info.get("load_factor"),
        "total_commands_processed": info.get("total_commands_processed"),
    }


@app.get("/users")
def list_users() -> Dict[str, List[str]]:
    cursor = 0
    keys: List[str] = []
    try:
        while True:
            cursor, batch = r.scan(cursor=cursor, match="user:*", count=100)
            keys.extend(batch)
            if cursor == 0:
                break
    except redis.RedisError as exc:
        raise _redis_unavailable(exc)
    return {"users": keys}
