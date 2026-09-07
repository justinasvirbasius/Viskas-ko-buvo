#pragma once

#include "ein/lan_protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ein {

enum class LanWorkKind : std::uint8_t {
    ClassifyMassiveUnit,
    SelectiveRefine,
    NativeRefine,
};

enum class LanProof : std::uint8_t {
    Identity = 1u << 0,
    Depth = 1u << 1,
    Motion = 1u << 2,
};

using LanProofs = std::uint8_t;

constexpr LanProofs operator|(LanProof left, LanProof right) noexcept {
    return static_cast<LanProofs>(left) | static_cast<LanProofs>(right);
}

constexpr LanProofs operator|(LanProofs left, LanProof right) noexcept {
    return static_cast<LanProofs>(left | static_cast<LanProofs>(right));
}

struct LanWorkRequest {
    LanWorkKind kind{LanWorkKind::ClassifyMassiveUnit};
    std::uint64_t frame_generation{};
    std::uint32_t seed_generation{};
    std::uint32_t unit_x{};
    std::uint32_t unit_y{};
    LanDigest input_digest{};
    LanFeatures required_features{};
    std::array<std::uint64_t, coat_count> coat_bytes{};
    std::uint32_t lease_milliseconds{50};
    std::uint32_t maximum_rtt_microseconds{};
    LanProofs required_proofs{
        LanProof::Identity | LanProof::Depth | LanProof::Motion};
    float maximum_error{1.0F / 255.0F};
};

struct LanLease {
    std::uint64_t lease_id{};
    LanNodeId node;
    LanWorkKind kind{LanWorkKind::ClassifyMassiveUnit};
    std::uint64_t boot_generation{};
    std::uint64_t frame_generation{};
    std::uint32_t seed_generation{};
    std::uint32_t unit_x{};
    std::uint32_t unit_y{};
    LanDigest input_digest{};
    LanFeatures required_features{};
    std::array<std::uint64_t, coat_count> coat_bytes{};
    LanProofs required_proofs{};
    float maximum_error{};
    std::uint64_t expires_at_milliseconds{};
};

// The digest and proof fields below are outputs of the coordinator's local
// verifier over the received payload, never claims copied from a remote node.
struct LanVerifiedResult {
    std::uint64_t lease_id{};
    LanNodeId node;
    std::uint64_t boot_generation{};
    std::uint64_t frame_generation{};
    std::uint32_t seed_generation{};
    std::uint32_t unit_x{};
    std::uint32_t unit_y{};
    LanDigest input_digest{};
    LanDigest output_digest{};
    LanProofs proofs{};
    std::uint32_t unresolved_pixels{};
    float maximum_error{};
};

enum class LanAdvertisementStatus : std::uint8_t {
    Accepted,
    Refreshed,
    Restarted,
    Stale,
    Invalid,
};

enum class LanResultStatus : std::uint8_t {
    Accepted,
    UnknownLease,
    Duplicate,
    NodeMismatch,
    BootMismatch,
    GenerationMismatch,
    DigestMismatch,
    ProofIncomplete,
    Unresolved,
    ErrorExceeded,
    Expired,
};

enum class LanFallbackReason : std::uint8_t {
    LeaseExpired,
    NodeExpired,
    NodeRestarted,
    TrustRevoked,
    BootMismatch,
    GenerationMismatch,
    DigestMismatch,
    ProofIncomplete,
    Unresolved,
    ErrorExceeded,
};

struct LanFallbackWork {
    std::uint64_t lease_id{};
    LanWorkRequest request;
    LanFallbackReason reason{LanFallbackReason::LeaseExpired};
};

struct LanResourceView {
    LanNodeId node;
    std::uint64_t boot_generation{};
    std::uint64_t sequence{};
    LanFeatures features{};
    bool trusted{};
    std::uint32_t remaining_unit_slots{};
    std::uint32_t estimated_rtt_microseconds{};
    std::uint64_t expires_at_milliseconds{};
    std::array<std::uint64_t, coat_count> remaining_coat_bytes{};
};

// Coordinates discovered resources, but never authenticates them. The caller
// must bind set_trusted() to an authenticated node identity and carry leased
// work over an authenticated, encrypted transport.
class LanAdministrator {
public:
    LanAdministrator();
    ~LanAdministrator();

    LanAdministrator(const LanAdministrator&) = delete;
    LanAdministrator& operator=(const LanAdministrator&) = delete;
    LanAdministrator(LanAdministrator&&) noexcept;
    LanAdministrator& operator=(LanAdministrator&&) noexcept;

    void set_trusted(LanNodeId node, bool trusted);
    [[nodiscard]] bool is_trusted(LanNodeId node) const;

    [[nodiscard]] LanAdvertisementStatus observe(
        const LanAdvertisement& advertisement,
        std::uint64_t now_milliseconds);
    [[nodiscard]] std::size_t expire(std::uint64_t now_milliseconds);
    [[nodiscard]] std::vector<LanResourceView> resources(
        std::uint64_t now_milliseconds) const;

    [[nodiscard]] std::optional<LanLease> lease(
        const LanWorkRequest& request,
        std::uint64_t now_milliseconds);
    [[nodiscard]] LanResultStatus submit(
        const LanVerifiedResult& result,
        std::uint64_t now_milliseconds);

    [[nodiscard]] std::vector<LanFallbackWork> drain_fallbacks();
    [[nodiscard]] std::size_t active_lease_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] LanFeatures feature_for(LanWorkKind kind) noexcept;
[[nodiscard]] std::string_view to_string(LanAdvertisementStatus status) noexcept;
[[nodiscard]] std::string_view to_string(LanResultStatus status) noexcept;
[[nodiscard]] std::string_view to_string(LanFallbackReason reason) noexcept;

} // namespace ein
