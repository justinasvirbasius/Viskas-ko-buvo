#include "ein/graphics_shell.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <queue>
#include <sstream>
#include <stdexcept>

namespace ein {

namespace {
constexpr std::size_t align_up(std::size_t value, std::size_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        throw std::invalid_argument("alignment must be a non-zero power of two");
    }
    if (value > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
        throw std::overflow_error("alignment overflow");
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr bool is_gpu_state(ResourceState state) {
    return state == ResourceState::ShaderRead || state == ResourceState::ShaderWrite ||
           state == ResourceState::VertexRead || state == ResourceState::IndexRead ||
           state == ResourceState::IndirectRead || state == ResourceState::CopySource ||
           state == ResourceState::CopyDestination;
}
} // namespace

EinBits EinBits::make(std::uint32_t slot, std::uint16_t generation,
                      ResourceKind kind, Coat coat, Rights rights,
                      std::uint8_t domain) {
    if (slot == 0 || slot > 0xFFFFFu) throw std::invalid_argument("EIN slot out of range");
    if (generation == 0 || generation > 0xFFFu) throw std::invalid_argument("EIN generation out of range");
    const auto raw = static_cast<std::uint64_t>(slot) |
                     (static_cast<std::uint64_t>(generation) << 20u) |
                     (static_cast<std::uint64_t>(kind) << 32u) |
                     (static_cast<std::uint64_t>(coat) << 40u) |
                     (static_cast<std::uint64_t>(rights) << 48u) |
                     (static_cast<std::uint64_t>(domain) << 56u);
    return EinBits{raw};
}

std::size_t MemoryCoatManager::coat_index(Coat coat) {
    const auto i = static_cast<std::size_t>(coat);
    if (i >= coat_count) throw std::invalid_argument("unknown memory coat");
    return i;
}

MemoryCoatManager::MemoryCoatManager(std::array<std::size_t, coat_count> capacities) {
    for (std::size_t i = 0; i < arenas_.size(); ++i) {
        arenas_[i].capacity = capacities[i];
        arenas_[i].free.push_back({0, capacities[i]});
    }
}

std::optional<std::size_t> MemoryCoatManager::allocate(Arena& arena,
                                                        std::size_t bytes,
                                                        std::size_t alignment) {
    for (std::size_t i = 0; i < arena.free.size(); ++i) {
        const auto extent = arena.free[i];
        const auto aligned = align_up(extent.offset, alignment);
        if (aligned < extent.offset || aligned - extent.offset > extent.bytes) continue;
        const auto prefix = aligned - extent.offset;
        if (bytes > extent.bytes - prefix) continue;
        const auto suffix = extent.bytes - prefix - bytes;
        arena.free.erase(arena.free.begin() + static_cast<std::ptrdiff_t>(i));
        if (suffix) arena.free.insert(arena.free.begin() + static_cast<std::ptrdiff_t>(i),
                                     Extent{aligned + bytes, suffix});
        if (prefix) arena.free.insert(arena.free.begin() + static_cast<std::ptrdiff_t>(i),
                                     Extent{extent.offset, prefix});
        return aligned;
    }
    return std::nullopt;
}

void MemoryCoatManager::release(Arena& arena, Extent extent) {
    arena.free.push_back(extent);
    std::sort(arena.free.begin(), arena.free.end(),
              [](const Extent& a, const Extent& b) { return a.offset < b.offset; });
    std::vector<Extent> merged;
    for (const auto& item : arena.free) {
        if (!merged.empty() && merged.back().offset + merged.back().bytes == item.offset) {
            merged.back().bytes += item.bytes;
        } else {
            merged.push_back(item);
        }
    }
    arena.free = std::move(merged);
}

EinBits MemoryCoatManager::create_buffer(std::uint8_t domain, const BufferDesc& desc) {
    if (domain == 0) throw std::invalid_argument("domain zero is reserved");
    if (desc.bytes == 0) throw std::invalid_argument("zero-byte graphics buffer");
    const auto arena_index = coat_index(desc.coat);
    auto offset = allocate(arenas_[arena_index], desc.bytes, desc.alignment);
    if (!offset) throw std::runtime_error("memory coat exhausted: " + std::string(to_string(desc.coat)));

    std::uint32_t slot_index{};
    if (!reusable_.empty()) {
        slot_index = reusable_.back();
        reusable_.pop_back();
    } else {
        if (slots_.size() > 0xFFFFFu) throw std::runtime_error("EIN slot table exhausted");
        slot_index = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back({});
    }
    auto& slot = slots_[slot_index];
    slot.alive = true;
    slot.retire_fence = 0;
    slot.desc = desc;
    slot.offset = *offset;
    return EinBits::make(slot_index, slot.generation, ResourceKind::Buffer,
                         desc.coat, desc.rights, domain);
}

MemoryCoatManager::Slot& MemoryCoatManager::checked(EinBits id) {
    if (!id.valid() || id.slot() >= slots_.size()) throw std::invalid_argument("invalid EIN handle");
    auto& slot = slots_[id.slot()];
    if (!slot.alive || slot.generation != id.generation() || id.kind() != ResourceKind::Buffer)
        throw std::runtime_error("stale or mismatched EIN handle");
    return slot;
}

const MemoryCoatManager::Slot& MemoryCoatManager::checked(EinBits id) const {
    if (!id.valid() || id.slot() >= slots_.size()) throw std::invalid_argument("invalid EIN handle");
    const auto& slot = slots_[id.slot()];
    if (!slot.alive || slot.generation != id.generation() || id.kind() != ResourceKind::Buffer)
        throw std::runtime_error("stale or mismatched EIN handle");
    return slot;
}

void MemoryCoatManager::transition(EinBits id, ResourceState next) {
    auto& slot = checked(id);
    if (slot.desc.initial_state == ResourceState::Retiring)
        throw std::runtime_error("cannot transition a retiring resource");
    if (next == ResourceState::CpuWrite && !id.permits(Right::Write))
        throw std::runtime_error("EIN rights deny CPU write");
    if ((next == ResourceState::ShaderWrite || next == ResourceState::CopyDestination) &&
        !id.permits(Right::Write))
        throw std::runtime_error("EIN rights deny GPU write");
    if ((next == ResourceState::ShaderRead || next == ResourceState::VertexRead ||
         next == ResourceState::IndexRead || next == ResourceState::IndirectRead ||
         next == ResourceState::CopySource) && !id.permits(Right::Read))
        throw std::runtime_error("EIN rights deny GPU read");
    if (next == ResourceState::CpuRead && slot.desc.coat != Coat::Readback &&
        slot.desc.coat != Coat::Recovery)
        throw std::runtime_error("CPU read is restricted to readback and recovery coats");
    slot.desc.initial_state = next;
}

void MemoryCoatManager::retire(EinBits id, std::uint64_t after_fence) {
    auto& slot = checked(id);
    slot.desc.initial_state = ResourceState::Retiring;
    slot.retire_fence = after_fence;
}

void MemoryCoatManager::collect(std::uint64_t completed_fence) {
    for (std::uint32_t i = 1; i < slots_.size(); ++i) {
        auto& slot = slots_[i];
        if (!slot.alive || slot.desc.initial_state != ResourceState::Retiring ||
            slot.retire_fence > completed_fence) continue;
        release(arenas_[coat_index(slot.desc.coat)], {slot.offset, slot.desc.bytes});
        slot.alive = false;
        slot.generation = static_cast<std::uint16_t>((slot.generation % 0xFFFu) + 1u);
        reusable_.push_back(i);
    }
}

AllocationView MemoryCoatManager::view(EinBits id) const {
    const auto& slot = checked(id);
    return {id, slot.desc.name, slot.offset, slot.desc.bytes,
            slot.desc.coat, slot.desc.initial_state};
}

CoatStats MemoryCoatManager::stats(Coat coat) const {
    const auto& arena = arenas_[coat_index(coat)];
    std::size_t free_bytes = 0, largest = 0;
    for (const auto& item : arena.free) {
        free_bytes += item.bytes;
        largest = std::max(largest, item.bytes);
    }
    return {arena.capacity, arena.capacity - free_bytes, largest};
}

NodeId ConnectivityGraph::add_node(NodeKind kind, std::string name,
                                    std::uint8_t domain) {
    if (domain == 0) throw std::invalid_argument("graph nodes require a domain");
    nodes_.push_back({kind, std::move(name), domain});
    return static_cast<NodeId>(nodes_.size() - 1);
}

void ConnectivityGraph::connect(NodeId from, NodeId to, PortKind port, Access access) {
    if (from == 0 || to == 0 || from >= nodes_.size() || to >= nodes_.size())
        throw std::invalid_argument("connection endpoint does not exist");
    if (from == to) throw std::invalid_argument("self connections are not executable");
    const auto& a = nodes_[from];
    const auto& b = nodes_[to];
    if (a.domain != b.domain && access != Access::Read)
        throw std::runtime_error("cross-domain links are read-only unless explicitly shared");
    if (port == PortKind::Pixels && b.kind != NodeKind::Surface)
        throw std::runtime_error("pixel output must terminate at a surface");
    links_.push_back({from, to, port, access});
}

std::vector<NodeId> ConnectivityGraph::execution_order() const {
    std::vector<std::size_t> indegree(nodes_.size());
    std::vector<std::vector<NodeId>> outgoing(nodes_.size());
    for (const auto& link : links_) {
        ++indegree[link.to];
        outgoing[link.from].push_back(link.to);
    }
    std::queue<NodeId> ready;
    for (NodeId i = 1; i < nodes_.size(); ++i) if (indegree[i] == 0) ready.push(i);
    std::vector<NodeId> result;
    while (!ready.empty()) {
        const auto node = ready.front(); ready.pop(); result.push_back(node);
        for (const auto next : outgoing[node]) if (--indegree[next] == 0) ready.push(next);
    }
    if (result.size() != nodes_.size() - 1)
        throw std::runtime_error("connectivity graph contains a cycle");
    return result;
}

std::string ConnectivityGraph::describe() const {
    std::ostringstream out;
    for (const auto& link : links_)
        out << nodes_[link.from].name << " -> " << nodes_[link.to].name
            << " [port=" << static_cast<int>(link.port)
            << ", access=" << static_cast<int>(link.access) << "]\n";
    return out.str();
}

GraphicsAdministrator::GraphicsAdministrator()
    : memory_({
          2u << 20u,   // C0 Bootstrap
          8u << 20u,   // C1 Frame
          32u << 20u,  // C2 Upload
          32u << 20u,  // C3 Stream
          256u << 20u, // C4 Geometry
          512u << 20u, // C5 Texture
          128u << 20u, // C6 Storage
          64u << 20u,  // C7 Visibility
          16u << 20u,  // C8 Indirect
          256u << 20u, // C9 Attachment
          128u << 20u, // C10 History
          16u << 20u,  // C11 Readback
          64u << 20u,  // C12 Recovery
      }) {}

std::uint8_t GraphicsAdministrator::open_session(std::string name) {
    if (next_session_ == 0) throw std::runtime_error("graphics session space exhausted");
    const auto id = next_session_++;
    const auto node = graph_.add_node(NodeKind::Session, name, id);
    sessions_.emplace(id, Session{std::move(name), node});
    return id;
}

EinBits GraphicsAdministrator::create_buffer(std::uint8_t session,
                                              const BufferDesc& desc) {
    if (!sessions_.contains(session)) throw std::invalid_argument("unknown graphics session");
    return memory_.create_buffer(session, desc);
}

NodeId GraphicsAdministrator::attach_resource(std::uint8_t session, EinBits resource) {
    if (!sessions_.contains(session) || resource.domain() != session)
        throw std::runtime_error("resource does not belong to the session");
    const auto item = memory_.view(resource);
    return graph_.add_node(NodeKind::Resource, std::string(item.name), session);
}

NodeId GraphicsAdministrator::attach_pass(std::uint8_t session, std::string name) {
    if (!sessions_.contains(session)) throw std::invalid_argument("unknown graphics session");
    return graph_.add_node(NodeKind::Pass, std::move(name), session);
}

void GraphicsAdministrator::connect(NodeId from, NodeId to, PortKind port, Access access) {
    graph_.connect(from, to, port, access);
}

std::string GraphicsAdministrator::barrier_bits(ResourceState before,
                                                ResourceState after,
                                                Access access) {
    if (before == after && access == Access::Read) return {};
    if (before == ResourceState::CpuWrite && is_gpu_state(after))
        return "GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT";
    if (before == ResourceState::ShaderWrite && after == ResourceState::IndirectRead)
        return "GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT";
    if (before == ResourceState::ShaderWrite && after == ResourceState::VertexRead)
        return "GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT";
    if (before == ResourceState::ShaderWrite)
        return "GL_SHADER_STORAGE_BARRIER_BIT";
    if (before == ResourceState::CopyDestination)
        return "GL_BUFFER_UPDATE_BARRIER_BIT";
    if (after == ResourceState::CpuRead)
        return "GL_BUFFER_UPDATE_BARRIER_BIT | GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT";
    return "GL_ALL_BARRIER_BITS /* conservative until backend specialization */";
}

Submission GraphicsAdministrator::submit(std::uint8_t session,
                                         std::span<const Work> work) {
    if (!sessions_.contains(session)) throw std::invalid_argument("unknown graphics session");
    (void)graph_.execution_order();
    Submission submission{++submitted_fence_, {}};
    for (const auto& command : work) {
        for (const auto& use : command.uses) {
            if (use.resource.domain() != session)
                throw std::runtime_error("submission attempted to use another session's resource");
            const auto before = memory_.view(use.resource).state;
            const auto bits = barrier_bits(before, use.required, use.access);
            if (!bits.empty()) submission.barriers.push_back(
                {std::string(to_string(before)), std::string(to_string(use.required)), bits});
            memory_.transition(use.resource, use.required);
        }
    }
    return submission;
}

void GraphicsAdministrator::complete(std::uint64_t fence) {
    if (fence > submitted_fence_) throw std::invalid_argument("cannot complete an unknown fence");
    completed_fence_ = std::max(completed_fence_, fence);
    memory_.collect(completed_fence_);
}

void GraphicsAdministrator::retire(std::uint8_t session, EinBits resource) {
    if (resource.domain() != session) throw std::runtime_error("retire denied across domains");
    memory_.retire(resource, submitted_fence_);
}

FullPathRelay::FullPathRelay(std::uint32_t output_width,
                             std::uint32_t output_height,
                             FullPathConfig config)
    : output_width_(output_width), output_height_(output_height), config_(config) {
    if (output_width == 0 || output_height == 0)
        throw std::invalid_argument("full-path output extent must be non-zero");
    if (!(config.internal_scale >= 0.25F && config.internal_scale <= 1.0F))
        throw std::invalid_argument("internal scale must be within [0.25, 1.0]");
    if (config.full_refresh_interval == 0)
        throw std::invalid_argument("full refresh interval must be non-zero");
    if (!(config.minimum_confidence >= 0.0F && config.minimum_confidence <= 1.0F))
        throw std::invalid_argument("minimum confidence must be within [0, 1]");
    if (!(config.maximum_preservation_error >= 0.0F &&
          config.maximum_preservation_error <= 1.0F))
        throw std::invalid_argument("maximum preservation error must be within [0, 1]");
    if (config.workgroup_alignment == 0 ||
        (config.workgroup_alignment & (config.workgroup_alignment - 1u)) != 0)
        throw std::invalid_argument("workgroup alignment must be a power of two");
}

std::uint32_t FullPathRelay::aligned_extent(std::uint32_t full, float scale,
                                            std::uint32_t alignment) {
    const auto scaled = static_cast<std::uint32_t>(
        std::max(1.0F, std::ceil(static_cast<float>(full) * scale)));
    return (scaled + alignment - 1u) & ~(alignment - 1u);
}

PathFramePlan FullPathRelay::plan(bool camera_cut, bool topology_changed) {
    if (camera_cut) invalidate("camera cut");
    else if (topology_changed) invalidate("topology changed");

    std::string reason;
    if (!history_valid_) reason = invalid_reason_;
    else if (confidence_ < config_.minimum_confidence) reason = "relay confidence below threshold";
    else if (relay_frames_ >= config_.full_refresh_interval) reason = "scheduled full refresh";

    if (!reason.empty()) {
        return {PathFrameMode::FullSeed, output_width_, output_height_,
                output_width_, output_height_, seed_generation_ + 1u, 0.0F,
                std::move(reason)};
    }

    const float confidence_range = std::max(0.001F, 1.0F - config_.minimum_confidence);
    const float normalized = std::clamp(
        (confidence_ - config_.minimum_confidence) / confidence_range, 0.0F, 1.0F);
    const float history_weight = 0.72F + 0.22F * normalized;
    return {PathFrameMode::Relay,
            aligned_extent(output_width_, config_.internal_scale, config_.workgroup_alignment),
            aligned_extent(output_height_, config_.internal_scale, config_.workgroup_alignment),
            output_width_, output_height_, seed_generation_, history_weight,
            "motion/depth/confidence relay"};
}

bool FullPathRelay::commit(const PathFramePlan& completed, const PathProof& proof) {
    if (completed.output_width != output_width_ || completed.output_height != output_height_)
        throw std::runtime_error("completed path extent does not match the relay");
    if (proof.seed_generation != completed.seed_generation)
        throw std::runtime_error("proof and frame use different full-seed generations");
    if (!std::isfinite(proof.maximum_error))
        throw std::invalid_argument("proof error must be finite");

    const bool common_proof = proof.identity_complete && proof.depth_complete &&
                              proof.unresolved_tiles == 0 &&
                              proof.maximum_error <= config_.maximum_preservation_error;
    const bool correct = common_proof &&
                         (completed.mode == PathFrameMode::FullSeed || proof.motion_complete);
    if (!correct) {
        invalidate("correctness proof failed; native full path required");
        return false;
    }

    if (completed.mode == PathFrameMode::FullSeed) {
        if (completed.input_width != output_width_ || completed.input_height != output_height_)
            throw std::runtime_error("a full seed must run at native output extent");
        seed_generation_ = completed.seed_generation;
        relay_frames_ = 0;
        confidence_ = 1.0F;
        history_valid_ = true;
        invalid_reason_.clear();
        return true;
    }
    if (!history_valid_ || completed.seed_generation != seed_generation_)
        throw std::runtime_error("relay frame refers to an absent or stale full seed");
    ++relay_frames_;
    return true;
}

void FullPathRelay::observe_confidence(float mean_confidence) {
    if (!std::isfinite(mean_confidence))
        throw std::invalid_argument("relay confidence must be finite");
    confidence_ = std::clamp(mean_confidence, 0.0F, 1.0F);
}

void FullPathRelay::invalidate(std::string reason) {
    history_valid_ = false;
    confidence_ = 0.0F;
    invalid_reason_ = reason.empty() ? "path explicitly invalidated" : std::move(reason);
}

const std::array<RelayCoatStep, coat_count>& FullPathRelay::coat_route() noexcept {
    static constexpr std::array route{
        RelayCoatStep{Coat::Bootstrap, "reconstruction kernels and binding schema"},
        RelayCoatStep{Coat::Frame, "jitter, exposure and frame constants"},
        RelayCoatStep{Coat::Upload, "camera and material deltas"},
        RelayCoatStep{Coat::Stream, "motion, deformation and particle continuity"},
        RelayCoatStep{Coat::Geometry, "full-seed shapes and LOD ranges"},
        RelayCoatStep{Coat::Texture, "full-seed material frequencies and LUTs"},
        RelayCoatStep{Coat::Storage, "current features and reconstruction workspace"},
        RelayCoatStep{Coat::Visibility, "disocclusion, reactive and confidence masks"},
        RelayCoatStep{Coat::Indirect, "budgeted refine and reconstruction dispatches"},
        RelayCoatStep{Coat::Attachment, "current reduced-cost source raster"},
        RelayCoatStep{Coat::History, "motion-reprojected native pathway"},
        RelayCoatStep{Coat::Readback, "confidence and preservation telemetry"},
        RelayCoatStep{Coat::Recovery, "full-seed recipe and path generation"},
    };
    return route;
}

std::string_view to_string(Coat value) noexcept {
    switch (value) {
        case Coat::Bootstrap: return "bootstrap";
        case Coat::Frame: return "frame";
        case Coat::Upload: return "upload";
        case Coat::Stream: return "stream";
        case Coat::Geometry: return "geometry";
        case Coat::Texture: return "texture";
        case Coat::Storage: return "storage";
        case Coat::Visibility: return "visibility";
        case Coat::Indirect: return "indirect";
        case Coat::Attachment: return "attachment";
        case Coat::History: return "history";
        case Coat::Readback: return "readback";
        case Coat::Recovery: return "recovery";
    }
    return "unknown";
}

std::string_view to_string(ResourceState value) noexcept {
    switch (value) {
        case ResourceState::Undefined: return "undefined";
        case ResourceState::CpuWrite: return "cpu-write";
        case ResourceState::CpuRead: return "cpu-read";
        case ResourceState::CopySource: return "copy-source";
        case ResourceState::CopyDestination: return "copy-destination";
        case ResourceState::ShaderRead: return "shader-read";
        case ResourceState::ShaderWrite: return "shader-write";
        case ResourceState::VertexRead: return "vertex-read";
        case ResourceState::IndexRead: return "index-read";
        case ResourceState::IndirectRead: return "indirect-read";
        case ResourceState::Retiring: return "retiring";
    }
    return "unknown";
}

std::string_view to_string(PathFrameMode value) noexcept {
    switch (value) {
        case PathFrameMode::FullSeed: return "full-seed";
        case PathFrameMode::Relay: return "relay";
    }
    return "unknown";
}

} // namespace ein
