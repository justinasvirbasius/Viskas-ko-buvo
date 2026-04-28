from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Sequence, Tuple
from collections import defaultdict, deque
import numpy as np

Vec3 = Tuple[float, float, float]


@dataclass
class Mesh:
    vertices: np.ndarray
    faces: np.ndarray
    normals: np.ndarray | None = None

    def remove_degenerate_faces(self, eps: float = 1e-12) -> int:
        if len(self.faces) == 0:
            return 0
        keep = []
        removed = 0
        for f in self.faces:
            a, b, c = self.vertices[f[0]], self.vertices[f[1]], self.vertices[f[2]]
            area2 = np.linalg.norm(np.cross(b - a, c - a))
            if area2 > eps and len(set(int(x) for x in f)) == 3:
                keep.append(f)
            else:
                removed += 1
        self.faces = np.asarray(keep, dtype=np.int32) if keep else np.zeros((0, 3), dtype=np.int32)
        return removed

    def compute_normals(self) -> np.ndarray:
        normals = np.zeros_like(self.vertices, dtype=np.float32)
        for a, b, c in self.faces:
            va, vb, vc = self.vertices[a], self.vertices[b], self.vertices[c]
            n = np.cross(vb - va, vc - va)
            length = float(np.linalg.norm(n))
            if length > 1e-12:
                n = n / length
                normals[a] += n
                normals[b] += n
                normals[c] += n
        lengths = np.linalg.norm(normals, axis=1)
        safe = lengths > 1e-12
        normals[safe] /= lengths[safe][:, None]
        normals[~safe] = np.array([0.0, 1.0, 0.0], dtype=np.float32)
        self.normals = normals.astype(np.float32)
        return self.normals

    def signed_volume(self) -> float:
        vol = 0.0
        for a, b, c in self.faces:
            va, vb, vc = self.vertices[a], self.vertices[b], self.vertices[c]
            vol += float(np.dot(va, np.cross(vb, vc))) / 6.0
        return vol

    def surface_area(self) -> float:
        area = 0.0
        for a, b, c in self.faces:
            va, vb, vc = self.vertices[a], self.vertices[b], self.vertices[c]
            area += 0.5 * float(np.linalg.norm(np.cross(vb - va, vc - va)))
        return area

    def edge_counts(self) -> Dict[Tuple[int, int], int]:
        counts: Dict[Tuple[int, int], int] = defaultdict(int)
        for a, b, c in self.faces:
            for u, v in ((a, b), (b, c), (c, a)):
                key = (int(min(u, v)), int(max(u, v)))
                counts[key] += 1
        return dict(counts)

    def orient_consistently(self) -> None:
        if len(self.faces) == 0:
            return

        edge_to_faces: Dict[Tuple[int, int], List[Tuple[int, int]]] = defaultdict(list)
        for fi, (a, b, c) in enumerate(self.faces):
            for u, v in ((a, b), (b, c), (c, a)):
                edge_to_faces[(int(min(u, v)), int(max(u, v)))].append((fi, 1 if u < v else -1))

        adjacency: Dict[int, List[Tuple[int, bool]]] = defaultdict(list)
        for entries in edge_to_faces.values():
            if len(entries) != 2:
                continue
            (f0, dir0), (f1, dir1) = entries
            should_flip = (dir0 == dir1)
            adjacency[f0].append((f1, should_flip))
            adjacency[f1].append((f0, should_flip))

        visited = np.zeros(len(self.faces), dtype=np.uint8)
        flip = np.zeros(len(self.faces), dtype=np.uint8)
        for start in range(len(self.faces)):
            if visited[start]:
                continue
            visited[start] = 1
            q = deque([start])
            while q:
                f = q.popleft()
                for nb, should_flip in adjacency.get(f, []):
                    desired = flip[f] ^ int(should_flip)
                    if not visited[nb]:
                        visited[nb] = 1
                        flip[nb] = desired
                        q.append(nb)

        for i in range(len(self.faces)):
            if flip[i]:
                self.faces[i, [1, 2]] = self.faces[i, [2, 1]]

        if self.signed_volume() < 0:
            self.faces[:, [1, 2]] = self.faces[:, [2, 1]]
        self.compute_normals()

    def fill_boundary_holes(self, max_loop_edges: int = 512) -> int:
        counts = self.edge_counts()
        boundary_edges = [edge for edge, count in counts.items() if count == 1]
        if not boundary_edges:
            return 0

        adjacency: Dict[int, List[int]] = defaultdict(list)
        edge_set = set()
        for a, b in boundary_edges:
            adjacency[a].append(b)
            adjacency[b].append(a)
            edge_set.add((min(a, b), max(a, b)))

        visited_edges = set()
        loops: List[List[int]] = []

        for start_edge in list(edge_set):
            if start_edge in visited_edges:
                continue
            a, b = start_edge
            loop = [a, b]
            visited_edges.add(start_edge)
            prev, cur = a, b

            for _ in range(max_loop_edges):
                candidates = [
                    n for n in adjacency[cur]
                    if (min(cur, n), max(cur, n)) not in visited_edges and n != prev
                ]
                if not candidates:
                    break
                nxt = candidates[0]
                e = (min(cur, nxt), max(cur, nxt))
                visited_edges.add(e)
                if nxt == loop[0]:
                    break
                loop.append(nxt)
                prev, cur = cur, nxt

            if len(loop) >= 3:
                if loop[-1] == loop[0]:
                    loop = loop[:-1]
                loops.append(loop)

        new_vertices = [v for v in self.vertices]
        new_faces = [tuple(int(x) for x in f) for f in self.faces]
        patched_faces = 0

        for loop in loops:
            if len(loop) < 3 or len(loop) > max_loop_edges:
                continue
            pts = self.vertices[np.asarray(loop, dtype=np.int32)]
            centroid = np.mean(pts, axis=0).astype(np.float32)
            if float(np.max(np.linalg.norm(pts - centroid[None, :], axis=1))) < 1e-8:
                continue

            center_idx = len(new_vertices)
            new_vertices.append(centroid)
            n = len(loop)
            for i in range(n):
                a = int(loop[i])
                b = int(loop[(i + 1) % n])
                if a != b:
                    new_faces.append((a, b, center_idx))
                    patched_faces += 1

        self.vertices = np.asarray(new_vertices, dtype=np.float32)
        self.faces = np.asarray(new_faces, dtype=np.int32) if new_faces else np.zeros((0, 3), dtype=np.int32)
        self.remove_degenerate_faces()
        self.orient_consistently()
        return patched_faces

    def validation_report(self) -> dict:
        removed = self.remove_degenerate_faces()
        if self.normals is None:
            self.compute_normals()

        counts = self.edge_counts()
        boundary_edges = sum(1 for v in counts.values() if v == 1)
        nonmanifold_edges = sum(1 for v in counts.values() if v > 2)
        vertices = int(len(self.vertices))
        edges = int(len(counts))
        faces = int(len(self.faces))
        euler = vertices - edges + faces
        volume = float(self.signed_volume())
        area = float(self.surface_area())

        closed = boundary_edges == 0
        manifold = nonmanifold_edges == 0 and closed
        oriented = volume > 0.0

        score = 1.0
        if not closed:
            score -= min(0.55, boundary_edges / max(1, edges))
        if nonmanifold_edges:
            score -= min(0.40, nonmanifold_edges / max(1, edges))
        if not oriented:
            score -= 0.20
        if faces == 0 or area <= 1e-12 or volume <= 1e-12:
            score -= 0.60
        score = max(0.0, min(1.0, score))

        return {
            "vertex_count": vertices,
            "edge_count": edges,
            "face_count": faces,
            "removed_degenerate_faces_last_check": int(removed),
            "boundary_edge_count": int(boundary_edges),
            "nonmanifold_edge_count": int(nonmanifold_edges),
            "euler_characteristic": int(euler),
            "signed_volume": volume,
            "surface_area": area,
            "closed": bool(closed),
            "manifold": bool(manifold),
            "oriented_positive_volume": bool(oriented),
            "solid_rechts_score": float(score),
        }

    def save_obj(self, path: str | Path) -> None:
        if self.normals is None:
            self.compute_normals()
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", encoding="utf-8") as f:
            f.write("# OBJ generated by sdfpack rechts v3\n")
            for v in self.vertices:
                f.write(f"v {v[0]:.7f} {v[1]:.7f} {v[2]:.7f}\n")
            for n in self.normals:
                f.write(f"vn {n[0]:.7f} {n[1]:.7f} {n[2]:.7f}\n")
            for a, b, c in self.faces:
                a1, b1, c1 = int(a) + 1, int(b) + 1, int(c) + 1
                f.write(f"f {a1}//{a1} {b1}//{b1} {c1}//{c1}\n")

    def save_ply(self, path: str | Path) -> None:
        if self.normals is None:
            self.compute_normals()
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", encoding="utf-8") as f:
            f.write("ply\nformat ascii 1.0\n")
            f.write(f"element vertex {len(self.vertices)}\n")
            f.write("property float x\nproperty float y\nproperty float z\n")
            f.write("property float nx\nproperty float ny\nproperty float nz\n")
            f.write(f"element face {len(self.faces)}\n")
            f.write("property list uchar int vertex_indices\nend_header\n")
            for v, n in zip(self.vertices, self.normals):
                f.write(f"{v[0]:.7f} {v[1]:.7f} {v[2]:.7f} {n[0]:.7f} {n[1]:.7f} {n[2]:.7f}\n")
            for a, b, c in self.faces:
                f.write(f"3 {int(a)} {int(b)} {int(c)}\n")

    def save_stl_ascii(self, path: str | Path, name: str = "sdfpack_rechts_v3_mesh") -> None:
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", encoding="utf-8") as f:
            f.write(f"solid {name}\n")
            for a, b, c in self.faces:
                va, vb, vc = self.vertices[a], self.vertices[b], self.vertices[c]
                n = np.cross(vb - va, vc - va)
                l = float(np.linalg.norm(n))
                if l > 1e-12:
                    n = n / l
                else:
                    n = np.array([0.0, 1.0, 0.0], dtype=np.float32)
                f.write(f"  facet normal {n[0]:.7e} {n[1]:.7e} {n[2]:.7e}\n")
                f.write("    outer loop\n")
                for v in (va, vb, vc):
                    f.write(f"      vertex {v[0]:.7e} {v[1]:.7e} {v[2]:.7e}\n")
                f.write("    endloop\n  endfacet\n")
            f.write(f"endsolid {name}\n")


