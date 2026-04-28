from __future__ import annotations

import numpy as np


def gaussian_kernel1d(sigma: float, radius: int | None = None) -> np.ndarray:
    if sigma <= 0:
        return np.array([1.0], dtype=np.float32)
    if radius is None:
        radius = max(1, int(np.ceil(3.0 * sigma)))
    x = np.arange(-radius, radius + 1, dtype=np.float32)
    k = np.exp(-(x * x) / (2.0 * sigma * sigma))
    k /= np.sum(k)
    return k.astype(np.float32)


def convolve1d_edge(a: np.ndarray, kernel: np.ndarray, axis: int) -> np.ndarray:
    if kernel.size == 1:
        return a.astype(np.float32, copy=True)
    radius = kernel.size // 2
    pad = [(0, 0)] * a.ndim
    pad[axis] = (radius, radius)
    padded = np.pad(a, pad, mode="edge")
    moved = np.moveaxis(padded, axis, 0)
    out = np.empty((a.shape[axis],) + moved.shape[1:], dtype=np.float32)
    for i in range(a.shape[axis]):
        out[i] = np.tensordot(kernel, moved[i : i + kernel.size], axes=(0, 0))
    return np.moveaxis(out, 0, axis)


def gaussian_blur3d(a: np.ndarray, sigma: float) -> np.ndarray:
    k = gaussian_kernel1d(sigma)
    out = a.astype(np.float32, copy=True)
    out = convolve1d_edge(out, k, axis=0)
    out = convolve1d_edge(out, k, axis=1)
    out = convolve1d_edge(out, k, axis=2)
    return out.astype(np.float32)


def downsample2(a: np.ndarray) -> np.ndarray:
    if min(a.shape[:3]) < 2:
        return a.copy()
    slices = []
    for ox in (0, 1):
        for oy in (0, 1):
            for oz in (0, 1):
                slices.append(a[ox::2, oy::2, oz::2])
    min_shape = tuple(min(s.shape[i] for s in slices) for i in range(slices[0].ndim))
    cropped = [s[tuple(slice(0, m) for m in min_shape)] for s in slices]
    return (sum(cropped) / len(cropped)).astype(np.float32)
