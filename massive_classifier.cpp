#include "ein/massive_classifier.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_set>

namespace ein {

namespace {
constexpr std::size_t lane(ResolutionClass value) noexcept {
    return static_cast<std::size_t>(value);
}

constexpr std::size_t cost_lane(CostClass value) noexcept {
    return static_cast<std::size_t>(value);
}

bool finite_tile(const TileEvidence& tile) {
    if (!std::isfinite(tile.minimum_confidence) ||
        !std::isfinite(tile.maximum_depth_error) ||
        !std::isfinite(tile.maximum_motion_pixels) ||
        !std::isfinite(tile.variance) ||
        !std::isfinite(tile.mean_luma_gradient)) return false;
    return std::all_of(tile.cost_pressure.begin(), tile.cost_pressure.end(),
                       [](float value) { return std::isfinite(value) && value >= 0.0F; });
}

bool structurally_valid(const TileEvidence& tile) {
    return tile.pixel_count > 0 && tile.pixel_count <= pixels_per_classification_tile &&
           tile.refine_pixels <= tile.pixel_count &&
           tile.identity_mismatch_pixels <= tile.pixel_count &&
           tile.invalid_samples <= tile.pixel_count &&
           tile.minimum_confidence >= 0.0F && tile.minimum_confidence <= 1.0F &&
           tile.maximum_depth_error >= 0.0F && tile.maximum_motion_pixels >= 0.0F &&
           tile.variance >= 0.0F && tile.mean_luma_gradient >= 0.0F;
}

ResolutionClass maximum_resolution(const std::array<std::uint32_t, resolution_class_count>& counts) {
    for (std::size_t i = counts.size(); i-- > 0;)
        if (counts[i] != 0) return static_cast<ResolutionClass>(i);
    return ResolutionClass::Relay;
}
} // namespace

MassiveClassifier::MassiveClassifier(ClassificationThresholds thresholds)
    : thresholds_(thresholds) {
    const auto unit_interval = [](float value) {
        return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
    };
    if (!unit_interval(thresholds.relay_minimum_confidence) ||
        !unit_interval(thresholds.selective_minimum_confidence) ||
        !unit_interval(thresholds.unit_saturation_ratio) ||
        !unit_interval(thresholds.frame_full_native_equivalent_ratio))
        throw std::invalid_argument("classification ratios must be within [0, 1]");
    if (!std::isfinite(thresholds.relay_maximum_depth_error) ||
        !std::isfinite(thresholds.selective_maximum_depth_error) ||
        thresholds.relay_maximum_depth_error < 0.0F ||
        thresholds.selective_maximum_depth_error < thresholds.relay_maximum_depth_error)
        throw std::invalid_argument("classification depth thresholds are invalid");
    if (thresholds.selective_maximum_refine_pixels > pixels_per_classification_tile ||
        thresholds.selective_maximum_identity_mismatches > pixels_per_classification_tile)
        throw std::invalid_argument("selective thresholds exceed a 16x16 tile");
    if (thresholds.selective_queue_capacity == 0 ||
        thresholds.queue_threads_per_group == 0)
        throw std::invalid_argument("classification queue capacities must be non-zero");
}

ResolutionClass MassiveClassifier::classify_tile(const MassiveUnitEvidence& unit,
                                                 const TileEvidence& tile) const {
    if (unit.seed_generation == 0 ||
        unit.seed_generation != unit.history_seed_generation ||
        !finite_tile(tile) || !structurally_valid(tile) || tile.invalid_samples != 0)
        return ResolutionClass::Full;

    const bool relay = tile.refine_pixels == 0 &&
                       tile.identity_mismatch_pixels == 0 &&
                       tile.minimum_confidence >= thresholds_.relay_minimum_confidence &&
                       tile.maximum_depth_error <= thresholds_.relay_maximum_depth_error;
    if (relay) return ResolutionClass::Relay;

    const bool selective =
        tile.refine_pixels <= thresholds_.selective_maximum_refine_pixels &&
        tile.identity_mismatch_pixels <= thresholds_.selective_maximum_identity_mismatches &&
        tile.minimum_confidence >= thresholds_.selective_minimum_confidence &&
        tile.maximum_depth_error <= thresholds_.selective_maximum_depth_error;
    return selective ? ResolutionClass::SelectiveRefine : ResolutionClass::Native;
}

CostClass MassiveClassifier::dominant_cost(const TileEvidence& tile) {
    const auto found = std::max_element(tile.cost_pressure.begin(), tile.cost_pressure.end());
    return static_cast<CostClass>(std::distance(tile.cost_pressure.begin(), found));
}

ClassificationBatch MassiveClassifier::classify(
    std::span<const MassiveUnitEvidence> evidence) const {
    ClassificationBatch result;
    std::array<double, cost_class_count> cost_totals{};
    std::unordered_set<std::uint64_t> unit_coordinates;

    for (std::size_t unit_index = 0; unit_index < evidence.size(); ++unit_index) {
        const auto& input = evidence[unit_index];
        if (input.tiles.empty() || input.tiles.size() >
            massive_unit_tile_extent * massive_unit_tile_extent)
            throw std::invalid_argument("a massive unit must contain 1..64 leaf tiles");
        const auto unit_key = (static_cast<std::uint64_t>(input.unit_y) << 32u) | input.unit_x;
        if (!unit_coordinates.insert(unit_key).second)
            throw std::invalid_argument("duplicate massive unit coordinates");

        MassiveClassificationUnit unit;
        unit.unit_x = input.unit_x;
        unit.unit_y = input.unit_y;
        unit.seed_generation = input.seed_generation;
        std::array<double, cost_class_count> unit_costs{};
        std::uint64_t occupied_tiles{};

        for (std::size_t tile_index = 0; tile_index < input.tiles.size(); ++tile_index) {
            const auto& tile = input.tiles[tile_index];
            const auto first_x = static_cast<std::uint64_t>(input.unit_x) * massive_unit_tile_extent;
            const auto first_y = static_cast<std::uint64_t>(input.unit_y) * massive_unit_tile_extent;
            if (tile.tile_x < first_x || tile.tile_x >= first_x + massive_unit_tile_extent ||
                tile.tile_y < first_y || tile.tile_y >= first_y + massive_unit_tile_extent)
                throw std::invalid_argument("leaf tile lies outside its massive unit");
            const auto local_x = static_cast<std::uint64_t>(tile.tile_x) - first_x;
            const auto local_y = static_cast<std::uint64_t>(tile.tile_y) - first_y;
            const auto tile_bit = std::uint64_t{1} <<
                (local_y * massive_unit_tile_extent + local_x);
            if ((occupied_tiles & tile_bit) != 0)
                throw std::invalid_argument("duplicate leaf tile coordinates");
            occupied_tiles |= tile_bit;
            const auto classification = classify_tile(input, tile);
            const auto cost = finite_tile(tile) ? dominant_cost(tile) : CostClass::Post;
            const auto record_index = static_cast<std::uint32_t>(result.tiles.size());
            result.tiles.push_back({
                static_cast<std::uint32_t>(unit_index),
                static_cast<std::uint32_t>(tile_index),
                tile.tile_x, tile.tile_y, tile.pixel_count, tile.refine_pixels,
                classification, cost, tile.minimum_confidence, tile.maximum_depth_error,
            });
            result.queues[lane(classification)].push_back(record_index);
            ++unit.tile_counts[lane(classification)];
            unit.pixel_count += tile.pixel_count;
            unit.minimum_confidence = std::min(unit.minimum_confidence,
                                               std::clamp(tile.minimum_confidence, 0.0F, 1.0F));
            for (std::size_t i = 0; i < cost_class_count; ++i) {
                const auto pressure = finite_tile(tile) ? tile.cost_pressure[i] : 0.0F;
                unit_costs[i] += pressure;
                cost_totals[i] += pressure;
            }
        }
        unit.dominant_cost = static_cast<CostClass>(std::distance(
            unit_costs.begin(), std::max_element(unit_costs.begin(), unit_costs.end())));
        result.units.push_back(unit);
    }

    // If the sparse lane is over capacity, promote its riskiest tiles to full native tiles.
    auto& selective = result.queues[lane(ResolutionClass::SelectiveRefine)];
    auto& native = result.queues[lane(ResolutionClass::Native)];
    if (selective.size() > thresholds_.selective_queue_capacity) {
        std::sort(selective.begin(), selective.end(), [&](std::uint32_t a, std::uint32_t b) {
            const auto& left = result.tiles[a];
            const auto& right = result.tiles[b];
            if (left.minimum_confidence != right.minimum_confidence)
                return left.minimum_confidence < right.minimum_confidence;
            return left.maximum_depth_error > right.maximum_depth_error;
        });
        const auto overflow = selective.size() - thresholds_.selective_queue_capacity;
        for (std::size_t i = 0; i < overflow; ++i) {
            auto& tile = result.tiles[selective[i]];
            auto& unit = result.units[tile.unit_index];
            --unit.tile_counts[lane(ResolutionClass::SelectiveRefine)];
            ++unit.tile_counts[lane(ResolutionClass::Native)];
            tile.resolution = ResolutionClass::Native;
            native.push_back(selective[i]);
        }
        result.rebalanced_tiles = static_cast<std::uint32_t>(overflow);
        selective.erase(selective.begin(), selective.begin() + static_cast<std::ptrdiff_t>(overflow));
    }

    for (auto& unit : result.units) {
        unit.resolution = maximum_resolution(unit.tile_counts);
        const auto non_relay = unit.tile_counts[lane(ResolutionClass::SelectiveRefine)] +
                               unit.tile_counts[lane(ResolutionClass::Native)] +
                               unit.tile_counts[lane(ResolutionClass::Full)];
        const auto tile_total = std::accumulate(unit.tile_counts.begin(), unit.tile_counts.end(), 0u);
        unit.saturated = static_cast<float>(non_relay) /
                         static_cast<float>(tile_total) >= thresholds_.unit_saturation_ratio;
    }

    for (const auto& tile : result.tiles) {
        result.total_pixels += tile.pixel_count;
        switch (tile.resolution) {
            case ResolutionClass::Relay: break;
            case ResolutionClass::SelectiveRefine:
                result.native_equivalent_pixels += tile.refine_pixels;
                break;
            case ResolutionClass::Native:
            case ResolutionClass::Full:
                result.native_equivalent_pixels += tile.pixel_count;
                break;
        }
    }
    for (const auto& tile : result.tiles) {
        auto& unit = result.units[tile.unit_index];
        unit.native_equivalent_pixels += tile.resolution == ResolutionClass::SelectiveRefine
            ? tile.refine_pixels
            : tile.resolution >= ResolutionClass::Native ? tile.pixel_count : 0u;
    }

    const bool has_full_tiles = !result.queues[lane(ResolutionClass::Full)].empty();
    const double native_ratio = result.total_pixels == 0 ? 1.0 :
        static_cast<double>(result.native_equivalent_pixels) /
        static_cast<double>(result.total_pixels);
    result.full_required = has_full_tiles ||
        native_ratio >= thresholds_.frame_full_native_equivalent_ratio;

    result.hot_lane = ResolutionClass::Relay;
    for (std::size_t i = 1; i < result.queues.size(); ++i)
        if (result.queues[i].size() > result.queues[lane(result.hot_lane)].size())
            result.hot_lane = static_cast<ResolutionClass>(i);
    result.hot_cost = static_cast<CostClass>(std::distance(
        cost_totals.begin(), std::max_element(cost_totals.begin(), cost_totals.end())));

    for (std::size_t i = 0; i < result.queues.size(); ++i) {
        const auto count = static_cast<std::uint32_t>(result.queues[i].size());
        result.dispatch[i].groups_x =
            (count + thresholds_.queue_threads_per_group - 1u) /
            thresholds_.queue_threads_per_group;
    }
    return result;
}

std::string_view to_string(ResolutionClass value) noexcept {
    switch (value) {
        case ResolutionClass::Relay: return "relay";
        case ResolutionClass::SelectiveRefine: return "selective-refine";
        case ResolutionClass::Native: return "native";
        case ResolutionClass::Full: return "full";
    }
    return "unknown";
}

std::string_view to_string(CostClass value) noexcept {
    switch (value) {
        case CostClass::Alu: return "alu";
        case CostClass::Memory: return "memory";
        case CostClass::Branch: return "branch";
        case CostClass::Texture: return "texture";
        case CostClass::Overdraw: return "overdraw";
        case CostClass::Post: return "post";
    }
    return "unknown";
}

} // namespace ein
