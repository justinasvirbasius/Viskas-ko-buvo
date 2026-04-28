from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Tuple
import numpy as np

from .chunking import build_chunk_id, chunk_scores, chunk_bounds, seam_mask
from .convolution import gaussian_blur3d, downsample2

Vec3 = Tuple[float, float, float]
Vec3i = Tuple[int, int, int]


def normalize01(a: np.ndarray, eps: float = 1e-8) -> np.ndarray:
    lo = float(np.nanmin(a))
    hi = float(np.nanmax(a))
    return ((a - lo) / (hi - lo + eps)).astype(np.float32)


def percentile_pressure(a: np.ndarray, pct: float = 95.0, eps: float = 1e-6) -> np.ndarray:
    s = float(np.percentile(np.abs(a), pct))
    return np.clip(np.abs(a) / (s + eps), 0.0, 1.0).astype(np.float32)


@dataclass
class MetaLayers:
    sdf_raw: np.ndarray
    sdf: np.ndarray
    occupancy: np.ndarray
    material: np.ndarray
    gradient: np.ndarray
    normal: np.ndarray
    curvature: np.ndarray
    surface: np.ndarray
    entropy: np.ndarray
    lod: np.ndarray
    rechts: np.ndarray
    value_pressure: np.ndarray
    moat: np.ndarray
    band_index: np.ndarray
    chunk_id: np.ndarray
    chunk_score: np.ndarray
    chunk_bounds: np.ndarray
    seam: np.ndarray
    mip_sdf_1: np.ndarray
    mip_entropy_1: np.ndarray
    mip_sdf_2: np.ndarray
    mip_entropy_2: np.ndarray
    bounds_min: Vec3
    bounds_max: Vec3
    spacing: Vec3
    chunk_size: Vec3i

    @property
    def resolution(self) -> Vec3i:
        return tuple(int(v) for v in self.sdf.shape)

    def save_npz(self, path: str | Path) -> None:
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        np.savez_compressed(
            path,
            sdf_raw=self.sdf_raw.astype(np.float32),
            sdf=self.sdf.astype(np.float32),
            occupancy=self.occupancy.astype(np.uint8),
            material=self.material.astype(np.int16),
            gradient=self.gradient.astype(np.float32),
            normal=self.normal.astype(np.float32),
            curvature=self.curvature.astype(np.float32),
            surface=self.surface.astype(np.float32),
            entropy=self.entropy.astype(np.float32),
            lod=self.lod.astype(np.uint8),
            rechts=self.rechts.astype(np.float32),
            value_pressure=self.value_pressure.astype(np.float32),
            moat=self.moat.astype(np.float32),
            band_index=self.band_index.astype(np.uint8),
            chunk_id=self.chunk_id.astype(np.int32),
            chunk_score=self.chunk_score.astype(np.float32),
            chunk_bounds=self.chunk_bounds.astype(np.float32),
            seam=self.seam.astype(np.uint8),
            mip_sdf_1=self.mip_sdf_1.astype(np.float32),
            mip_entropy_1=self.mip_entropy_1.astype(np.float32),
            mip_sdf_2=self.mip_sdf_2.astype(np.float32),
            mip_entropy_2=self.mip_entropy_2.astype(np.float32),
            bounds_min=np.asarray(self.bounds_min, dtype=np.float32),
            bounds_max=np.asarray(self.bounds_max, dtype=np.float32),
            spacing=np.asarray(self.spacing, dtype=np.float32),
            chunk_size=np.asarray(self.chunk_size, dtype=np.int32),
        )

    def stats(self) -> dict:
        return {
            "resolution": list(self.resolution),
            "bounds_min": list(self.bounds_min),
            "bounds_max": list(self.bounds_max),
            "spacing": list(self.spacing),
            "chunk_size": list(self.chunk_size),
            "inside_voxels": int(np.count_nonzero(self.occupancy)),
            "surface_voxels": int(np.count_nonzero(self.surface > 0.50)),
            "material_ids": sorted(int(x) for x in np.unique(self.material)),
            "sdf_min": float(np.min(self.sdf)),
            "sdf_max": float(np.max(self.sdf)),
            "entropy_mean": float(np.mean(self.entropy)),
            "entropy_max": float(np.max(self.entropy)),
            "rechts_mean": float(np.mean(self.rechts)),
            "rechts_min": float(np.min(self.rechts)),
            "rechts_max": float(np.max(self.rechts)),
            "value_pressure_mean": float(np.mean(self.value_pressure)),
            "value_pressure_max": float(np.max(self.value_pressure)),
            "chunk_grid": list(self.chunk_score.shape),
            "chunk_score_max": float(np.max(self.chunk_score)),
            "mip_sdf_1_shape": list(self.mip_sdf_1.shape),
            "mip_sdf_2_shape": list(self.mip_sdf_2.shape),
        }


