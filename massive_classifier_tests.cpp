#include "ein/massive_classifier.hpp"
#include "ein/gpu_layout.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

ein::TileEvidence safe_tile(std::uint32_t x, std::uint32_t y) {
    ein::TileEvidence tile;
    tile.tile_x = x;
    tile.tile_y = y;
    tile.minimum_confidence = 0.999F;
    tile.maximum_depth_error = 0.001F;
    tile.maximum_motion_pixels = 0.25F;
    tile.variance = 0.1F;
    tile.mean_luma_gradient = 0.08F;
    tile.cost_pressure = {0.3F, 0.2F, 0.1F, 0.8F, 0.05F, 0.2F};
    return tile;
}

ein::MassiveUnitEvidence safe_unit(std::uint32_t x = 0, std::uint32_t y = 0,
                                   std::uint32_t generation = 7) {
    ein::MassiveUnitEvidence unit;
    unit.unit_x = x;
    unit.unit_y = y;
    unit.seed_generation = generation;
    unit.history_seed_generation = generation;
    for (std::uint32_t ty = 0; ty < ein::massive_unit_tile_extent; ++ty)
        for (std::uint32_t tx = 0; tx < ein::massive_unit_tile_extent; ++tx)
            unit.tiles.push_back(safe_tile(x * ein::massive_unit_tile_extent + tx,
                                           y * ein::massive_unit_tile_extent + ty));
    return unit;
}

void test_massive_shape_and_relay_lane() {
    expect(ein::classification_tile_extent == 16, "leaf classification must remain 16x16");
    expect(ein::massive_unit_pixel_extent == 128, "massive unit must cover 128x128 pixels");
    expect(sizeof(ein::GpuTileRecord) == 80 && sizeof(ein::GpuMassiveUnitRecord) == 64,
           "CPU and GLSL record layouts diverged");
    const ein::MassiveClassifier classifier;
    const std::array input{safe_unit()};
    const auto batch = classifier.classify(input);
    expect(batch.units.size() == 1 && batch.tiles.size() == 64,
           "massive unit did not contain 64 leaf tiles");
    expect(batch.units[0].resolution == ein::ResolutionClass::Relay,
           "fully proven unit was not relayed");
    expect(batch.queues[0].size() == 64 && batch.dispatch[0].groups_x == 1,
           "relay queue or dispatch arguments are incorrect");
    expect(batch.hot_cost == ein::CostClass::Texture,
           "dominant cost lane was not aggregated");
    expect(!batch.full_required, "safe unit unexpectedly requested FULL");
}

void test_selective_and_native_routing() {
    auto unit = safe_unit();
    unit.tiles[3].refine_pixels = 24;
    unit.tiles[3].identity_mismatch_pixels = 8;
    unit.tiles[3].minimum_confidence = 0.8F;
    unit.tiles[3].maximum_depth_error = 0.01F;
    unit.tiles[11].refine_pixels = 220;
    unit.tiles[11].minimum_confidence = 0.2F;

    const ein::MassiveClassifier classifier;
    const std::array input{unit};
    const auto batch = classifier.classify(input);
    expect(batch.queues[1].size() == 1, "selective tile missed its queue");
    expect(batch.queues[2].size() == 1, "native tile missed its queue");
    expect(batch.units[0].resolution == ein::ResolutionClass::Native,
           "unit did not expose its highest required resolution");
    expect(!batch.full_required, "two refined tiles should not force FULL");
}

void test_generation_and_invalid_samples_force_full() {
    auto mismatch = safe_unit();
    mismatch.history_seed_generation = mismatch.seed_generation - 1;
    auto invalid = safe_unit(1, 0);
    invalid.tiles[0].invalid_samples = 1;

    const ein::MassiveClassifier classifier;
    const std::array input{mismatch, invalid};
    const auto batch = classifier.classify(input);
    expect(batch.queues[3].size() == 65, "FULL queue did not receive failed proofs");
    expect(batch.full_required, "failed unit proofs did not request FULL");
}

void test_native_pressure_escalates_frame() {
    auto unit = safe_unit();
    for (std::size_t i = 0; i < 48; ++i) {
        unit.tiles[i].refine_pixels = unit.tiles[i].pixel_count;
        unit.tiles[i].minimum_confidence = 0.1F;
    }
    const ein::MassiveClassifier classifier;
    const std::array input{unit};
    const auto batch = classifier.classify(input);
    expect(batch.native_equivalent_pixels * 100 >= batch.total_pixels * 72,
           "test did not create sufficient native pressure");
    expect(batch.full_required, "native-equivalent saturation did not escalate to FULL");
    expect(batch.units[0].saturated, "massive unit did not report saturation");
}

void test_sparse_queue_rebalances_safely() {
    ein::ClassificationThresholds thresholds;
    thresholds.selective_queue_capacity = 2;
    ein::MassiveClassifier classifier(thresholds);
    auto unit = safe_unit();
    for (std::size_t i = 0; i < 4; ++i) {
        unit.tiles[i].refine_pixels = 16;
        unit.tiles[i].minimum_confidence = 0.55F + static_cast<float>(i) * 0.1F;
        unit.tiles[i].maximum_depth_error = 0.01F;
    }
    const std::array input{unit};
    const auto batch = classifier.classify(input);
    expect(batch.rebalanced_tiles == 2, "sparse overflow was not rebalanced");
    expect(batch.queues[1].size() == 2 && batch.queues[2].size() == 2,
           "overflow did not promote to native work");
    expect(!batch.full_required, "small queue rebalance unnecessarily forced FULL");
}

void test_duplicate_tiles_are_rejected() {
    auto unit = safe_unit();
    unit.tiles[1].tile_x = unit.tiles[0].tile_x;
    unit.tiles[1].tile_y = unit.tiles[0].tile_y;
    const ein::MassiveClassifier classifier;
    const std::array input{unit};
    bool rejected = false;
    try {
        (void)classifier.classify(input);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    expect(rejected, "duplicate leaf tile was accepted");
}

} // namespace

int main() {
    try {
        test_massive_shape_and_relay_lane();
        test_selective_and_native_routing();
        test_generation_and_invalid_samples_force_full();
        test_native_pressure_escalates_frame();
        test_sparse_queue_rebalances_safely();
        test_duplicate_tiles_are_rejected();
        std::cout << "all massive classification unit tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failure: " << error.what() << '\n';
        return 1;
    }
}
