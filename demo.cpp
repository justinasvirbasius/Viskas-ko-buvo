#include "ein/graphics_shell.hpp"
#include "ein/massive_classifier.hpp"

#include <array>
#include <iomanip>
#include <iostream>
#include <vector>

int main() {
    using namespace ein;
    try {
        GraphicsAdministrator shell;
        const auto app = shell.open_session("lod-field-app");
        const Rights rwc = Right::Read | Right::Write | Right::Connect;
        const Rights rc = Right::Read | Right::Connect;

        const auto camera = shell.create_buffer(app, {
            "camera-ubo", 512, 256, Coat::Frame, rwc, ResourceState::CpuWrite});
        const auto objects = shell.create_buffer(app, {
            "object-ssbo", 4u << 20u, 256, Coat::Storage, rwc, ResourceState::CpuWrite});
        const auto visible = shell.create_buffer(app, {
            "visible-ssbo", 2u << 20u, 256, Coat::Visibility, rwc, ResourceState::Undefined});
        const auto indirect = shell.create_buffer(app, {
            "indirect-commands", 4096, 256, Coat::Indirect, rwc, ResourceState::CpuWrite});
        const auto mesh = shell.create_buffer(app, {
            "mesh-resident", 12u << 20u, 256, Coat::Geometry, rc, ResourceState::CopyDestination});

        const auto camera_node = shell.attach_resource(app, camera);
        const auto objects_node = shell.attach_resource(app, objects);
        const auto visible_node = shell.attach_resource(app, visible);
        const auto indirect_node = shell.attach_resource(app, indirect);
        const auto mesh_node = shell.attach_resource(app, mesh);
        const auto cull = shell.attach_pass(app, "compute:cull-lod");
        const auto draw = shell.attach_pass(app, "draw:opaque-indirect");

        shell.connect(camera_node, cull, PortKind::Memory, Access::Read);
        shell.connect(objects_node, cull, PortKind::Memory, Access::Read);
        shell.connect(cull, visible_node, PortKind::Memory, Access::Write);
        shell.connect(cull, indirect_node, PortKind::Commands, Access::Write);
        shell.connect(visible_node, draw, PortKind::Memory, Access::Read);
        shell.connect(indirect_node, draw, PortKind::Commands, Access::Read);
        shell.connect(mesh_node, draw, PortKind::Memory, Access::Read);

        const std::array frame_work{
            Work{"compute:cull-lod", {
                {camera, ResourceState::ShaderRead, Access::Read},
                {objects, ResourceState::ShaderRead, Access::Read},
                {visible, ResourceState::ShaderWrite, Access::Write},
                {indirect, ResourceState::ShaderWrite, Access::Write},
            }},
            Work{"draw:opaque-indirect", {
                {visible, ResourceState::ShaderRead, Access::Read},
                {indirect, ResourceState::IndirectRead, Access::Read},
                {mesh, ResourceState::VertexRead, Access::Read},
            }},
        };

        const auto submission = shell.submit(app, frame_work);
        std::cout << "EIN Full-Path Graphics Relay / correctness slice\n"
                  << "session: " << static_cast<unsigned>(app)
                  << "  fence: " << submission.fence << "\n\n"
                  << "typed connectivity\n" << shell.graph().describe() << "\n"
                  << "barriers emitted\n";
        for (const auto& barrier : submission.barriers)
            std::cout << "  " << barrier.before << " -> " << barrier.after
                      << " : " << barrier.gl_bits << '\n';

        std::cout << "\nmemory coats\n";
        for (const auto coat : {Coat::Bootstrap, Coat::Frame, Coat::Upload,
                                Coat::Stream, Coat::Geometry, Coat::Texture,
                                Coat::Storage, Coat::Visibility, Coat::Indirect,
                                Coat::Attachment, Coat::History, Coat::Readback,
                                Coat::Recovery}) {
            const auto stat = shell.memory().stats(coat);
            std::cout << "  " << std::setw(9) << to_string(coat)
                      << " used=" << stat.used << " capacity=" << stat.capacity
                      << " largest-free=" << stat.largest_free_block << '\n';
        }

        shell.complete(submission.fence);
        shell.retire(app, visible);
        const auto cleanup = shell.submit(app, std::span<const Work>{});
        shell.complete(cleanup.fence);
        std::cout << "\nretirement: visible-ssbo reclaimed after fence "
                  << cleanup.fence << "\n";

        bool stale_rejected = false;
        try {
            (void)shell.memory().view(visible);
        } catch (const std::runtime_error&) {
            stale_rejected = true;
        }
        if (!stale_rejected) throw std::runtime_error("stale EIN handle was accepted");

        const auto replacement = shell.create_buffer(app, {
            "visible-ssbo-next-generation", 2u << 20u, 256, Coat::Visibility,
            rwc, ResourceState::Undefined});
        if (replacement.slot() != visible.slot() ||
            replacement.generation() == visible.generation())
            throw std::runtime_error("retired EIN slot was not safely regenerated");
        std::cout << "generation: stale handle rejected; slot " << replacement.slot()
                  << " advanced " << visible.generation() << " -> "
                  << replacement.generation() << '\n';

        FullPathRelay relay(1920, 1080, FullPathConfig{
            .internal_scale = 0.58F,
            .full_refresh_interval = 3,
            .minimum_confidence = 0.62F,
            .maximum_preservation_error = 1.0F / 255.0F,
            .workgroup_alignment = 8,
        });
        const auto seed = relay.plan();
        if (seed.mode != PathFrameMode::FullSeed || seed.input_width != 1920 ||
            seed.input_height != 1080 || seed.history_weight != 0.0F)
            throw std::runtime_error("full path did not begin from a native seed");
        const PathProof seed_proof{
            seed.seed_generation, true, true, false, 0, 0.0F};
        if (!relay.commit(seed, seed_proof))
            throw std::runtime_error("valid full seed proof was rejected");
        relay.observe_confidence(0.91F);
        const auto relayed = relay.plan();
        if (relayed.mode != PathFrameMode::Relay || relayed.seed_generation != 1)
            throw std::runtime_error("full path did not enter relay mode");
        const PathProof relay_proof{
            relayed.seed_generation, true, true, true, 0, 1.0F / 1024.0F};
        if (!relay.commit(relayed, relay_proof))
            throw std::runtime_error("valid relay proof was rejected");

        const auto unproven = relay.plan();
        const PathProof failed_proof{
            unproven.seed_generation, false, true, true, 4, 0.0F};
        if (relay.commit(unproven, failed_proof))
            throw std::runtime_error("unproven relay was allowed into history");
        const auto fail_closed = relay.plan();
        if (fail_closed.mode != PathFrameMode::FullSeed)
            throw std::runtime_error("failed proof did not force a full path");

        if (!relay.commit(fail_closed, PathProof{
                fail_closed.seed_generation, true, true, false, 0, 0.0F}))
            throw std::runtime_error("recovery full seed was rejected");

        relay.observe_confidence(0.95F);
        const auto excessive_error = relay.plan();
        if (relay.commit(excessive_error, PathProof{
                excessive_error.seed_generation, true, true, true, 0, 0.02F}))
            throw std::runtime_error("out-of-bound preservation error was accepted");
        const auto error_recovery = relay.plan();
        if (error_recovery.mode != PathFrameMode::FullSeed)
            throw std::runtime_error("preservation error did not force a full path");
        if (!relay.commit(error_recovery, PathProof{
                error_recovery.seed_generation, true, true, false, 0, 0.0F}))
            throw std::runtime_error("error-recovery full seed was rejected");
        const auto cut = relay.plan(true, false);
        if (cut.mode != PathFrameMode::FullSeed)
            throw std::runtime_error("camera cut failed to request a full seed");

        std::cout << "\nfull-path relay\n"
                  << "  seed: " << seed.input_width << 'x' << seed.input_height
                  << " generation=" << seed.seed_generation << '\n'
                  << "  relay: " << relayed.input_width << 'x' << relayed.input_height
                  << " -> " << relayed.output_width << 'x' << relayed.output_height
                  << " history-weight=" << std::fixed << std::setprecision(3)
                  << relayed.history_weight << '\n'
                  << "  proof failure: " << fail_closed.reason << '\n'
                  << "  error failure: " << error_recovery.reason << '\n'
                  << "  refresh trigger: " << cut.reason << "\n\n"
                  << "coat pathway\n";
        for (const auto& step : FullPathRelay::coat_route())
            std::cout << "  C" << static_cast<unsigned>(step.coat) << ' '
                      << to_string(step.coat) << " : " << step.payload << '\n';

        MassiveUnitEvidence classification_unit;
        classification_unit.seed_generation = relay.seed_generation();
        classification_unit.history_seed_generation = relay.seed_generation();
        for (std::uint32_t y = 0; y < massive_unit_tile_extent; ++y) {
            for (std::uint32_t x = 0; x < massive_unit_tile_extent; ++x) {
                TileEvidence tile;
                tile.tile_x = x;
                tile.tile_y = y;
                tile.minimum_confidence = 0.998F;
                tile.maximum_depth_error = 0.001F;
                tile.cost_pressure = {0.5F, 0.3F, 0.2F, 1.2F, 0.1F, 0.4F};
                classification_unit.tiles.push_back(tile);
            }
        }
        classification_unit.tiles[5].refine_pixels = 20;
        classification_unit.tiles[5].minimum_confidence = 0.8F;
        classification_unit.tiles[5].maximum_depth_error = 0.01F;
        classification_unit.tiles[19].refine_pixels = 220;
        classification_unit.tiles[19].minimum_confidence = 0.2F;
        const std::array classification_input{classification_unit};
        const auto classified = MassiveClassifier{}.classify(classification_input);
        std::cout << "\nmassive classification unit\n"
                  << "  extent: " << massive_unit_pixel_extent << 'x'
                  << massive_unit_pixel_extent << " pixels / "
                  << classified.tiles.size() << " leaf tiles\n"
                  << "  queues: relay=" << classified.queues[0].size()
                  << " selective=" << classified.queues[1].size()
                  << " native=" << classified.queues[2].size()
                  << " full=" << classified.queues[3].size() << '\n'
                  << "  hot cost: " << to_string(classified.hot_cost)
                  << "  full required: " << (classified.full_required ? "yes" : "no") << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
