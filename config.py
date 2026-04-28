from __future__ import annotations

from dataclasses import dataclass
from typing import Tuple

Vec3i = Tuple[int, int, int]


@dataclass(frozen=True)
class BuildConfig:
    mode: str = "game"
    resolution: Vec3i = (64, 64, 64)
    convolve_sigma: float = 0.45
    entropy_sigma: float = 0.80
    surface_band_voxels: float = 2.5
    chunk_size: Vec3i = (16, 16, 16)
    lod_thresholds: Tuple[float, float, float] = (0.30, 0.55, 0.78)
    iso: float = 0.0
    weld_precision: float = 1e-6
    solidify_mesh: bool = True
    strict_solid_gate: bool = True
    min_solid_rechts_score: float = 0.999
    export_obj: bool = True
    export_ply: bool = True
    export_stl: bool = True

    def with_resolution(self, resolution: Vec3i | None) -> "BuildConfig":
        if resolution is None:
            return self
        return BuildConfig(
            mode=self.mode,
            resolution=tuple(int(v) for v in resolution),
            convolve_sigma=self.convolve_sigma,
            entropy_sigma=self.entropy_sigma,
            surface_band_voxels=self.surface_band_voxels,
            chunk_size=self.chunk_size,
            lod_thresholds=self.lod_thresholds,
            iso=self.iso,
            weld_precision=self.weld_precision,
            solidify_mesh=self.solidify_mesh,
            strict_solid_gate=self.strict_solid_gate,
            min_solid_rechts_score=self.min_solid_rechts_score,
            export_obj=self.export_obj,
            export_ply=self.export_ply,
            export_stl=self.export_stl,
        )

    def relaxed(self) -> "BuildConfig":
        return BuildConfig(
            mode=self.mode,
            resolution=self.resolution,
            convolve_sigma=self.convolve_sigma,
            entropy_sigma=self.entropy_sigma,
            surface_band_voxels=self.surface_band_voxels,
            chunk_size=self.chunk_size,
            lod_thresholds=self.lod_thresholds,
            iso=self.iso,
            weld_precision=self.weld_precision,
            solidify_mesh=self.solidify_mesh,
            strict_solid_gate=False,
            min_solid_rechts_score=self.min_solid_rechts_score,
            export_obj=self.export_obj,
            export_ply=self.export_ply,
            export_stl=self.export_stl,
        )


def profile_config(profile: str) -> BuildConfig:
    p = profile.lower().strip()

    if p == "debug":
        return BuildConfig(
            mode="debug",
            resolution=(32, 32, 32),
            convolve_sigma=0.25,
            entropy_sigma=0.55,
            chunk_size=(8, 8, 8),
            lod_thresholds=(0.25, 0.50, 0.75),
            min_solid_rechts_score=0.995,
        )

    if p == "game":
        return BuildConfig(
            mode="game",
            resolution=(64, 64, 64),
            convolve_sigma=0.45,
            entropy_sigma=0.80,
            chunk_size=(16, 16, 16),
            lod_thresholds=(0.30, 0.55, 0.78),
            min_solid_rechts_score=0.999,
        )

    if p == "collision":
        return BuildConfig(
            mode="collision",
            resolution=(48, 48, 48),
            convolve_sigma=0.75,
            entropy_sigma=1.10,
            chunk_size=(16, 16, 16),
            lod_thresholds=(0.40, 0.65, 0.85),
            min_solid_rechts_score=0.999,
        )

    if p in ("high", "high_detail", "asset"):
        return BuildConfig(
            mode="high",
            resolution=(96, 96, 96),
            convolve_sigma=0.35,
            entropy_sigma=0.65,
            chunk_size=(16, 16, 16),
            lod_thresholds=(0.22, 0.45, 0.70),
            min_solid_rechts_score=0.999,
        )

    raise ValueError(f"Unknown profile {profile!r}. Use debug, game, collision, or high.")
