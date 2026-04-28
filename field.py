from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, Iterable, List, Tuple
import json
import numpy as np

from .composition import compose_sdf
from .config import BuildConfig
from .convolution import gaussian_blur3d
from .metamap import MetaLayers, derive_meta_layers
from .primitives import SDFPrimitive, primitives_from_json

Vec3 = Tuple[float, float, float]
Vec3i = Tuple[int, int, int]


class MetaMap3D:
    def __init__(
        self,
        bounds_min: Vec3 = (-2.5, -2.5, -2.5),
        bounds_max: Vec3 = (2.5, 2.5, 2.5),
        resolution: Vec3i = (64, 64, 64),
    ) -> None:
        self.bounds_min = tuple(float(v) for v in bounds_min)
        self.bounds_max = tuple(float(v) for v in bounds_max)
        self.resolution = tuple(int(v) for v in resolution)
        self._validate()

    def _validate(self) -> None:
        if any(r < 3 for r in self.resolution):
            raise ValueError("Each resolution axis must be >= 3.")
        for lo, hi in zip(self.bounds_min, self.bounds_max):
            if hi <= lo:
                raise ValueError("bounds_max must be greater than bounds_min.")

    @property
    def spacing(self) -> Vec3:
        return tuple((self.bounds_max[i] - self.bounds_min[i]) / (self.resolution[i] - 1) for i in range(3))

    def coordinate_grid(self) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
        xs = np.linspace(self.bounds_min[0], self.bounds_max[0], self.resolution[0], dtype=np.float32)
        ys = np.linspace(self.bounds_min[1], self.bounds_max[1], self.resolution[1], dtype=np.float32)
        zs = np.linspace(self.bounds_min[2], self.bounds_max[2], self.resolution[2], dtype=np.float32)
        return np.meshgrid(xs, ys, zs, indexing="ij")

    def boundary_margin_report(self, sdf: np.ndarray) -> dict:
        border_values = np.concatenate([
            sdf[0, :, :].ravel(), sdf[-1, :, :].ravel(),
            sdf[:, 0, :].ravel(), sdf[:, -1, :].ravel(),
            sdf[:, :, 0].ravel(), sdf[:, :, -1].ravel(),
        ])
        return {
            "border_min_sdf": float(np.min(border_values)),
            "border_mean_sdf": float(np.mean(border_values)),
            "surface_touches_bounds": bool(np.min(border_values) <= 0.0),
        }

    def build(self, primitives: Iterable[SDFPrimitive], config: BuildConfig) -> tuple[MetaLayers, dict]:
        x, y, z = self.coordinate_grid()
        sdf = np.full(self.resolution, np.inf, dtype=np.float32)
        material = np.zeros(self.resolution, dtype=np.int16)

        count = 0
        operations = []
        for prim in primitives:
            count += 1
            operations.append({"op": prim.op, "material_id": int(prim.material_id), "type": prim.__class__.__name__})
            prim_sdf = prim.eval_grid(x, y, z).astype(np.float32)
            sdf, material = compose_sdf(
                sdf,
                material,
                prim_sdf,
                material_id=int(prim.material_id),
                op=prim.op,
                smooth_k=float(prim.smooth_k),
            )

        if count == 0:
            raise ValueError("Cannot build an SDF package without primitives.")

        if not np.all(np.isfinite(sdf)):
            sdf = np.where(np.isfinite(sdf), sdf, 1e6).astype(np.float32)

        raw_boundary = self.boundary_margin_report(sdf)
        sdf_packaged = gaussian_blur3d(sdf, config.convolve_sigma).astype(np.float32)
        packaged_boundary = self.boundary_margin_report(sdf_packaged)

        meta = derive_meta_layers(
            sdf_raw=sdf,
            sdf=sdf_packaged,
            material=material,
            bounds_min=self.bounds_min,
            bounds_max=self.bounds_max,
            spacing=self.spacing,
            chunk_size=config.chunk_size,
            entropy_sigma=config.entropy_sigma,
            surface_band_voxels=config.surface_band_voxels,
            lod_thresholds=config.lod_thresholds,
        )

        build_report = {
            "primitive_count": count,
            "operations": operations,
            "raw_boundary": raw_boundary,
            "packaged_boundary": packaged_boundary,
            "resolution": list(self.resolution),
            "spacing": list(self.spacing),
        }
        return meta, build_report


def load_scene(path: str | Path, override_resolution: Vec3i | None = None) -> tuple[MetaMap3D, List[SDFPrimitive], Dict[str, Any]]:
    path = Path(path)
    scene = json.loads(path.read_text(encoding="utf-8"))
    bounds_min = tuple(scene.get("bounds_min", (-2.5, -2.5, -2.5)))
    bounds_max = tuple(scene.get("bounds_max", (2.5, 2.5, 2.5)))
    resolution = tuple(override_resolution or scene.get("resolution", (64, 64, 64)))
    return MetaMap3D(bounds_min, bounds_max, resolution), primitives_from_json(scene), scene


def write_json(path: str | Path, data: Dict[str, Any]) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")
