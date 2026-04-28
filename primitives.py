from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, List, Tuple
import numpy as np

Vec3 = Tuple[float, float, float]


def _v3(value: Any, name: str) -> Vec3:
    if len(value) != 3:
        raise ValueError(f"{name} must contain exactly 3 numbers.")
    return (float(value[0]), float(value[1]), float(value[2]))


@dataclass(frozen=True)
class SDFPrimitive:
    material_id: int = 1
    op: str = "union"
    smooth_k: float = 0.0

    def eval_grid(self, x: np.ndarray, y: np.ndarray, z: np.ndarray) -> np.ndarray:
        raise NotImplementedError

    def to_dict(self) -> Dict[str, Any]:
        raise NotImplementedError


@dataclass(frozen=True)
class Sphere(SDFPrimitive):
    center: Vec3 = (0.0, 0.0, 0.0)
    radius: float = 1.0

    def eval_grid(self, x: np.ndarray, y: np.ndarray, z: np.ndarray) -> np.ndarray:
        cx, cy, cz = self.center
        return np.sqrt((x - cx) ** 2 + (y - cy) ** 2 + (z - cz) ** 2) - self.radius

    def to_dict(self) -> Dict[str, Any]:
        return {"type": "sphere", "center": list(self.center), "radius": self.radius, "material_id": self.material_id, "op": self.op, "smooth_k": self.smooth_k}


@dataclass(frozen=True)
class Box(SDFPrimitive):
    center: Vec3 = (0.0, 0.0, 0.0)
    half_extents: Vec3 = (1.0, 1.0, 1.0)

    def eval_grid(self, x: np.ndarray, y: np.ndarray, z: np.ndarray) -> np.ndarray:
        cx, cy, cz = self.center
        hx, hy, hz = self.half_extents
        qx = np.abs(x - cx) - hx
        qy = np.abs(y - cy) - hy
        qz = np.abs(z - cz) - hz
        outside = np.sqrt(np.maximum(qx, 0) ** 2 + np.maximum(qy, 0) ** 2 + np.maximum(qz, 0) ** 2)
        inside = np.minimum(np.maximum(qx, np.maximum(qy, qz)), 0.0)
        return outside + inside

    def to_dict(self) -> Dict[str, Any]:
        return {"type": "box", "center": list(self.center), "half_extents": list(self.half_extents), "material_id": self.material_id, "op": self.op, "smooth_k": self.smooth_k}


@dataclass(frozen=True)
class Capsule(SDFPrimitive):
    a: Vec3 = (-1.0, 0.0, 0.0)
    b: Vec3 = (1.0, 0.0, 0.0)
    radius: float = 0.25

    def eval_grid(self, x: np.ndarray, y: np.ndarray, z: np.ndarray) -> np.ndarray:
        ax, ay, az = self.a
        bx, by, bz = self.b
        pax, pay, paz = x - ax, y - ay, z - az
        bax, bay, baz = bx - ax, by - ay, bz - az
        baba = bax * bax + bay * bay + baz * baz
        if baba <= 1e-12:
            return np.sqrt(pax * pax + pay * pay + paz * paz) - self.radius
        h = np.clip((pax * bax + pay * bay + paz * baz) / baba, 0.0, 1.0)
        dx, dy, dz = pax - bax * h, pay - bay * h, paz - baz * h
        return np.sqrt(dx * dx + dy * dy + dz * dz) - self.radius

    def to_dict(self) -> Dict[str, Any]:
        return {"type": "capsule", "a": list(self.a), "b": list(self.b), "radius": self.radius, "material_id": self.material_id, "op": self.op, "smooth_k": self.smooth_k}


@dataclass(frozen=True)
class Torus(SDFPrimitive):
    center: Vec3 = (0.0, 0.0, 0.0)
    major_radius: float = 0.9
    minor_radius: float = 0.18

    def eval_grid(self, x: np.ndarray, y: np.ndarray, z: np.ndarray) -> np.ndarray:
        cx, cy, cz = self.center
        px, py, pz = x - cx, y - cy, z - cz
        qx = np.sqrt(px * px + pz * pz) - self.major_radius
        qy = py
        return np.sqrt(qx * qx + qy * qy) - self.minor_radius

    def to_dict(self) -> Dict[str, Any]:
        return {"type": "torus", "center": list(self.center), "major_radius": self.major_radius, "minor_radius": self.minor_radius, "material_id": self.material_id, "op": self.op, "smooth_k": self.smooth_k}


@dataclass(frozen=True)
class Plane(SDFPrimitive):
    normal: Vec3 = (0.0, 1.0, 0.0)
    offset: float = 0.0

    def eval_grid(self, x: np.ndarray, y: np.ndarray, z: np.ndarray) -> np.ndarray:
        nx, ny, nz = self.normal
        length = max((nx * nx + ny * ny + nz * nz) ** 0.5, 1e-8)
        nx, ny, nz = nx / length, ny / length, nz / length
        return x * nx + y * ny + z * nz + self.offset

    def to_dict(self) -> Dict[str, Any]:
        return {"type": "plane", "normal": list(self.normal), "offset": self.offset, "material_id": self.material_id, "op": self.op, "smooth_k": self.smooth_k}


def primitive_from_dict(d: Dict[str, Any]) -> SDFPrimitive:
    kind = str(d.get("type", "")).lower()
    common = dict(material_id=int(d.get("material_id", 1)), op=str(d.get("op", "union")), smooth_k=float(d.get("smooth_k", 0.0)))

    if kind == "sphere":
        return Sphere(center=_v3(d.get("center", (0, 0, 0)), "center"), radius=float(d.get("radius", 1.0)), **common)
    if kind == "box":
        return Box(center=_v3(d.get("center", (0, 0, 0)), "center"), half_extents=_v3(d.get("half_extents", (1, 1, 1)), "half_extents"), **common)
    if kind == "capsule":
        return Capsule(a=_v3(d.get("a", (-1, 0, 0)), "a"), b=_v3(d.get("b", (1, 0, 0)), "b"), radius=float(d.get("radius", 0.25)), **common)
    if kind == "torus":
        return Torus(center=_v3(d.get("center", (0, 0, 0)), "center"), major_radius=float(d.get("major_radius", 0.9)), minor_radius=float(d.get("minor_radius", 0.18)), **common)
    if kind == "plane":
        return Plane(normal=_v3(d.get("normal", (0, 1, 0)), "normal"), offset=float(d.get("offset", 0.0)), **common)

    raise ValueError(f"Unknown primitive type: {kind!r}")


def primitives_from_json(scene: Dict[str, Any]) -> List[SDFPrimitive]:
    primitives = scene.get("primitives", [])
    if not primitives:
        raise ValueError("Scene must contain at least one primitive.")
    return [primitive_from_dict(p) for p in primitives]
