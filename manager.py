from __future__ import annotations

import json
import threading
import time
from datetime import datetime, timezone
from typing import Any

from .config import Config
from .connectors import native_forecast, run_native
from .routines import BUILTINS
from .ticker import run_ticker, ticker_forecast


class ScriptManager:
    def __init__(self, config: Config):
        self.config = config
        self._locks: dict[str, threading.Lock] = {}

    def list_routines(self) -> list[dict[str, str]]:
        rows = [{"name": name, "kind": "builtin"} for name in BUILTINS]
        if self.config.native_enabled:
            rows.append({"name": "native", "kind": "isolated-native"})
        if self.config.ticker_enabled:
            rows.append({"name": "reporitor_ticker", "kind": "powershell-connector"})
        return rows

    def run(self, routine: str, apply: bool = False) -> dict[str, Any]:
        lock = self._locks.setdefault(routine, threading.Lock())
        if not lock.acquire(blocking=False):
            raise RuntimeError(f"routine already running: {routine}")
        try:
            if routine == "native":
                result = run_native(self.config, {"timestamp": datetime.now(timezone.utc).isoformat()})
            elif routine == "reporitor_ticker":
                result = run_ticker(self.config)
            elif routine in BUILTINS:
                if self.config.mode == "observe" and apply:
                    raise PermissionError("observe mode does not permit apply actions")
                result = BUILTINS[routine](self.config, apply=apply)
            else:
                raise KeyError(f"unknown routine: {routine}")
            response = {"timestamp": datetime.now(timezone.utc).isoformat(), "routine": routine,
                        "status": "ok", "result": result}
            return self._bound_output(response)
        finally:
            lock.release()

    def _bound_output(self, response: dict[str, Any]) -> dict[str, Any]:
        """Keep verbose path lists within the configured JSON output budget."""
        result = response.get("result", {})
        list_keys = ("candidates", "old_logs", "removed")
        for key in list_keys:
            rows = result.get(key)
            if not isinstance(rows, list):
                continue
            original_size = len(rows)
            while rows and len(json.dumps(response, separators=(",", ":")).encode("utf-8")) > self.config.max_output_bytes:
                rows.pop()
            if len(rows) < original_size:
                result[f"{key}_omitted"] = True
        encoded_size = len(json.dumps(response, separators=(",", ":")).encode("utf-8"))
        if encoded_size > self.config.max_output_bytes:
            # Scalar and routine metadata alone exceeded the limit: return a small, explicit summary.
            response["result"] = {"summary": "output exceeded configured max_output_bytes",
                                  "original_routine": result.get("routine", response.get("routine")),
                                  "configured_limit_bytes": self.config.max_output_bytes}
        return response

    def forecast(self) -> dict[str, Any]:
        routine_names = set(BUILTINS) | ({"native"} if self.config.native_enabled else set()) | ({"reporitor_ticker"} if self.config.ticker_enabled else set())
        unknown = []
        pairs = []
        jobs = self.config.jobs
        for job in jobs:
            if job.routine not in routine_names:
                unknown.append({"job": job.name, "routine": job.routine})
        for i, left in enumerate(jobs):
            for right in jobs[i + 1:]:
                overlap = sorted(set(left.writes) & (set(right.reads) | set(right.writes)) |
                                 set(right.writes) & set(left.reads))
                if overlap:
                    pairs.append({"jobs": [left.name, right.name], "severity": "conflict",
                                  "shared_resources": overlap,
                                  "recommendation": "these interval jobs start together; keep the manager's serialized execution or assign distinct resource ownership"})
        disk = BUILTINS["disk_report"](self.config)
        low_space = [r["path"] for r in disk["roots"] if r.get("below_min_free")]
        native = native_forecast(self.config)
        ticker = ticker_forecast(self.config)
        advisories = []
        missing_roots = [r["path"] for r in disk["roots"] if not r.get("exists")]
        if missing_roots:
            advisories.append({"severity": "warning", "kind": "missing-root", "paths": missing_roots})
        if low_space:
            advisories.append({"severity": "warning", "kind": "low-disk-headroom", "paths": low_space})
        if native.get("status") not in {"ready", "disabled"}:
            advisories.append({"severity": "warning", "kind": "native-connector", "detail": native.get("reason", native["status"])})
        if ticker.get("status") not in {"ready", "disabled"}:
            advisories.append({"severity": "warning", "kind": "reporitor-ticker", "detail": ticker.get("reason", ticker["status"])})
        return {"generated_at": datetime.now(timezone.utc).isoformat(), "mode": self.config.mode,
                "schedule_jobs": len(jobs), "unknown_routines": unknown, "resource_collisions": pairs,
                "disk": disk, "native_connector": native, "reporitor_ticker": ticker, "advisories": advisories,
                "decision": "review-required" if unknown or pairs or advisories else "clear"}

    def serve(self, stop: threading.Event | None = None) -> None:
        """Run configured interval jobs until stop is set or Ctrl+C arrives."""
        stop = stop or threading.Event()
        due = {job.name: time.monotonic() for job in self.config.jobs}
        jobs = {job.name: job for job in self.config.jobs}
        while not stop.is_set():
            now = time.monotonic()
            for name, job in jobs.items():
                if now >= due[name]:
                    try:
                        print(json.dumps(self.run(job.routine)), flush=True)
                    except Exception as e:
                        print(json.dumps({"job": name, "status": "error", "error": str(e)}), flush=True)
                    due[name] = now + job.every_seconds
            stop.wait(self.config.poll_seconds)
