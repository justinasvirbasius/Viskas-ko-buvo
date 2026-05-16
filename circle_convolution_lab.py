
from __future__ import annotations

"""
circle_convolution_lab.py

A complete single-file program for the circle as a computational object:

    circle
    -> radians / phase
    -> complex unit circle
    -> circular statistics and entropy
    -> circular convolution
    -> Fourier modes
    -> winding number
    -> torus cycles

Requires:
    pip install numpy

Run:
    python circle_convolution_lab.py demo
    python circle_convolution_lab.py selftest
    python circle_convolution_lab.py demo --csv circle_demo.csv
"""

from dataclasses import dataclass
from enum import Enum
from math import atan2, cos, isclose, log, pi, sin, sqrt
from pathlib import Path
import argparse
import csv
from typing import Iterable, Literal

import numpy as np


TAU = 2.0 * pi


# ---------------------------------------------------------------------
# Core angle utilities
# ---------------------------------------------------------------------

def radians(degrees: float | np.ndarray) -> float | np.ndarray:
    """Convert degrees to radians."""
    return np.asarray(degrees) * pi / 180.0


def degrees(theta_rad: float | np.ndarray) -> float | np.ndarray:
    """Convert radians to degrees."""
    return np.asarray(theta_rad) * 180.0 / pi


def wrap_angle(theta_rad: float | np.ndarray, *, period: float = TAU) -> float | np.ndarray:
    """
    Wrap angle into [-period/2, period/2).

    For normal radians, this is [-π, π).
    """
    theta = np.asarray(theta_rad)
    wrapped = (theta + period / 2.0) % period - period / 2.0
    if np.ndim(theta_rad) == 0:
        return float(wrapped)
    return wrapped


def wrap_angle_positive(theta_rad: float | np.ndarray, *, period: float = TAU) -> float | np.ndarray:
    """
    Wrap angle into [0, period).

    For normal radians, this is [0, 2π).
    """
    theta = np.asarray(theta_rad)
    wrapped = theta % period
    if np.ndim(theta_rad) == 0:
        return float(wrapped)
    return wrapped


def unwrap_angles(theta_rad: np.ndarray) -> np.ndarray:
    """Remove artificial ±2π jumps from a phase sequence."""
    return np.unwrap(np.asarray(theta_rad, dtype=np.float64), period=TAU)


def phasor(theta_rad: float | np.ndarray) -> complex | np.ndarray:
    """
    Euler unit-circle map:

        z = e^(iθ)
    """
    theta = np.asarray(theta_rad, dtype=np.float64)
    z = np.exp(1j * theta)
    if np.ndim(theta_rad) == 0:
        return complex(z)
    return z


def angle_of(z: complex | np.ndarray) -> float | np.ndarray:
    """Return angle/phase of a complex number or array."""
    out = np.angle(z)
    if np.ndim(z) == 0:
        return float(out)
    return out


def angular_distance(a_rad: float | np.ndarray, b_rad: float | np.ndarray) -> float | np.ndarray:
    """Smallest signed angular displacement from b to a."""
    return wrap_angle(np.asarray(a_rad) - np.asarray(b_rad))


# ---------------------------------------------------------------------
# Circle geometry
# ---------------------------------------------------------------------

class Region(str, Enum):
    INSIDE = "inside"
    BOUNDARY = "boundary"
    OUTSIDE = "outside"


