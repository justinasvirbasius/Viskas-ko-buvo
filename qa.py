from __future__ import annotations

from typing import Any, Dict


def strict_validation_gate(report: Dict[str, Any], min_score: float = 0.999) -> tuple[bool, list[str]]:
    reasons: list[str] = []

    if not report.get("closed", False):
        reasons.append("mesh is not closed")
    if not report.get("manifold", False):
        reasons.append("mesh is not manifold")
    if not report.get("oriented_positive_volume", False):
        reasons.append("mesh is not positively oriented")
    if int(report.get("boundary_edge_count", 1)) != 0:
        reasons.append(f"boundary_edge_count={report.get('boundary_edge_count')}")
    if int(report.get("nonmanifold_edge_count", 1)) != 0:
        reasons.append(f"nonmanifold_edge_count={report.get('nonmanifold_edge_count')}")
    if float(report.get("solid_rechts_score", 0.0)) < min_score:
        reasons.append(f"solid_rechts_score={report.get('solid_rechts_score')} below {min_score}")

    return len(reasons) == 0, reasons


def package_value_score(meta_stats: Dict[str, Any], mesh_report: Dict[str, Any]) -> float:
    solid = float(mesh_report.get("solid_rechts_score", 0.0))
    entropy_max = float(meta_stats.get("entropy_max", 0.0))
    value_max = float(meta_stats.get("value_pressure_max", 0.0))
    surface_voxels = float(meta_stats.get("surface_voxels", 0.0))
    inside_voxels = max(float(meta_stats.get("inside_voxels", 1.0)), 1.0)
    surface_ratio = min(surface_voxels / inside_voxels, 1.0)

    score = 0.55 * solid + 0.15 * entropy_max + 0.20 * value_max + 0.10 * surface_ratio
    return max(0.0, min(1.0, score))
