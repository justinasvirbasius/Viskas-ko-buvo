from __future__ import annotations

from typing import Tuple
import numpy as np

Vec3 = Tuple[float, float, float]
Vec3i = Tuple[int, int, int]


def build_chunk_id(shape: Vec3i, chunk_size: Vec3i) -> tuple[np.ndarray, Vec3i]:
    nx, ny, nz = shape
    sx, sy, sz = chunk_size
    cx = (nx + sx - 1) // sx
    cy = (ny + sy - 1) // sy
    cz = (nz + sz - 1) // sz
    ix = (np.arange(nx) // sx)[:, None, None]
    iy = (np.arange(ny) // sy)[None, :, None]
    iz = (np.arange(nz) // sz)[None, None, :]
    chunk_id = ix * (cy * cz) + iy * cz + iz
    return chunk_id.astype(np.int32), (cx, cy, cz)


def chunk_scores(entropy: np.ndarray, value_pressure: np.ndarray, chunk_size: Vec3i) -> np.ndarray:
    nx, ny, nz = entropy.shape
    sx, sy, sz = chunk_size
    cx = (nx + sx - 1) // sx
    cy = (ny + sy - 1) // sy
    cz = (nz + sz - 1) // sz
    scores = np.zeros((cx, cy, cz), dtype=np.float32)
    for x in range(cx):
        for y in range(cy):
            for z in range(cz):
                e = entropy[x*sx:min((x+1)*sx,nx), y*sy:min((y+1)*sy,ny), z*sz:min((z+1)*sz,nz)]
                v = value_pressure[x*sx:min((x+1)*sx,nx), y*sy:min((y+1)*sy,ny), z*sz:min((z+1)*sz,nz)]
                scores[x, y, z] = float(0.40 * np.mean(e) + 0.30 * np.max(e) + 0.30 * np.mean(v))
    return scores


def chunk_bounds(shape: Vec3i, bounds_min: Vec3, spacing: Vec3, chunk_size: Vec3i, chunk_grid: Vec3i) -> np.ndarray:
    cx, cy, cz = chunk_grid
    sx, sy, sz = chunk_size
    bmin = np.asarray(bounds_min, dtype=np.float32)
    sp = np.asarray(spacing, dtype=np.float32)
    bounds = np.zeros((cx, cy, cz, 6), dtype=np.float32)
    for ix in range(cx):
        for iy in range(cy):
            for iz in range(cz):
                lo_idx = np.asarray([ix*sx, iy*sy, iz*sz], dtype=np.float32)
                hi_idx = np.asarray([
                    min((ix+1)*sx, shape[0]-1),
                    min((iy+1)*sy, shape[1]-1),
                    min((iz+1)*sz, shape[2]-1),
                ], dtype=np.float32)
                lo = bmin + lo_idx * sp
                hi = bmin + hi_idx * sp
                bounds[ix, iy, iz] = np.asarray([lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]], dtype=np.float32)
    return bounds


def seam_mask(shape: Vec3i, chunk_size: Vec3i) -> np.ndarray:
    nx, ny, nz = shape
    sx, sy, sz = chunk_size
    x = np.arange(nx)[:, None, None]
    y = np.arange(ny)[None, :, None]
    z = np.arange(nz)[None, None, :]
    mask = (
        (x % sx == 0) | (y % sy == 0) | (z % sz == 0) |
        (x % sx == sx - 1) | (y % sy == sy - 1) | (z % sz == sz - 1)
    )
    return mask.astype(np.uint8)