@dataclass(frozen=True)
class Circle:
    """
    Circle centered at (cx, cy) with radius r.

    Geometry:
        (x - cx)^2 + (y - cy)^2 = r^2

    Parameterization:
        x = cx + r cos(θ)
        y = cy + r sin(θ)
    """

    radius: float = 1.0
    center_x: float = 0.0
    center_y: float = 0.0

    def __post_init__(self) -> None:
        if self.radius <= 0.0:
            raise ValueError("Circle radius must be positive.")

    @property
    def circumference(self) -> float:
        return TAU * self.radius

    @property
    def area(self) -> float:
        return pi * self.radius * self.radius

    @property
    def curvature(self) -> float:
        """
        Constant circle curvature:

            κ = 1/r
        """
        return 1.0 / self.radius

    def arc_length(self, theta_rad: float) -> float:
        """
        Arc length corresponding to an angle:

            s = rθ
        """
        return self.radius * theta_rad

    def angle_from_arc_length(self, arc_length: float) -> float:
        """
        Radian definition:

            θ = s/r
        """
        return arc_length / self.radius

    def point(self, theta_rad: float | np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        """Point(s) on the circle at angle θ."""
        theta = np.asarray(theta_rad, dtype=np.float64)
        x = self.center_x + self.radius * np.cos(theta)
        y = self.center_y + self.radius * np.sin(theta)
        return x, y

    def complex_point(self, theta_rad: float | np.ndarray) -> complex | np.ndarray:
        """Complex form of point(s) on the circle."""
        return complex(self.center_x, self.center_y) + self.radius * phasor(theta_rad)

    def phase_of_point(self, x: float, y: float) -> float:
        """Angle of a point relative to the circle center."""
        return atan2(y - self.center_y, x - self.center_x)

    def radial_distance(self, x: float, y: float) -> float:
        return sqrt((x - self.center_x) ** 2 + (y - self.center_y) ** 2)

    def classify_point(self, x: float, y: float, *, tol: float = 1e-12) -> Region:
        """
        Classify a point as inside, on boundary, or outside.

        inside:
            distance < r

        boundary:
            distance ≈ r

        outside:
            distance > r
        """
        d = self.radial_distance(x, y)

        if isclose(d, self.radius, abs_tol=tol, rel_tol=tol):
            return Region.BOUNDARY

        if d < self.radius:
            return Region.INSIDE

        return Region.OUTSIDE


# ---------------------------------------------------------------------
# Circular grid and fields
# ---------------------------------------------------------------------

@dataclass(frozen=True)
class CircularGrid:
    """
    Uniform sample grid on the circle.

    The grid covers [0, 2π), endpoint excluded.
    """

    sample_count: int

    def __post_init__(self) -> None:
        if self.sample_count < 2:
            raise ValueError("sample_count must be at least 2.")

    @property
    def theta(self) -> np.ndarray:
        return np.linspace(0.0, TAU, self.sample_count, endpoint=False)

    @property
    def delta_theta(self) -> float:
        return TAU / self.sample_count

    @property
    def modes(self) -> np.ndarray:
        """
        Integer Fourier modes:
            0, 1, 2, ..., -3, -2, -1
        """
        return np.fft.fftfreq(self.sample_count, d=1.0 / self.sample_count).astype(int)


def von_mises_like_bump(theta: np.ndarray, *, center_rad: float, concentration: float) -> np.ndarray:
    """
    Smooth circular bump.

    This is an unnormalized von-Mises-like shape:
        exp(kappa * cos(theta - center))
    """
    if concentration < 0.0:
        raise ValueError("concentration must be nonnegative.")

    return np.exp(concentration * np.cos(theta - center_rad))


def normalize_density_on_circle(values: np.ndarray, delta_theta: float) -> np.ndarray:
    """
    Normalize sampled values so that:

        ∫ f(θ)dθ ≈ Σ f_j Δθ = 1
    """
    values = np.asarray(values, dtype=np.float64)

    if np.any(values < 0.0):
        raise ValueError("Density values must be nonnegative.")

    mass = float(np.sum(values) * delta_theta)

    if mass <= 0.0:
        raise ValueError("Cannot normalize density with zero mass.")

    return values / mass


# ---------------------------------------------------------------------
# Circular statistics and entropy
# ---------------------------------------------------------------------

@dataclass(frozen=True)
class CircularStats:
    mean_angle_rad: float
    mean_angle_deg: float
    resultant_length: float
    circular_variance: float
    circular_std_rad: float


def circular_stats(angles_rad: np.ndarray, weights: np.ndarray | None = None) -> CircularStats:
    """
    Circular mean and coherence/order.

    R = |mean(e^(iθ))|

    R close to 1:
        phases coalesce / align

    R close to 0:
        phases spread around circle
    """
    theta = np.asarray(angles_rad, dtype=np.float64)

    if theta.ndim != 1:
        raise ValueError("angles_rad must be a 1D array.")

    if len(theta) == 0:
        raise ValueError("angles_rad cannot be empty.")

    z = phasor(theta)

    if weights is None:
        mean_z = np.mean(z)
    else:
        w = np.asarray(weights, dtype=np.float64)
        if w.shape != theta.shape:
            raise ValueError("weights must have the same shape as angles_rad.")
        if np.any(w < 0.0):
            raise ValueError("weights must be nonnegative.")
        if np.sum(w) <= 0.0:
            raise ValueError("weights must have positive total.")
        mean_z = np.sum(w * z) / np.sum(w)

    mean_angle = float(np.angle(mean_z))
    resultant = float(abs(mean_z))
    variance = 1.0 - resultant

    # A standard circular deviation proxy.
    # Clamp for numerical safety.
    if resultant <= 0.0:
        circ_std = float("inf")
    else:
        circ_std = float(np.sqrt(max(0.0, -2.0 * np.log(min(resultant, 1.0)))))

    return CircularStats(
        mean_angle_rad=mean_angle,
        mean_angle_deg=float(degrees(mean_angle)),
        resultant_length=resultant,
        circular_variance=variance,
        circular_std_rad=circ_std,
    )


@dataclass(frozen=True)
class EntropyReport:
    entropy_nats: float
    entropy_bits: float
    normalized_entropy: float


def entropy_from_probabilities(probabilities: np.ndarray) -> EntropyReport:
    """
    Shannon entropy of a discrete probability vector:

        H = -Σ p_i log(p_i)
    """
    p = np.asarray(probabilities, dtype=np.float64)

    if p.ndim != 1:
        raise ValueError("probabilities must be 1D.")

    if np.any(p < 0.0):
        raise ValueError("probabilities must be nonnegative.")

    total = float(np.sum(p))
    if total <= 0.0:
        raise ValueError("probabilities must have positive total.")

    p = p / total
    p_nonzero = p[p > 0.0]

    h_nats = float(-np.sum(p_nonzero * np.log(p_nonzero)))
    h_bits = float(h_nats / log(2.0))

    if len(p) <= 1:
        normalized = 0.0
    else:
        normalized = float(h_nats / log(len(p)))

    return EntropyReport(h_nats, h_bits, normalized)


def phase_entropy(angles_rad: np.ndarray, *, bins: int = 64) -> EntropyReport:
    """
    Entropy of phases around the circle.

    Uniformly spread phases:
        high entropy

    Tightly clustered phases:
        low entropy
    """
    if bins < 2:
        raise ValueError("bins must be at least 2.")

    theta = wrap_angle_positive(np.asarray(angles_rad, dtype=np.float64))
    counts, _edges = np.histogram(theta, bins=bins, range=(0.0, TAU))
    return entropy_from_probabilities(counts)


def density_entropy_on_circle(density: np.ndarray, delta_theta: float) -> EntropyReport:
    """
    Differential-entropy-style discrete approximation for a circular density.

    First converts f(θ) into bin probabilities:
        p_j ≈ f(θ_j) Δθ
    """
    f = np.asarray(density, dtype=np.float64)

    if np.any(f < 0.0):
        raise ValueError("density must be nonnegative.")

    probabilities = f * delta_theta
    return entropy_from_probabilities(probabilities)


# ---------------------------------------------------------------------
# Circular convolution
# ---------------------------------------------------------------------

def circular_convolution_direct(
    f: np.ndarray,
    g: np.ndarray,
    *,
    delta_theta: float | None = None,
) -> np.ndarray:
    """
    Direct circular convolution.

    Discrete cyclic form:
        h[n] = Σ_m f[m] g[(n - m) mod N]

    Integral approximation on the circle:
        h(θ) = ∫ f(φ) g(θ - φ)dφ
        h[n] ≈ Δθ Σ_m f[m]g[n-m]

    If delta_theta is given, the result is scaled as an integral.
    """
    f = np.asarray(f)
    g = np.asarray(g)

    if f.ndim != 1 or g.ndim != 1:
        raise ValueError("f and g must be 1D arrays.")

    if f.shape != g.shape:
        raise ValueError("f and g must have the same shape.")

    n = len(f)
    h = np.zeros(n, dtype=np.result_type(f, g, np.float64))

    for i in range(n):
        total = 0.0
        for m in range(n):
            total += f[m] * g[(i - m) % n]
        h[i] = total

    if delta_theta is not None:
        h = h * delta_theta

    return h


def circular_convolution_fft(
    f: np.ndarray,
    g: np.ndarray,
    *,
    delta_theta: float | None = None,
) -> np.ndarray:
    """
    Fast circular convolution by FFT.

    Circular convolution in phase domain corresponds to multiplication
    in Fourier domain.
    """
    f = np.asarray(f)
    g = np.asarray(g)

    if f.ndim != 1 or g.ndim != 1:
        raise ValueError("f and g must be 1D arrays.")

    if f.shape != g.shape:
        raise ValueError("f and g must have the same shape.")

    h = np.fft.ifft(np.fft.fft(f) * np.fft.fft(g))

    if np.isrealobj(f) and np.isrealobj(g):
        h = h.real

    if delta_theta is not None:
        h = h * delta_theta

    return h


# ---------------------------------------------------------------------
# Fourier series on the circle
# ---------------------------------------------------------------------

@dataclass(frozen=True)
class FourierSeries:
    modes: np.ndarray
    coefficients: np.ndarray

    def power(self) -> np.ndarray:
        return np.abs(self.coefficients) ** 2

    def entropy(self) -> EntropyReport:
        return entropy_from_probabilities(self.power())


def fourier_series(values: np.ndarray) -> FourierSeries:
    """
    Fourier coefficients for samples around the circle.

    If:
        f(θ_j), θ_j = 2πj/N

    Then:
        c_k = FFT(f)_k / N

    and approximately:
        f(θ) = Σ_k c_k e^(ikθ)
    """
    values = np.asarray(values)

    if values.ndim != 1:
        raise ValueError("values must be 1D.")

    n = len(values)
    modes = np.fft.fftfreq(n, d=1.0 / n).astype(int)
    coefficients = np.fft.fft(values) / n
    return FourierSeries(modes=modes, coefficients=coefficients)


def reconstruct_from_fourier(series: FourierSeries, *, keep_modes: int | None = None) -> np.ndarray:
    """
    Reconstruct a signal from Fourier coefficients.

    If keep_modes is None:
        use all modes.

    If keep_modes is an integer:
        keep modes with |k| <= keep_modes and zero the rest.
    """
    coeffs = np.array(series.coefficients, copy=True)

    if keep_modes is not None:
        if keep_modes < 0:
            raise ValueError("keep_modes must be nonnegative.")
        coeffs[np.abs(series.modes) > keep_modes] = 0.0

    return np.fft.ifft(coeffs * len(coeffs))


def dominant_modes(series: FourierSeries, *, count: int = 5, ignore_zero: bool = False) -> list[tuple[int, float, float]]:
    """
    Return dominant Fourier modes as:

        (mode_number, amplitude, phase_rad)
    """
    if count <= 0:
        raise ValueError("count must be positive.")

    power = series.power().copy()

    if ignore_zero:
        power[series.modes == 0] = 0.0

    idx = np.argsort(power)[::-1][:count]

    out: list[tuple[int, float, float]] = []
    for i in idx:
        mode = int(series.modes[i])
        amplitude = float(abs(series.coefficients[i]))
        phase = float(np.angle(series.coefficients[i]))
        out.append((mode, amplitude, phase))

    return out


# ---------------------------------------------------------------------
# Winding number and closed return
# ---------------------------------------------------------------------

def winding_number_complex_path(path: np.ndarray, *, close_path: bool = True) -> float:
    """
    Winding number of a complex path around the origin.

    It measures net completed turns:
        +1 = one counterclockwise loop
        -1 = one clockwise loop
         0 = no net loop

    The path must not pass through the origin.
    """
    z = np.asarray(path, dtype=np.complex128)

    if z.ndim != 1:
        raise ValueError("path must be a 1D complex array.")

    if len(z) < 2:
        raise ValueError("path must contain at least two points.")

    if np.any(np.abs(z) < 1e-14):
        raise ValueError("path passes too close to the origin; winding is undefined.")

    if close_path:
        z = np.concatenate([z, z[:1]])

    increments = np.angle(z[1:] / z[:-1])
    return float(np.sum(increments) / TAU)


def winding_number_angles(angles_rad: np.ndarray) -> float:
    """
    Winding count from an unwrapped phase sequence:

        winding = Δθ / 2π
    """
    theta = unwrap_angles(np.asarray(angles_rad, dtype=np.float64))

    if theta.ndim != 1 or len(theta) < 2:
        raise ValueError("angles_rad must be a 1D array with at least two values.")

    return float((theta[-1] - theta[0]) / TAU)


# ---------------------------------------------------------------------
# Torus: two coupled circles
# ---------------------------------------------------------------------

@dataclass(frozen=True)
class TorusPointCloud:
    u: np.ndarray
    v: np.ndarray
    x: np.ndarray
    y: np.ndarray
    z: np.ndarray


def torus_knot(
    *,
    p: int,
    q: int,
    major_radius: float = 2.0,
    minor_radius: float = 0.6,
    sample_count: int = 1024,
) -> TorusPointCloud:
    """
    Generate a torus knot.

    p and q are winding counts around the two torus cycles.

    This encodes:
        circle × circle = torus
    """
    if sample_count < 4:
        raise ValueError("sample_count must be at least 4.")

    if major_radius <= 0.0 or minor_radius <= 0.0:
        raise ValueError("radii must be positive.")

    t = np.linspace(0.0, TAU, sample_count, endpoint=False)

    u = p * t
    v = q * t

    x = (major_radius + minor_radius * np.cos(v)) * np.cos(u)
    y = (major_radius + minor_radius * np.cos(v)) * np.sin(u)
    z = minor_radius * np.sin(v)

    return TorusPointCloud(u=u, v=v, x=x, y=y, z=z)


# ---------------------------------------------------------------------
# Full report
# ---------------------------------------------------------------------

@dataclass(frozen=True)
class CircleConvolutionReport:
    circle: Circle
    sample_count: int
    convolution_peak_angle_rad: float
    convolution_peak_angle_deg: float
    field_entropy: EntropyReport
    convolved_entropy: EntropyReport
    phase_stats: CircularStats
    phase_entropy_report: EntropyReport
    dominant_fourier_modes: list[tuple[int, float, float]]
    winding_number: float


def analyze_circle_system(sample_count: int = 512) -> CircleConvolutionReport:
    """
    Build and analyze a complete circle system:

        1. Unit circle
        2. Two circular fields
        3. Circular convolution
        4. Fourier modes
        5. Circular phase statistics
        6. Entropy
        7. Winding number
    """
    circle = Circle(radius=1.0)
    grid = CircularGrid(sample_count)
    theta = grid.theta

    # Two fields living on a circle.
    # Think of them as phase probability fields or wrapped kernels.
    f = von_mises_like_bump(theta, center_rad=radians(40.0), concentration=7.0)
    g = von_mises_like_bump(theta, center_rad=radians(95.0), concentration=10.0)

    f = normalize_density_on_circle(f, grid.delta_theta)
    g = normalize_density_on_circle(g, grid.delta_theta)

    h = circular_convolution_fft(f, g, delta_theta=grid.delta_theta)
    h = np.maximum(h.real, 0.0)
    h = normalize_density_on_circle(h, grid.delta_theta)

    peak_index = int(np.argmax(h))
    peak_angle = float(theta[peak_index])

    series = fourier_series(h)
    dom = dominant_modes(series, count=7, ignore_zero=False)

    # Phase sample from density h: deterministic quantile-like sampling.
    probabilities = h * grid.delta_theta
    probabilities = probabilities / probabilities.sum()

    rng = np.random.default_rng(123)
    phase_samples = rng.choice(theta, size=4096, replace=True, p=probabilities)

    stats = circular_stats(phase_samples)
    phase_h = phase_entropy(phase_samples, bins=64)

    # Winding example: a curve that goes around the origin twice.
    path = phasor(2.0 * theta)
    wind = winding_number_complex_path(path)

    return CircleConvolutionReport(
        circle=circle,
        sample_count=sample_count,
        convolution_peak_angle_rad=peak_angle,
        convolution_peak_angle_deg=float(degrees(peak_angle)),
        field_entropy=density_entropy_on_circle(f, grid.delta_theta),
        convolved_entropy=density_entropy_on_circle(h, grid.delta_theta),
        phase_stats=stats,
        phase_entropy_report=phase_h,
        dominant_fourier_modes=dom,
        winding_number=wind,
    )


def print_report(report: CircleConvolutionReport) -> None:
    print("\nCIRCLE / PHASE / CONVOLUTION / ENTROPY REPORT")
    print("=" * 58)

    print("\nGEOMETRY")
    print("-" * 58)
    print(f"radius:                         {report.circle.radius:.6f}")
    print(f"circumference 2πr:              {report.circle.circumference:.6f}")
    print(f"area πr²:                       {report.circle.area:.6f}")
    print(f"curvature 1/r:                  {report.circle.curvature:.6f}")

    print("\nCIRCULAR CONVOLUTION")
    print("-" * 58)
    print(f"samples around circle:          {report.sample_count}")
    print(f"convolution peak angle:         {report.convolution_peak_angle_rad:.6f} rad")
    print(f"convolution peak angle:         {report.convolution_peak_angle_deg:.6f} degrees")

    print("\nENTROPY")
    print("-" * 58)
    print(f"original field entropy:         {report.field_entropy.entropy_nats:.6f} nats")
    print(f"convolved field entropy:        {report.convolved_entropy.entropy_nats:.6f} nats")
    print(f"phase entropy:                  {report.phase_entropy_report.entropy_nats:.6f} nats")
    print(f"normalized phase entropy:       {report.phase_entropy_report.normalized_entropy:.6f}")

    print("\nPHASE COALESCENCE")
    print("-" * 58)
    print(f"mean phase:                     {report.phase_stats.mean_angle_rad:.6f} rad")
    print(f"mean phase:                     {report.phase_stats.mean_angle_deg:.6f} degrees")
    print(f"order parameter R:              {report.phase_stats.resultant_length:.6f}")
    print(f"circular variance 1-R:          {report.phase_stats.circular_variance:.6f}")
    print(f"circular std:                   {report.phase_stats.circular_std_rad:.6f} rad")

    print("\nFOURIER MODES")
    print("-" * 58)
    print("mode      amplitude        phase(rad)")
    for mode, amp, phase in report.dominant_fourier_modes:
        print(f"{mode:>4d}      {amp:>10.6e}     {phase:>10.6f}")

    print("\nTOPOLOGY")
    print("-" * 58)
    print(f"example winding number:         {report.winding_number:.6f}")


def write_demo_csv(path: str | Path, *, sample_count: int = 512) -> None:
    """
    Export the demonstration fields as CSV.

    Columns:
        theta_rad, theta_deg, f, g, convolution
    """
    grid = CircularGrid(sample_count)
    theta = grid.theta

    f = von_mises_like_bump(theta, center_rad=radians(40.0), concentration=7.0)
    g = von_mises_like_bump(theta, center_rad=radians(95.0), concentration=10.0)

    f = normalize_density_on_circle(f, grid.delta_theta)
    g = normalize_density_on_circle(g, grid.delta_theta)

    h = circular_convolution_fft(f, g, delta_theta=grid.delta_theta)
    h = np.maximum(h.real, 0.0)
    h = normalize_density_on_circle(h, grid.delta_theta)

    with Path(path).open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(["theta_rad", "theta_deg", "field_f", "field_g", "circular_convolution"])
        for th, fv, gv, hv in zip(theta, f, g, h):
            writer.writerow([float(th), float(degrees(th)), float(fv), float(gv), float(hv)])


# ---------------------------------------------------------------------
# Self-tests
# ---------------------------------------------------------------------

def _assert_close(a: float, b: float, *, tol: float = 1e-9) -> None:
    if abs(a - b) > tol:
        raise AssertionError(f"{a} != {b} within tolerance {tol}")


def run_self_tests() -> None:
    # Radian and degree conversion.
    _assert_close(float(radians(180.0)), pi)
    _assert_close(float(degrees(pi)), 180.0)

    # Wrap.
    _assert_close(float(wrap_angle(3.0 * pi)), -pi)
    _assert_close(float(wrap_angle_positive(3.0 * pi)), pi)

    # Circle geometry.
    c = Circle(radius=2.0)
    _assert_close(c.circumference, 4.0 * pi)
    _assert_close(c.area, 4.0 * pi)
    _assert_close(c.curvature, 0.5)
    _assert_close(c.angle_from_arc_length(c.arc_length(1.25)), 1.25)

    x, y = c.point(pi / 2.0)
    _assert_close(float(x), 0.0, tol=1e-12)
    _assert_close(float(y), 2.0, tol=1e-12)

    assert c.classify_point(0.0, 0.0) == Region.INSIDE
    assert c.classify_point(2.0, 0.0) == Region.BOUNDARY
    assert c.classify_point(3.0, 0.0) == Region.OUTSIDE

    # Phasor magnitude.
    z = phasor(np.linspace(0.0, TAU, 128, endpoint=False))
    if not np.allclose(np.abs(z), 1.0):
        raise AssertionError("phasor magnitude is not 1")

    # Circular statistics.
    stats = circular_stats(np.zeros(32))
    _assert_close(stats.resultant_length, 1.0)

    spread = np.linspace(0.0, TAU, 256, endpoint=False)
    spread_stats = circular_stats(spread)
    if spread_stats.resultant_length > 1e-12:
        raise AssertionError("uniform phases should have near-zero resultant length")

    # Entropy.
    locked = entropy_from_probabilities(np.array([1.0, 0.0, 0.0, 0.0]))
    _assert_close(locked.normalized_entropy, 0.0)

    uniform = entropy_from_probabilities(np.array([1.0, 1.0, 1.0, 1.0]))
    _assert_close(uniform.normalized_entropy, 1.0)

    # Circular convolution direct vs FFT.
    f = np.array([1.0, 2.0, 3.0, 4.0])
    g = np.array([5.0, 6.0, 7.0, 8.0])
    hd = circular_convolution_direct(f, g)
    hf = circular_convolution_fft(f, g)
    if not np.allclose(hd, hf):
        raise AssertionError("direct and FFT circular convolution disagree")

    # Fourier reconstruction.
    grid = CircularGrid(128)
    values = 2.0 + 0.5 * np.cos(3.0 * grid.theta) + 0.25 * np.sin(5.0 * grid.theta)
    series = fourier_series(values)
    recon = reconstruct_from_fourier(series)
    if not np.allclose(recon.real, values):
        raise AssertionError("Fourier reconstruction failed")

    # Dominant modes should include zero for the constant part.
    modes = dominant_modes(series, count=3, ignore_zero=False)
    if modes[0][0] != 0:
        raise AssertionError("dominant mode should be 0 for signal with large DC component")

    # Winding number.
    path1 = phasor(grid.theta)
    path2 = phasor(2.0 * grid.theta)
    _assert_close(winding_number_complex_path(path1), 1.0)
    _assert_close(winding_number_complex_path(path2), 2.0)

    # Torus.
    torus = torus_knot(p=2, q=3, sample_count=128)
    assert torus.x.shape == (128,)
    assert torus.y.shape == (128,)
    assert torus.z.shape == (128,)

    # Full report smoke test.
    report = analyze_circle_system(sample_count=128)
    if not (0.0 <= report.phase_stats.resultant_length <= 1.0):
        raise AssertionError("invalid order parameter")
    if not (0.0 <= report.phase_entropy_report.normalized_entropy <= 1.0):
        raise AssertionError("invalid normalized entropy")

    print("All self-tests passed.")


# ---------------------------------------------------------------------
# Command line interface
# ---------------------------------------------------------------------

def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Circle, phase, circular convolution, Fourier, entropy, and winding-number lab."
    )

    sub = parser.add_subparsers(dest="command", required=True)

    demo = sub.add_parser("demo", help="Run the circle-convolution demonstration.")
    demo.add_argument("--samples", type=int, default=512, help="Number of samples around the circle.")
    demo.add_argument("--csv", type=str, default=None, help="Optional CSV export path.")

    sub.add_parser("selftest", help="Run built-in tests.")

    torus_cmd = sub.add_parser("torus", help="Generate torus knot CSV.")
    torus_cmd.add_argument("--p", type=int, default=2, help="First winding count.")
    torus_cmd.add_argument("--q", type=int, default=3, help="Second winding count.")
    torus_cmd.add_argument("--samples", type=int, default=1024, help="Number of samples.")
    torus_cmd.add_argument("--csv", type=str, required=True, help="Output CSV path.")

    return parser


def run_torus_export(args: argparse.Namespace) -> None:
    cloud = torus_knot(p=args.p, q=args.q, sample_count=args.samples)

    with Path(args.csv).open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(["u_rad", "v_rad", "x", "y", "z"])
        for u, v, x, y, z in zip(cloud.u, cloud.v, cloud.x, cloud.y, cloud.z):
            writer.writerow([float(u), float(v), float(x), float(y), float(z)])

    print(f"Wrote torus knot CSV to: {args.csv}")


def main() -> None:
    parser = build_arg_parser()
    args = parser.parse_args()

    if args.command == "selftest":
        run_self_tests()
        return

    if args.command == "demo":
        report = analyze_circle_system(sample_count=args.samples)
        print_report(report)

        if args.csv is not None:
            write_demo_csv(args.csv, sample_count=args.samples)
            print(f"\nWrote demo CSV to: {args.csv}")

        return

    if args.command == "torus":
        run_torus_export(args)
        return

    raise RuntimeError(f"Unknown command: {args.command}")


if __name__ == "__main__":
    main()
