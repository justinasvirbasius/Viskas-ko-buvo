from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, Tuple
import json
import time

from .config import profile_config
from .manager import build_default_rechts_demo
from .qa import package_value_score, strict_validation_gate


def run_smoke_press(
    out_dir: str | Path,
    resolution: Tuple[int, int, int] = (28, 28, 28),
    profile: str = "debug",
) -> Dict[str, Any]:
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)

    cfg = profile_config(profile).with_resolution(resolution)
    start = time.perf_counter()

    im = build_default_rechts_demo(cfg)
    meta = im.build_meta()
    mesh = im.build_mesh()
    validation = mesh.validation_report()
    ok, reasons = strict_validation_gate(validation, cfg.min_solid_rechts_score)
    validation["strict_gate_passed"] = bool(ok)
    validation["strict_gate_reasons"] = reasons

    meta_stats = meta.stats()
    value = package_value_score(meta_stats, validation)

    manifest = im.export_all(out)
    elapsed = time.perf_counter() - start

    report = {
        "format": "sdfpack-rechts-v3-smoke-press",
        "profile": profile,
        "resolution": list(resolution),
        "elapsed_seconds": elapsed,
        "meta_stats": meta_stats,
        "mesh_validation": validation,
        "package_value_score": value,
        "pressure_verdict": "PASS" if ok and value >= 0.85 else "FAIL",
        "minimums": {
            "solid_rechts_score": cfg.min_solid_rechts_score,
            "package_value_score": 0.85,
        },
        "outputs": manifest["outputs"],
    }

    path = out / "smoke_press_value.json"
    path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    report["outputs"]["smoke_press_value"] = str(path)

    if report["pressure_verdict"] != "PASS":
        raise RuntimeError("Smoke press failed: " + "; ".join(reasons) + f"; package_value_score={value}")

    return report
