from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, List, Tuple
import shutil

from .config import BuildConfig, profile_config
from .field import MetaMap3D, write_json
from .mesh import Mesh, extract_mesh_marching_tetrahedra
from .metamap import MetaLayers
from .primitives import Box, Capsule, Plane, SDFPrimitive, Sphere, Torus
from .qa import strict_validation_gate, package_value_score


class InterfaceManager:
    def __init__(
        self,
        bounds_min: Tuple[float, float, float] = (-2.5, -2.5, -2.5),
        bounds_max: Tuple[float, float, float] = (2.5, 2.5, 2.5),
        config: BuildConfig | None = None,
    ) -> None:
        self.config = config or profile_config("game")
        self.map = MetaMap3D(bounds_min, bounds_max, self.config.resolution)
        self.primitives: List[SDFPrimitive] = []
        self.meta: MetaLayers | None = None
        self.mesh: Mesh | None = None
        self.validation: Dict[str, Any] | None = None
        self.build_report: Dict[str, Any] | None = None

    def add_sphere(self, center=(0,0,0), radius=1.0, material_id=1, op="union", smooth_k=0.0) -> "InterfaceManager":
        self.primitives.append(Sphere(center=tuple(float(v) for v in center), radius=float(radius), material_id=int(material_id), op=op, smooth_k=float(smooth_k)))
        return self

    def add_box(self, center=(0,0,0), half_extents=(1,1,1), material_id=1, op="union", smooth_k=0.0) -> "InterfaceManager":
        self.primitives.append(Box(center=tuple(float(v) for v in center), half_extents=tuple(float(v) for v in half_extents), material_id=int(material_id), op=op, smooth_k=float(smooth_k)))
        return self

    def add_capsule(self, a=(-1,0,0), b=(1,0,0), radius=0.25, material_id=1, op="union", smooth_k=0.0) -> "InterfaceManager":
        self.primitives.append(Capsule(a=tuple(float(v) for v in a), b=tuple(float(v) for v in b), radius=float(radius), material_id=int(material_id), op=op, smooth_k=float(smooth_k)))
        return self

    def add_torus(self, center=(0,0,0), major_radius=0.9, minor_radius=0.18, material_id=1, op="union", smooth_k=0.0) -> "InterfaceManager":
        self.primitives.append(Torus(center=tuple(float(v) for v in center), major_radius=float(major_radius), minor_radius=float(minor_radius), material_id=int(material_id), op=op, smooth_k=float(smooth_k)))
        return self

    def add_plane(self, normal=(0,1,0), offset=0.0, material_id=1, op="intersect", smooth_k=0.0) -> "InterfaceManager":
        self.primitives.append(Plane(normal=tuple(float(v) for v in normal), offset=float(offset), material_id=int(material_id), op=op, smooth_k=float(smooth_k)))
        return self

    def scene_dict(self) -> Dict[str, Any]:
        return {
            "format": "sdfpack-rechts-v3-scene",
            "bounds_min": list(self.map.bounds_min),
            "bounds_max": list(self.map.bounds_max),
            "resolution": list(self.map.resolution),
            "profile": self.config.mode,
            "primitives": [p.to_dict() for p in self.primitives],
        }

    def build_meta(self) -> MetaLayers:
        self.meta, self.build_report = self.map.build(self.primitives, self.config)
        return self.meta

    def build_mesh(self) -> Mesh:
        if self.meta is None:
            self.build_meta()
        assert self.meta is not None
        self.mesh = extract_mesh_marching_tetrahedra(
            self.meta.sdf,
            self.meta.bounds_min,
            self.meta.bounds_max,
            iso=self.config.iso,
            weld_precision=self.config.weld_precision,
            solidify=self.config.solidify_mesh,
        )
        self.validation = self.mesh.validation_report()
        ok, reasons = strict_validation_gate(self.validation, self.config.min_solid_rechts_score)
        self.validation["strict_gate_passed"] = bool(ok)
        self.validation["strict_gate_reasons"] = reasons
        if self.config.strict_solid_gate and not ok:
            raise RuntimeError("Strict solid gate failed: " + "; ".join(reasons))
        return self.mesh

    def export_all(self, out_dir: str | Path, include_runtime_headers: bool = True) -> Dict[str, Any]:
        out = Path(out_dir)
        out.mkdir(parents=True, exist_ok=True)

        if self.meta is None:
            self.build_meta()
        if self.mesh is None:
            self.build_mesh()

        assert self.meta is not None
        assert self.mesh is not None
        assert self.validation is not None

        package_path = out / "package.npz"
        scene_path = out / "scene.json"
        validation_path = out / "validation.json"
        manifest_path = out / "manifest.json"

        self.meta.save_npz(package_path)
        write_json(scene_path, self.scene_dict())
        write_json(validation_path, self.validation)

        mesh_paths: Dict[str, str] = {}
        if self.config.export_obj:
            p = out / "mesh.obj"
            self.mesh.save_obj(p)
            mesh_paths["obj"] = str(p)
        if self.config.export_ply:
            p = out / "mesh.ply"
            self.mesh.save_ply(p)
            mesh_paths["ply"] = str(p)
        if self.config.export_stl:
            p = out / "mesh.stl"
            self.mesh.save_stl_ascii(p)
            mesh_paths["stl"] = str(p)

        runtime_paths = {}
        if include_runtime_headers:
            project_root = Path(__file__).resolve().parents[1]
            for rel in ["runtime_cpp/sdf_package_layout.hpp", "runtime_c/sdf_package_layout.h", "schemas/package_layout.json"]:
                src = project_root / rel
                if src.exists():
                    dst = out / rel
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(src, dst)
                    runtime_paths[rel] = str(dst)

        meta_stats = self.meta.stats()
        value_score = package_value_score(meta_stats, self.validation)
        manifest = {
            "format": "sdfpack-rechts-v3-output",
            "profile": self.config.mode,
            "config": {
                "resolution": list(self.config.resolution),
                "convolve_sigma": self.config.convolve_sigma,
                "entropy_sigma": self.config.entropy_sigma,
                "surface_band_voxels": self.config.surface_band_voxels,
                "chunk_size": list(self.config.chunk_size),
                "lod_thresholds": list(self.config.lod_thresholds),
                "iso": self.config.iso,
                "strict_solid_gate": self.config.strict_solid_gate,
                "min_solid_rechts_score": self.config.min_solid_rechts_score,
            },
            "build_report": self.build_report,
            "meta_stats": meta_stats,
            "mesh_validation": self.validation,
            "package_value_score": value_score,
            "outputs": {
                "package": str(package_path),
                "scene": str(scene_path),
                "validation": str(validation_path),
                "meshes": mesh_paths,
                "runtime": runtime_paths,
                "manifest": str(manifest_path),
            },
        }
        write_json(manifest_path, manifest)
        return manifest


def build_default_rechts_demo(config: BuildConfig | None = None) -> InterfaceManager:
    cfg = config or profile_config("game")
    im = InterfaceManager(
        bounds_min=(-2.9, -2.9, -2.9),
        bounds_max=(2.9, 2.9, 2.9),
        config=cfg,
    )

    im.add_sphere(center=(0.0, 0.0, 0.0), radius=1.12, material_id=1)
    im.add_box(
        center=(0.42, 0.00, 0.02),
        half_extents=(0.62, 0.78, 0.58),
        material_id=2,
        op="smooth_union",
        smooth_k=0.30,
    )
    im.add_capsule(
        a=(-1.25, -0.90, -0.38),
        b=(1.35, 0.98, 0.56),
        radius=0.22,
        material_id=3,
    )
    im.add_torus(
        center=(-0.20, 0.15, 0.0),
        major_radius=0.85,
        minor_radius=0.11,
        material_id=4,
        op="smooth_union",
        smooth_k=0.16,
    )
    im.add_sphere(
        center=(-0.45, 0.0, 0.20),
        radius=0.30,
        material_id=9,
        op="subtract",
    )
    return im
