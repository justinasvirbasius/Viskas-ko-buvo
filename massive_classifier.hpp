#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ein {

inline constexpr std::uint32_t classification_tile_extent = 16;
inline constexpr std::uint32_t massive_unit_tile_extent = 8;
inline constexpr std::uint32_t massive_unit_pixel_extent =
    classification_tile_extent * massive_unit_tile_extent;
inline constexpr std::uint32_t pixels_per_classification_tile =
    classification_tile_extent * classification_tile_extent;

enum class ResolutionClass : std::uint8_t {
    Relay = 0,
    SelectiveRefine = 1,
    Native = 2,
    Full = 3,
};

enum class CostClass : std::uint8_t {
    Alu = 0,
    Memory = 1,
    Branch = 2,
    Texture = 3,
    Overdraw = 4,
    Post = 5,
};

inline constexpr std::size_t resolution_class_count = 4;
inline constexpr std::size_t cost_class_count = 6;

struct TileEvidence {
    std::uint32_t tile_x{};
    std::uint32_t tile_y{};
    std::uint32_t pixel_count{pixels_per_classification_tile};
    std::uint32_t refine_pixels{};
    std::uint32_t identity_mismatch_pixels{};
    std::uint32_t invalid_samples{};
    float minimum_confidence{1.0F};
    float maximum_depth_error{};
    float maximum_motion_pixels{};
    float variance{};
    float mean_luma_gradient{};
    std::array<float, cost_class_count> cost_pressure{};
};

struct MassiveUnitEvidence {
    std::uint32_t unit_x{};
    std::uint32_t unit_y{};
    std::uint32_t seed_generation{};
    std::uint32_t history_seed_generation{};
    std::vector<TileEvidence> tiles;
};

struct ClassificationThresholds {
    float relay_minimum_confidence{0.985F};
    float relay_maximum_depth_error{0.0025F};
    float selective_minimum_confidence{0.35F};
    float selective_maximum_depth_error{0.05F};
    std::uint32_t selective_maximum_refine_pixels{160};
    std::uint32_t selective_maximum_identity_mismatches{64};
    float unit_saturation_ratio{0.75F};
    float frame_full_native_equivalent_ratio{0.72F};
    std::uint32_t selective_queue_capacity{4096};
    std::uint32_t queue_threads_per_group{64};
};

struct ClassifiedTile {
    std::uint32_t unit_index{};
    std::uint32_t tile_index{};
    std::uint32_t tile_x{};
    std::uint32_t tile_y{};
    std::uint32_t pixel_count{};
    std::uint32_t refine_pixels{};
    ResolutionClass resolution{ResolutionClass::Full};
    CostClass dominant_cost{CostClass::Alu};
    float minimum_confidence{};
    float maximum_depth_error{};
};

struct MassiveClassificationUnit {
    std::uint32_t unit_x{};
    std::uint32_t unit_y{};
    std::uint32_t seed_generation{};
    ResolutionClass resolution{ResolutionClass::Full};
    CostClass dominant_cost{CostClass::Alu};
    std::array<std::uint32_t, resolution_class_count> tile_counts{};
    std::uint32_t pixel_count{};
    std::uint32_t native_equivalent_pixels{};
    float minimum_confidence{1.0F};
    bool saturated{};
};

struct DispatchArgs {
    std::uint32_t groups_x{};
    std::uint32_t groups_y{1};
    std::uint32_t groups_z{1};
};

struct ClassificationBatch {
    std::vector<MassiveClassificationUnit> units;
    std::vector<ClassifiedTile> tiles;
    // Entries index `tiles`; lanes are Relay, SelectiveRefine, Native, Full.
    std::array<std::vector<std::uint32_t>, resolution_class_count> queues;
    std::array<DispatchArgs, resolution_class_count> dispatch;
    ResolutionClass hot_lane{ResolutionClass::Relay};
    CostClass hot_cost{CostClass::Alu};
    std::uint32_t rebalanced_tiles{};
    std::uint64_t total_pixels{};
    std::uint64_t native_equivalent_pixels{};
    bool full_required{};
};

class MassiveClassifier {
public:
    explicit MassiveClassifier(ClassificationThresholds thresholds = {});

    [[nodiscard]] ClassificationBatch classify(
        std::span<const MassiveUnitEvidence> evidence) const;
    [[nodiscard]] const ClassificationThresholds& thresholds() const noexcept {
        return thresholds_;
    }

private:
    [[nodiscard]] ResolutionClass classify_tile(const MassiveUnitEvidence& unit,
                                                const TileEvidence& tile) const;
    static CostClass dominant_cost(const TileEvidence& tile);

    ClassificationThresholds thresholds_;
};

[[nodiscard]] std::string_view to_string(ResolutionClass value) noexcept;
[[nodiscard]] std::string_view to_string(CostClass value) noexcept;

} // namespace ein
