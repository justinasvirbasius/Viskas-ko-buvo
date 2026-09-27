from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path
from typing import Any

from .config import Config


def _script_path(config: Config) -> Path | None:
    if not config.ticker_module_path:
        return None
    base = config.ticker_module_path
    if base.is_dir():
        return (base / "Start-ReporitorTicker.ps1").resolve()
    if base.name.lower() == "reporitorticker.psd1":
        return (base.parent / "Start-ReporitorTicker.ps1").resolve()
    return base.resolve()


def _find_executable(config: Config) -> str | None:
    candidates = [config.ticker_executable] if config.ticker_executable else []
    candidates.extend(["pwsh", "powershell"])
    for candidate in candidates:
        if not candidate:
            continue
        found = shutil.which(candidate)
        if found:
            return found
    return None


def ticker_forecast(config: Config) -> dict[str, Any]:
    result: dict[str, Any] = {
        "enabled": config.ticker_enabled,
        "routine": "reporitor_ticker",
        "repository": config.ticker_repository,
        "continuous": config.ticker_continuous,
    }
    if not config.ticker_enabled:
        result["status"] = "disabled"
        return result
    script = _script_path(config)
    executable = _find_executable(config)
    if script is None:
        result.update(status="unavailable", reason="reporitor_ticker.module_path is not configured")
        return result
    if not script.exists():
        result.update(status="unavailable", reason=f"ticker start script does not exist: {script}")
        return result
    if not executable:
        result.update(status="unavailable", reason="PowerShell executable was not found")
        return result
    if not config.ticker_root:
        result.update(status="unavailable", reason="reporitor_ticker.root is not configured")
        return result
    result.update(status="ready", executable=executable, script=str(script), root=str(config.ticker_root))
    return result


def run_ticker(config: Config) -> dict[str, Any]:
    forecast = ticker_forecast(config)
    if forecast["status"] != "ready":
        raise RuntimeError(f"reporitor ticker not ready: {forecast.get('reason', forecast['status'])}")
    assert config.ticker_root is not None
    command = [
        forecast["executable"],
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        forecast["script"],
        "-Root",
        str(config.ticker_root),
        "-IntervalSeconds",
        str(config.ticker_interval_seconds),
        "-MaxTicks",
        str(config.ticker_max_ticks),
    ]
    if config.ticker_repository:
        command.extend(["-Repository", config.ticker_repository])
    if config.ticker_continuous:
        command.append("-Continuous")
    proc = subprocess.run(
        command,
        text=True,
        capture_output=True,
        timeout=config.ticker_timeout_seconds,
        check=False,
    )
    return {
        "routine": "reporitor_ticker",
        "exit_code": proc.returncode,
        "stdout": proc.stdout[-8192:],
        "stderr": proc.stderr[-8192:],
        "root": str(config.ticker_root),
        "repository": config.ticker_repository,
        "status": "ok" if proc.returncode == 0 else "error",
    }