def derive_meta_layers(
    sdf_raw: np.ndarray,
    sdf: np.ndarray,
    material: np.ndarray,
    bounds_min: Vec3,
    bounds_max: Vec3,
    spacing: Vec3,
    chunk_size: Vec3i,
    entropy_sigma: float,
    surface_band_voxels: float,
    lod_thresholds: tuple[float, float, float],
) -> MetaLayers:
    sx, sy, sz = spacing
    band = max(spacing) * float(surface_band_voxels)

    occupancy = (sdf <= 0.0).astype(np.uint8)
    gx, gy, gz = np.gradient(sdf.astype(np.float32), sx, sy, sz, edge_order=1)
    gradient = np.stack([gx, gy, gz], axis=-1).astype(np.float32)
    grad_mag = np.sqrt(gx * gx + gy * gy + gz * gz).astype(np.float32)
    normal = gradient / (grad_mag[..., None] + 1e-8)

    gxx = np.gradient(gx, sx, axis=0, edge_order=1)
    gyy = np.gradient(gy, sy, axis=1, edge_order=1)
    gzz = np.gradient(gz, sz, axis=2, edge_order=1)
    laplace = (gxx + gyy + gzz).astype(np.float32)

    curvature = percentile_pressure(laplace, 95.0)
    surface = np.exp(-np.square(sdf / max(band, 1e-6))).astype(np.float32)
    grad_disturb = percentile_pressure(grad_mag - gaussian_blur3d(grad_mag, entropy_sigma), 95.0)

    mat = material.astype(np.float32)
    mx = np.abs(np.diff(mat, axis=0, prepend=mat[[0], :, :]))
    my = np.abs(np.diff(mat, axis=1, prepend=mat[:, [0], :]))
    mz = np.abs(np.diff(mat, axis=2, prepend=mat[:, :, [0]]))
    material_boundary = normalize01(mx + my + mz)

    entropy = normalize01(
        gaussian_blur3d(
            0.40 * surface +
            0.24 * curvature +
            0.20 * grad_disturb +
            0.16 * material_boundary,
            entropy_sigma,
        )
    )

    t0, t1, t2 = lod_thresholds
    lod = np.zeros_like(entropy, dtype=np.uint8)
    lod[entropy >= t0] = 1
    lod[entropy >= t1] = 2
    lod[entropy >= t2] = 3

    grad_quality = np.exp(-np.square(grad_mag - 1.0)).astype(np.float32)
    sdf_quality = 1.0 - np.clip(np.abs(sdf_raw - sdf) / (band + 1e-6), 0.0, 1.0)
    rechts = normalize01(0.48 * grad_quality + 0.32 * sdf_quality + 0.20 * (1.0 - curvature * 0.5))
    rechts = gaussian_blur3d(rechts.astype(np.float32), max(0.25, entropy_sigma * 0.5))

    moat = np.clip(np.abs(sdf) / max(band * 4.0, 1e-6), 0.0, 1.0).astype(np.float32)
    band_index = np.zeros_like(occupancy, dtype=np.uint8)
    band_index[np.abs(sdf) <= band] = 3
    band_index[(np.abs(sdf) > band) & (np.abs(sdf) <= 2.0 * band)] = 2
    band_index[(np.abs(sdf) > 2.0 * band) & (np.abs(sdf) <= 4.0 * band)] = 1

    value_pressure = normalize01(0.45 * entropy + 0.35 * surface + 0.20 * (1.0 - moat))
    value_pressure = gaussian_blur3d(value_pressure, max(0.2, entropy_sigma * 0.35))

    chunk_id, chunk_grid = build_chunk_id(tuple(int(v) for v in sdf.shape), chunk_size)
    cscore = chunk_scores(entropy, value_pressure, chunk_size)
    cbounds = chunk_bounds(tuple(int(v) for v in sdf.shape), bounds_min, spacing, chunk_size, chunk_grid)
    seam = seam_mask(tuple(int(v) for v in sdf.shape), chunk_size)

    mip_sdf_1 = downsample2(sdf)
    mip_entropy_1 = downsample2(entropy)
    mip_sdf_2 = downsample2(mip_sdf_1)
    mip_entropy_2 = downsample2(mip_entropy_1)

    return MetaLayers(
        sdf_raw=sdf_raw.astype(np.float32),
        sdf=sdf.astype(np.float32),
        occupancy=occupancy,
        material=material.astype(np.int16),
        gradient=gradient,
        normal=normal.astype(np.float32),
        curvature=curvature.astype(np.float32),
        surface=surface.astype(np.float32),
        entropy=entropy.astype(np.float32),
        lod=lod,
        rechts=rechts.astype(np.float32),
        value_pressure=value_pressure.astype(np.float32),
        moat=moat,
        band_index=band_index,
        chunk_id=chunk_id,
        chunk_score=cscore,
        chunk_bounds=cbounds,
        seam=seam,
        mip_sdf_1=mip_sdf_1,
        mip_entropy_1=mip_entropy_1,
        mip_sdf_2=mip_sdf_2,
        mip_entropy_2=mip_entropy_2,
        bounds_min=tuple(float(x) for x in bounds_min),
        bounds_max=tuple(float(x) for x in bounds_max),
        spacing=tuple(float(x) for x in spacing),
        chunk_size=tuple(int(x) for x in chunk_size),
    )
