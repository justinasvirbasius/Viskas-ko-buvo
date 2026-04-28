#ifndef SDF_PACKAGE_LAYOUT_H
#define SDF_PACKAGE_LAYOUT_H

#include <stdint.h>

typedef struct SdfpackV3Header {
    uint32_t nx, ny, nz;
    float bounds_min[3];
    float bounds_max[3];
    float spacing[3];
    uint32_t chunk_size[3];
    uint32_t chunk_grid[3];
} SdfpackV3Header;

typedef struct SdfpackV3RuntimeView {
    const float* sdf;
    const uint8_t* occupancy;
    const int16_t* material;
    const float* normal_xyz;
    const float* entropy;
    const uint8_t* lod;
    const float* rechts;
    const float* value_pressure;
} SdfpackV3RuntimeView;

#endif
