# SDF Prepackager Rechts v3

A stricter, more complete SDF/game-mesh prepackager.

This version is built around a hard principle:

> A package should not quietly pretend to be solid.  
> It must build, validate, report, and either pass or fail honestly.

## What v3 adds

### 1. Smoke press value system

The package includes a real pressure/smoke validation route:

```bash
python -m sdfpack.cli press --out out_press --resolution 28 28 28
```

It produces:

```text
smoke_press_value.json
```

The smoke press checks:

- meta-map creation
- mesh creation
- closed manifold status
- orientation
- non-manifold edges
- boundary edges
- package value score
- rechts solidity score
- pressure verdict

### 2. Proper meta maps

The `.npz` package contains:

| Layer | Type | Purpose |
|---|---:|---|
| `sdf_raw` | float32 `[nx,ny,nz]` | raw field |
| `sdf` | float32 `[nx,ny,nz]` | convolved/packaged field |
| `occupancy` | uint8 `[nx,ny,nz]` | inside/outside |
| `material` | int16 `[nx,ny,nz]` | material id |
| `gradient` | float32 `[nx,ny,nz,3]` | SDF vector derivative |
| `normal` | float32 `[nx,ny,nz,3]` | normalized gradient |
| `curvature` | float32 `[nx,ny,nz]` | Laplacian pressure |
| `surface` | float32 `[nx,ny,nz]` | surface band |
| `entropy` | float32 `[nx,ny,nz]` | geometry complexity |
| `lod` | uint8 `[nx,ny,nz]` | LOD tier 0..3 |
| `rechts` | float32 `[nx,ny,nz]` | local solidity confidence |
| `value_pressure` | float32 `[nx,ny,nz]` | stream/build importance |
| `moat` | float32 `[nx,ny,nz]` | normalized distance moat around surface |
| `band_index` | uint8 `[nx,ny,nz]` | distance band class |
| `chunk_id` | int32 `[nx,ny,nz]` | stable chunk address |
| `chunk_score` | float32 `[cx,cy,cz]` | chunk streaming priority |
| `chunk_bounds` | float32 `[cx,cy,cz,6]` | chunk world bounds |
| `seam` | uint8 `[nx,ny,nz]` | chunk seam mask |
| `mip_sdf_1` | float32 | downsampled SDF |
| `mip_entropy_1` | float32 | downsampled entropy |
| `mip_sdf_2` | float32 | second downsampled SDF |
| `mip_entropy_2` | float32 | second downsampled entropy |

### 3. Stricter mesh solidity

The mesh path includes:

- marching tetrahedra extraction
- vertex welding
- degenerate triangle removal
- consistent winding
- positive-volume orientation
- boundary-loop filling
- validation report
- strict gate: fail if not closed/manifold unless explicitly relaxed

### 4. Standard exports

The build exports:

- `package.npz`
- `mesh.obj`
- `mesh.ply`
- `mesh.stl`
- `scene.json`
- `manifest.json`
- `validation.json`
- `smoke_press_value.json`
- `runtime_cpp/sdf_package_layout.hpp`
- `runtime_c/sdf_package_layout.h`

## Install

```bash
cd sdf_prepackager_rechts_v3
python -m pip install -r requirements.txt
```

## Demo build

```bash
python -m sdfpack.cli demo --out out_demo --profile game --resolution 40 40 40
```

## Smoke press

```bash
python -m sdfpack.cli press --out out_press --resolution 28 28 28
```

## Build from JSON scene

```bash
python -m sdfpack.cli build examples/rechts_scene.json --out out_scene --profile game --mesh
```

## Profiles

- `debug`
- `game`
- `collision`
- `high`

## Important honesty

This package does not guarantee that every arbitrary SDF scene is clean.  
It validates the produced mesh and reports whether it is closed, manifold, and oriented.  
By default, the manager uses a strict gate for demo/export workflows.
