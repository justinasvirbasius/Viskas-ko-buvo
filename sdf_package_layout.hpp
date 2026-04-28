#pragma once
#include <cstdint>

/*
  sdfpack-rechts-v3 runtime layout notes.

  This is a small engine-side description header.
  The produced package.npz can be converted into an engine binary archive.

  Main arrays:
    sdf, occupancy, material, normal, entropy, lod, rechts, value_pressure
*/

struct SdfpackV3Header {
    uint32_t nx, ny, nz;
    float bounds_min[3];
    float bounds_max[3];
    float spacing[3];
    uint32_t chunk_size[3];
    uint32_t chunk_grid[3];
};

struct SdfpackV3RuntimeView {
    const float* sdf;
    const uint8_t* occupancy;
    const int16_t* material;
    const float* normal_xyz;
    const float* entropy;
    const uint8_t* lod;
    const float* rechts;
    const float* value_pressure;
};
