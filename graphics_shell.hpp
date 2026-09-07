#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ein {

enum class ResourceKind : std::uint8_t {
    Invalid = 0,
    Buffer = 1,
    Texture = 2,
    Shader = 3,
    Pass = 4,
    Surface = 5,
    Session = 6,
};

// A coat is a graphics-specific residency and lifetime policy, not merely a heap.
enum class Coat : std::uint8_t {
    Bootstrap = 0,  // capability tables, fallbacks and device-start data
    Frame = 1,      // short-lived uniforms and command data
    Upload = 2,     // CPU-written copy staging
    Stream = 3,     // rotating vertices, instances and particles
    Geometry = 4,   // durable vertex, index and meshlet data
    Texture = 5,    // sampled images, LUTs and sparse pages
    Storage = 6,    // general SSBOs and shader images
    Visibility = 7, // bounds, LOD and occlusion products
    Indirect = 8,   // draw/dispatch commands and counters
    Attachment = 9, // color, depth and G-buffer targets
    History = 10,   // temporal accumulation and prior-frame state
    Readback = 11,  // GPU-written telemetry returned to the CPU
    Recovery = 12,  // recipes and shadow data used for device rebuild
};

enum class Right : std::uint8_t {
    Read = 1u << 0,
    Write = 1u << 1,
    Execute = 1u << 2,
    Connect = 1u << 3,
    Share = 1u << 4,
    Present = 1u << 5,
};

using Rights = std::uint8_t;
constexpr Rights operator|(Right a, Right b) noexcept {
    return static_cast<Rights>(static_cast<Rights>(a) | static_cast<Rights>(b));
}
constexpr Rights operator|(Rights a, Right b) noexcept {
    return static_cast<Rights>(a | static_cast<Rights>(b));
}

// EIN bits are the routable identity/capability word shared by apps and the administrator.
// [63:56 domain][55:48 rights][47:40 coat][39:32 kind][31:20 generation][19:0 slot]
class EinBits {
public:
    constexpr EinBits() noexcept = default;
    explicit constexpr EinBits(std::uint64_t raw) noexcept : raw_(raw) {}

    static EinBits make(std::uint32_t slot, std::uint16_t generation,
                        ResourceKind kind, Coat coat, Rights rights,
                        std::uint8_t domain);

    [[nodiscard]] constexpr std::uint64_t raw() const noexcept { return raw_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return slot() != 0; }
    [[nodiscard]] constexpr std::uint32_t slot() const noexcept {
        return static_cast<std::uint32_t>(raw_ & 0xFFFFFu);
    }
    [[nodiscard]] constexpr std::uint16_t generation() const noexcept {
        return static_cast<std::uint16_t>((raw_ >> 20u) & 0xFFFu);
    }
    [[nodiscard]] constexpr ResourceKind kind() const noexcept {
        return static_cast<ResourceKind>((raw_ >> 32u) & 0xFFu);
    }
    [[nodiscard]] constexpr Coat coat() const noexcept {
        return static_cast<Coat>((raw_ >> 40u) & 0xFFu);
    }
    [[nodiscard]] constexpr Rights rights() const noexcept {
        return static_cast<Rights>((raw_ >> 48u) & 0xFFu);
    }
    [[nodiscard]] constexpr std::uint8_t domain() const noexcept {
        return static_cast<std::uint8_t>((raw_ >> 56u) & 0xFFu);
    }
    [[nodiscard]] constexpr bool permits(Right right) const noexcept {
        return (rights() & static_cast<Rights>(right)) != 0;
    }

    friend constexpr bool operator==(EinBits, EinBits) noexcept = default;

private:
    std::uint64_t raw_{};
};

enum class ResourceState : std::uint8_t {
    Undefined,
    CpuWrite,
    CpuRead,
    CopySource,
    CopyDestination,
    ShaderRead,
    ShaderWrite,
    VertexRead,
    IndexRead,
    IndirectRead,
    Retiring,
};

struct BufferDesc {
    std::string name;
    std::size_t bytes{};
    std::size_t alignment{256};
    Coat coat{Coat::Storage};
    Rights rights{};
    ResourceState initial_state{ResourceState::Undefined};
};

struct AllocationView {
    EinBits id;
    std::string_view name;
    std::size_t offset{};
    std::size_t bytes{};
    Coat coat{};
    ResourceState state{};
};

struct CoatStats {
    std::size_t capacity{};
    std::size_t used{};
    std::size_t largest_free_block{};
};

inline constexpr std::size_t coat_count = 13;

class MemoryCoatManager {
public:
    explicit MemoryCoatManager(std::array<std::size_t, coat_count> capacities);

    [[nodiscard]] EinBits create_buffer(std::uint8_t domain, const BufferDesc& desc);
    void transition(EinBits id, ResourceState next);
    void retire(EinBits id, std::uint64_t after_fence);
    void collect(std::uint64_t completed_fence);

    [[nodiscard]] AllocationView view(EinBits id) const;
    [[nodiscard]] CoatStats stats(Coat coat) const;

private:
    struct Extent { std::size_t offset{}; std::size_t bytes{}; };
    struct Arena {
        std::size_t capacity{};
        std::vector<Extent> free;
    };
    struct Slot {
        std::uint16_t generation{1};
        bool alive{};
        std::uint64_t retire_fence{};
        BufferDesc desc;
        std::size_t offset{};
    };

