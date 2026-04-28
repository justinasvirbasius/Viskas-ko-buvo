from __future__ import annotations

import numpy as np


def smooth_min(a: np.ndarray, b: np.ndarray, k: float) -> np.ndarray:
    if k <= 0.0:
        return np.minimum(a, b)
    h = np.clip(0.5 + 0.5 * (b - a) / k, 0.0, 1.0)
    return (b * (1.0 - h) + a * h) - k * h * (1.0 - h)


def compose_sdf(base_sdf: np.ndarray, base_material: np.ndarray, new_sdf: np.ndarray, material_id: int, op: str, smooth_k: float) -> tuple[np.ndarray, np.ndarray]:
    op = op.lower().strip()

    if op == "union":
        mask = new_sdf < base_sdf
        return np.minimum(base_sdf, new_sdf).astype(np.float32), np.where(mask, material_id, base_material).astype(np.int16)

    if op == "smooth_union":
        out = smooth_min(base_sdf, new_sdf, smooth_k)
        mask = new_sdf < (base_sdf + max(smooth_k, 0.0))
        return out.astype(np.float32), np.where(mask, material_id, base_material).astype(np.int16)

    if op in ("subtract", "difference", "cut"):
        out = np.maximum(base_sdf, -new_sdf)
        return out.astype(np.float32), base_material.astype(np.int16)

    if op in ("intersect", "intersection"):
        out = np.maximum(base_sdf, new_sdf)
        mask = new_sdf > base_sdf
        return out.astype(np.float32), np.where(mask, material_id, base_material).astype(np.int16)

    raise ValueError(f"Unsupported SDF operation {op!r}.")
