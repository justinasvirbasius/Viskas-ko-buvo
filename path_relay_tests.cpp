#include "ein/graphics_shell.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

ein::PathProof full_proof(const ein::PathFramePlan& plan) {
    return {plan.seed_generation, true, true, false, 0, 0.0F};
}

ein::PathProof relay_proof(const ein::PathFramePlan& plan) {
    return {plan.seed_generation, true, true, true, 0, 1.0F / 1024.0F};
}

void test_thirteen_coats_are_ordered() {
    const auto& route = ein::FullPathRelay::coat_route();
    expect(route.size() == ein::coat_count, "coat route size changed");
    for (std::size_t i = 0; i < route.size(); ++i)
        expect(static_cast<std::size_t>(route[i].coat) == i,
               "coat route is no longer C0-C12 ordered");
}

void test_full_seed_precedes_relay() {
    ein::FullPathRelay relay(1920, 1080);
    const auto seed = relay.plan();
    expect(seed.mode == ein::PathFrameMode::FullSeed, "first frame was not FULL");
    expect(seed.input_width == 1920 && seed.input_height == 1080,
           "FULL seed was not native resolution");
    expect(relay.commit(seed, full_proof(seed)), "valid FULL proof was rejected");

    relay.observe_confidence(0.9F);
    const auto next = relay.plan();
    expect(next.mode == ein::PathFrameMode::Relay, "valid history did not relay");
    expect(next.input_width == 1120 && next.input_height == 632,
           "default relay extent changed unexpectedly");
    expect(next.output_width == 1920 && next.output_height == 1080,
           "relay output stopped being native");
    expect(relay.commit(next, relay_proof(next)), "valid relay proof was rejected");
}

void test_unresolved_work_fails_closed() {
    ein::FullPathRelay relay(1280, 720);
    const auto seed = relay.plan();
    expect(relay.commit(seed, full_proof(seed)), "seed setup failed");
    relay.observe_confidence(0.95F);
    const auto frame = relay.plan();
    auto proof = relay_proof(frame);
    proof.unresolved_tiles = 1;
    expect(!relay.commit(frame, proof), "unresolved tile was promoted to history");
    expect(relay.plan().mode == ein::PathFrameMode::FullSeed,
           "unresolved tile did not schedule FULL");
}

void test_identity_and_error_fail_closed() {
    ein::FullPathRelay relay(1280, 720);
    auto seed = relay.plan();
    expect(relay.commit(seed, full_proof(seed)), "seed setup failed");
    relay.observe_confidence(0.95F);
    auto frame = relay.plan();
    auto bad_identity = relay_proof(frame);
    bad_identity.identity_complete = false;
    expect(!relay.commit(frame, bad_identity), "identity mismatch was accepted");

    seed = relay.plan();
    expect(relay.commit(seed, full_proof(seed)), "identity recovery seed failed");
    relay.observe_confidence(0.95F);
    frame = relay.plan();
    auto bad_error = relay_proof(frame);
    bad_error.maximum_error = 0.02F;
    expect(!relay.commit(frame, bad_error), "excess preservation error was accepted");
}

void test_discontinuity_is_latched() {
    ein::FullPathRelay relay(960, 540);
    const auto seed = relay.plan();
    expect(relay.commit(seed, full_proof(seed)), "seed setup failed");
    relay.observe_confidence(1.0F);

    const auto cut = relay.plan(true, false);
    expect(cut.mode == ein::PathFrameMode::FullSeed && cut.reason == "camera cut",
           "camera cut did not schedule FULL");
    expect(relay.plan().mode == ein::PathFrameMode::FullSeed,
           "discarded cut plan incorrectly restored old history");
}

void test_refresh_interval() {
    ein::FullPathRelay relay(800, 600, ein::FullPathConfig{
        .internal_scale = 0.5F,
        .full_refresh_interval = 2,
        .minimum_confidence = 0.5F,
        .maximum_preservation_error = 1.0F / 255.0F,
        .workgroup_alignment = 8,
    });
    auto frame = relay.plan();
    expect(relay.commit(frame, full_proof(frame)), "seed setup failed");
    relay.observe_confidence(1.0F);
    for (int i = 0; i < 2; ++i) {
        frame = relay.plan();
        expect(frame.mode == ein::PathFrameMode::Relay, "relay ended before refresh interval");
        expect(relay.commit(frame, relay_proof(frame)), "interval relay proof failed");
    }
    frame = relay.plan();
    expect(frame.mode == ein::PathFrameMode::FullSeed &&
           frame.reason == "scheduled full refresh",
           "refresh interval did not schedule FULL");
}

} // namespace

int main() {
    try {
        test_thirteen_coats_are_ordered();
        test_full_seed_precedes_relay();
        test_unresolved_work_fails_closed();
        test_identity_and_error_fail_closed();
        test_discontinuity_is_latched();
        test_refresh_interval();
        std::cout << "all EIN Full-Path Relay tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failure: " << error.what() << '\n';
        return 1;
    }
}