    [[nodiscard]] Slot& checked(EinBits id);
    [[nodiscard]] const Slot& checked(EinBits id) const;
    static std::size_t coat_index(Coat coat);
    static std::optional<std::size_t> allocate(Arena& arena, std::size_t bytes,
                                                std::size_t alignment);
    static void release(Arena& arena, Extent extent);

    std::array<Arena, coat_count> arenas_{};
    std::vector<Slot> slots_{1}; // slot zero is always invalid
    std::vector<std::uint32_t> reusable_;
};

enum class NodeKind : std::uint8_t { Session, Resource, Shader, Pass, Queue, Surface };
enum class PortKind : std::uint8_t { Control, Memory, Commands, Pixels, Telemetry };
enum class Access : std::uint8_t { Read, Write, ReadWrite, Execute, Present };
using NodeId = std::uint32_t;

struct Link {
    NodeId from{};
    NodeId to{};
    PortKind port{};
    Access access{};
};

class ConnectivityGraph {
public:
    [[nodiscard]] NodeId add_node(NodeKind kind, std::string name,
                                  std::uint8_t domain);
    void connect(NodeId from, NodeId to, PortKind port, Access access);
    [[nodiscard]] std::vector<NodeId> execution_order() const;
    [[nodiscard]] std::string describe() const;

private:
    struct Node { NodeKind kind{}; std::string name; std::uint8_t domain{}; };
    std::vector<Node> nodes_{{NodeKind::Session, "invalid", 0}};
    std::vector<Link> links_;
};

struct Use {
    EinBits resource;
    ResourceState required{};
    Access access{};
};

struct Work {
    std::string name;
    std::vector<Use> uses;
};

struct Barrier {
    std::string before;
    std::string after;
    std::string gl_bits;
};

struct Submission {
    std::uint64_t fence{};
    std::vector<Barrier> barriers;
};

enum class PathFrameMode : std::uint8_t {
    FullSeed,
    Relay,
};

struct FullPathConfig {
    float internal_scale{0.58F};
    std::uint32_t full_refresh_interval{120};
    float minimum_confidence{0.62F};
    float maximum_preservation_error{1.0F / 255.0F};
    std::uint32_t workgroup_alignment{8};
};

struct PathFramePlan {
    PathFrameMode mode{PathFrameMode::FullSeed};
    std::uint32_t input_width{};
    std::uint32_t input_height{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t seed_generation{};
    float history_weight{};
    std::string reason;
};

struct PathProof {
    std::uint32_t seed_generation{};
    bool identity_complete{};
    bool depth_complete{};
    bool motion_complete{};
    std::uint32_t unresolved_tiles{};
    float maximum_error{};
};

struct RelayCoatStep {
    Coat coat{};
    std::string_view payload;
};

// FullPathRelay is the public feature. The graph and memory administrator are
// supporting machinery beneath it, not the product's identity.
class FullPathRelay {
public:
    FullPathRelay(std::uint32_t output_width, std::uint32_t output_height,
                  FullPathConfig config = {});

    [[nodiscard]] PathFramePlan plan(bool camera_cut = false,
                                     bool topology_changed = false);
    // Returns false and invalidates history when the completed frame cannot
    // prove continuity. The caller must then execute the returned full-seed plan.
    [[nodiscard]] bool commit(const PathFramePlan& completed, const PathProof& proof);
    void observe_confidence(float mean_confidence);
    void invalidate(std::string reason);

    [[nodiscard]] bool history_valid() const noexcept { return history_valid_; }
    [[nodiscard]] std::uint32_t seed_generation() const noexcept { return seed_generation_; }
    [[nodiscard]] static const std::array<RelayCoatStep, coat_count>& coat_route() noexcept;

private:
    static std::uint32_t aligned_extent(std::uint32_t full, float scale,
                                        std::uint32_t alignment);

    std::uint32_t output_width_{};
    std::uint32_t output_height_{};
    FullPathConfig config_{};
    bool history_valid_{};
    std::uint32_t seed_generation_{};
    std::uint32_t relay_frames_{};
    float confidence_{};
    std::string invalid_reason_{"no full seed"};
};

class GraphicsAdministrator {
public:
    GraphicsAdministrator();

    [[nodiscard]] std::uint8_t open_session(std::string name);
    [[nodiscard]] EinBits create_buffer(std::uint8_t session, const BufferDesc& desc);
    [[nodiscard]] NodeId attach_resource(std::uint8_t session, EinBits resource);
    [[nodiscard]] NodeId attach_pass(std::uint8_t session, std::string name);
    void connect(NodeId from, NodeId to, PortKind port, Access access);
    [[nodiscard]] Submission submit(std::uint8_t session, std::span<const Work> work);
    void complete(std::uint64_t fence);
    void retire(std::uint8_t session, EinBits resource);

    [[nodiscard]] const MemoryCoatManager& memory() const noexcept { return memory_; }
    [[nodiscard]] const ConnectivityGraph& graph() const noexcept { return graph_; }

private:
    struct Session { std::string name; NodeId node{}; };
    static std::string barrier_bits(ResourceState before, ResourceState after,
                                    Access access);

    MemoryCoatManager memory_;
    ConnectivityGraph graph_;
    std::unordered_map<std::uint8_t, Session> sessions_;
    std::uint8_t next_session_{1};
    std::uint64_t submitted_fence_{};
    std::uint64_t completed_fence_{};
};

[[nodiscard]] std::string_view to_string(Coat value) noexcept;
[[nodiscard]] std::string_view to_string(ResourceState value) noexcept;
[[nodiscard]] std::string_view to_string(PathFrameMode value) noexcept;

} // namespace ein