_CORNERS = np.array([[0,0,0], [1,0,0], [1,1,0], [0,1,0], [0,0,1], [1,0,1], [1,1,1], [0,1,1]], dtype=np.int32)
_TETS = ((0, 5, 1, 6), (0, 1, 2, 6), (0, 2, 3, 6), (0, 3, 7, 6), (0, 7, 4, 6), (0, 4, 5, 6))


def _interp(p1: np.ndarray, p2: np.ndarray, v1: float, v2: float, iso: float) -> np.ndarray:
    denom = v2 - v1
    if abs(denom) < 1e-12:
        return (p1 + p2) * 0.5
    t = (iso - v1) / denom
    return p1 + t * (p2 - p1)


def _tet_triangles(pos: Sequence[np.ndarray], val: Sequence[float], iso: float) -> List[Tuple[np.ndarray, np.ndarray, np.ndarray]]:
    inside = [i for i, v in enumerate(val) if v <= iso]
    outside = [i for i, v in enumerate(val) if v > iso]
    if len(inside) == 0 or len(inside) == 4:
        return []
    if len(inside) == 1:
        i = inside[0]
        pts = [_interp(pos[i], pos[o], val[i], val[o], iso) for o in outside]
        return [(pts[0], pts[1], pts[2])]
    if len(inside) == 3:
        o = outside[0]
        pts = [_interp(pos[o], pos[i], val[o], val[i], iso) for i in inside]
        return [(pts[2], pts[1], pts[0])]
    i0, i1 = inside
    o0, o1 = outside
    p0 = _interp(pos[i0], pos[o0], val[i0], val[o0], iso)
    p1 = _interp(pos[i0], pos[o1], val[i0], val[o1], iso)
    p2 = _interp(pos[i1], pos[o0], val[i1], val[o0], iso)
    p3 = _interp(pos[i1], pos[o1], val[i1], val[o1], iso)
    return [(p0, p2, p1), (p1, p2, p3)]


