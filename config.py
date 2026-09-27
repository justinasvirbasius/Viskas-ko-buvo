from __future__ import annotations

import os
import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class Job:
    name: str
    routine: str
    every_seconds: int
    reads: tuple[str, ...] = ()
    writes: tuple[str, ...] = ()


@dataclass(frozen=True)
class Config:
    path: Path
    mode: str = "propose"
    poll_seconds: int = 5
    allowed_roots: tuple[Path, ...] = ()
    cleanup_roots: tuple[Path, ...] = ()
    cleanup_older_than_days: int = 30
    max_files: int = 50000
    max_output_bytes: int = 65536
    min_free_bytes: int = 1073741824
    allow_destructive: bool = False
    native_enabled: bool = False
    native_dir: Path | None = None
    native_library: Path | None = None
    native_routine: str = "probe"
    native_timeout_seconds: int = 10
    ticker_enabled: bool = False
    ticker_executable: str = ""
    ticker_module_path: Path | None = None
    ticker_root: Path | None = None
    ticker_repository: str = ""
    ticker_interval_seconds: int = 5
    ticker_max_ticks: int = 12
    ticker_timeout_seconds: int = 60
    ticker_continuous: bool = False
    jobs: tuple[Job, ...] = field(default_factory=tuple)


def _resolved(base: Path, raw: str) -> Path:
    path = Path(os.path.expandvars(os.path.expanduser(raw)))
    if not path.is_absolute():
        path = base / path
    return path.resolve()


def load_config(path: str | Path) -> Config:
    config_path = Path(path).expanduser().resolve()
    with config_path.open("rb") as f:
        data: dict[str, Any] = tomllib.load(f)
    base = config_path.parent
    manager = data.get("manager", {})
    policy = data.get("policy", {})
    native = data.get("native", {})
    ticker = data.get("reporitor_ticker", {})
    mode = manager.get("mode", "propose")
    if mode not in {"observe", "propose", "apply"}:
        raise ValueError("manager.mode must be observe, propose, or apply")
    roots = tuple(_resolved(base, p) for p in policy.get("allowed_roots", ["./data"]))
    cleanup_roots = tuple(_resolved(base, p) for p in policy.get("cleanup_roots", []))
    for root in cleanup_roots:
        if not any(root == allowed or root.is_relative_to(allowed) for allowed in roots):
            raise ValueError(f"cleanup root is outside allowed_roots: {root}")
    jobs = []
    seen = set()
    for item in data.get("jobs", []):
        name = str(item["name"])
        if name in seen:
            raise ValueError(f"duplicate job name: {name}")
        seen.add(name)
        interval = int(item.get("every_seconds", 0))
        if interval < 1:
            raise ValueError(f"job {name} every_seconds must be >= 1")
        jobs.append(Job(name, str(item["routine"]), interval,
                        tuple(item.get("reads", [])), tuple(item.get("writes", []))))
    native_dir = _resolved(base, native.get("directory", "./connectors"))
    library = native.get("library")
    native_library = _resolved(base, library) if library else None
    ticker_module = ticker.get("module_path")
    ticker_root = ticker.get("root")
    return Config(
        path=config_path, mode=mode,
        poll_seconds=max(1, int(manager.get("poll_seconds", 5))),
        allowed_roots=roots, cleanup_roots=cleanup_roots,
        cleanup_older_than_days=max(1, int(policy.get("cleanup_older_than_days", 30))),
        max_files=max(1, int(policy.get("max_files", 50000))),
        max_output_bytes=max(1024, int(policy.get("max_output_bytes", 65536))),
        min_free_bytes=max(0, int(policy.get("min_free_bytes", 1073741824))),
        allow_destructive=bool(policy.get("allow_destructive", False)),
        native_enabled=bool(native.get("enabled", False)), native_dir=native_dir,
        native_library=native_library, native_routine=str(native.get("routine", "probe")),
        native_timeout_seconds=max(1, int(native.get("timeout_seconds", 10))),
        ticker_enabled=bool(ticker.get("enabled", False)),
        ticker_executable=str(ticker.get("executable", "")),
        ticker_module_path=_resolved(base, ticker_module) if ticker_module else None,
        ticker_root=_resolved(base, ticker_root) if ticker_root else None,
        ticker_repository=str(ticker.get("repository", "")),
        ticker_interval_seconds=max(1, int(ticker.get("interval_seconds", 5))),
        ticker_max_ticks=max(1, int(ticker.get("max_ticks", 12))),
        ticker_timeout_seconds=max(1, int(ticker.get("timeout_seconds", 60))),
        ticker_continuous=bool(ticker.get("continuous", False)),
        jobs=tuple(jobs),
    )
