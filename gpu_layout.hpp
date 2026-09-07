#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ein {

// These structures mirror the std430 records used by the classification shaders.
// Keep them POD and update the static assertions whenever GLSL layouts change.
struct alignas(16) GpuTileRecord {
    std::array<std::uint32_t, 4> location{};
    std::array<std::uint32_t, 4> proof{};
    std::array<float, 4> quality{};
    std::array<float, 4> cost_a{};
    std::array<float, 4> cost_b{};
};

struct alignas(16) GpuMassiveUnitRecord {
    std::array<std::uint32_t, 4> location{};
    std::array<std::uint32_t, 4> class_counts{};
    std::array<std::uint32_t, 4> pressure{};
    std::array<float, 4> quality{};
};

struct alignas(16) GpuQueueHeader {
    std::uint32_t count{};
    std::uint32_t overflow{};
    std::array<std::uint32_t, 2> padding{};
};

struct alignas(16) GpuTileTask {
    std::uint32_t tile_x{};
    std::uint32_t tile_y{};
    std::uint32_t record_index{};
    std::uint32_t dominant_cost{};
};

struct alignas(16) GpuDispatchCommand {
    std::uint32_t groups_x{};
    std::uint32_t groups_y{1};
    std::uint32_t groups_z{1};
    std::uint32_t padding{};
};

struct alignas(16) GpuClassificationSummary {
    std::uint32_t total_tiles{};
    std::array<std::uint32_t, 4> class_counts{};
    std::uint32_t total_pixels{};
    std::uint32_t native_equivalent_pixels{};
    std::uint32_t full_required{};
    std::uint32_t saturated_units{};
    std::uint32_t rebalanced_tiles{};
    std::array<std::uint32_t, 6> cost_totals{};
    std::uint32_t hot_lane{};
    std::uint32_t hot_cost{};
    std::array<std::uint32_t, 2> tail_padding{};
};

struct alignas(16) GpuClassificationConfig {
    std::array<std::uint32_t, 2> output_extent{};
    std::array<std::uint32_t, 2> tile_grid_extent{};
    std::uint32_t seed_generation{};
    std::uint32_t history_seed_generation{};
    std::uint32_t force_full{};
    std::uint32_t padding0{};
    float relay_minimum_confidence{};
    float relay_maximum_depth_error{};
    float selective_minimum_confidence{};
    float selective_maximum_depth_error{};
    std::uint32_t selective_maximum_refine_pixels{};
    std::uint32_t selective_maximum_identity_mismatches{};
    std::array<std::uint32_t, 2> padding1{};
};

struct alignas(16) GpuMassiveConfig {
    std::array<std::uint32_t, 2> tile_grid_extent{};
    std::array<std::uint32_t, 2> massive_grid_extent{};
    std::array<std::uint32_t, 4> queue_capacity{};
    float unit_saturation_ratio{};
    float padding0{};
    std::array<std::uint32_t, 2> padding1{};
};

struct alignas(16) GpuDispatchConfig {
    std::array<std::uint32_t, 4> queue_capacity{};
    std::uint32_t threads_per_group{};
    float frame_full_native_equivalent_ratio{};
    std::array<std::uint32_t, 2> padding{};
};

static_assert(std::is_standard_layout_v<GpuTileRecord>);
static_assert(sizeof(GpuTileRecord) == 80);
static_assert(sizeof(GpuMassiveUnitRecord) == 64);
static_assert(sizeof(GpuQueueHeader) == 16);
static_assert(sizeof(GpuTileTask) == 16);
static_assert(sizeof(GpuDispatchCommand) == 16);
static_assert(sizeof(GpuClassificationSummary) == 80);
static_assert(sizeof(GpuClassificationConfig) == 64);
static_assert(sizeof(GpuMassiveConfig) == 48);
static_assert(sizeof(GpuDispatchConfig) == 32);

} // namespace ein