def _dedupe_vertices(raw_vertices: List[np.ndarray], raw_faces: List[Tuple[int, int, int]], precision: float) -> tuple[np.ndarray, np.ndarray]:
    index: Dict[Tuple[int, int, int], int] = {}
    vertices: List[np.ndarray] = []
    faces: List[Tuple[int, int, int]] = []

    def key(v: np.ndarray) -> Tuple[int, int, int]:
        return tuple(int(round(float(c) / precision)) for c in v)

    def get(v: np.ndarray) -> int:
        k = key(v)
        found = index.get(k)
        if found is not None:
            return found
        idx = len(vertices)
        index[k] = idx
        vertices.append(v.astype(np.float32))
        return idx

    for a, b, c in raw_faces:
        ia, ib, ic = get(raw_vertices[a]), get(raw_vertices[b]), get(raw_vertices[c])
        if ia != ib and ib != ic and ia != ic:
            faces.append((ia, ib, ic))

    return (
        np.asarray(vertices, dtype=np.float32),
        np.asarray(faces, dtype=np.int32) if faces else np.zeros((0, 3), dtype=np.int32),
    )


def extract_mesh_marching_tetrahedra(
    sdf: np.ndarray,
    bounds_min: Vec3,
    bounds_max: Vec3,
    iso: float = 0.0,
    weld_precision: float = 1e-6,
    solidify: bool = True,
) -> Mesh:
    nx, ny, nz = sdf.shape
    bmin = np.asarray(bounds_min, dtype=np.float32)
    bmax = np.asarray(bounds_max, dtype=np.float32)
    scale = (bmax - bmin) / np.asarray((nx - 1, ny - 1, nz - 1), dtype=np.float32)

    raw_vertices: List[np.ndarray] = []
    raw_faces: List[Tuple[int, int, int]] = []

    for i in range(nx - 1):
        for j in range(ny - 1):
            for k in range(nz - 1):
                vals = []
                poses = []
                base = np.array([i, j, k], dtype=np.float32)
                for off in _CORNERS:
                    idx = base + off.astype(np.float32)
                    poses.append(bmin + idx * scale)
                    vals.append(float(sdf[i + off[0], j + off[1], k + off[2]]))

                if min(vals) > iso or max(vals) < iso:
                    continue

                for tet in _TETS:
                    for tri in _tet_triangles([poses[t] for t in tet], [vals[t] for t in tet], iso):
                        start = len(raw_vertices)
                        raw_vertices.extend(tri)
                        raw_faces.append((start, start + 1, start + 2))

    if not raw_vertices:
        return Mesh(
            vertices=np.zeros((0, 3), dtype=np.float32),
            faces=np.zeros((0, 3), dtype=np.int32),
            normals=np.zeros((0, 3), dtype=np.float32),
        )

    vertices, faces = _dedupe_vertices(raw_vertices, raw_faces, weld_precision)
    mesh = Mesh(vertices=vertices, faces=faces)
    mesh.remove_degenerate_faces()
    if solidify:
        mesh.orient_consistently()
        before = mesh.validation_report()
        if before["boundary_edge_count"] > 0:
            mesh.fill_boundary_holes()
            mesh.orient_consistently()
    else:
        mesh.compute_normals()
    return mesh
