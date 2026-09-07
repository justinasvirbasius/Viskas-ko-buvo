#include "ein/lan_administrator.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ein {

namespace {
struct LanNodeIdHash {
    std::size_t operator()(LanNodeId node) const noexcept {
        const auto first = std::hash<std::uint64_t>{}(node.high);
        const auto second = std::hash<std::uint64_t>{}(node.low);
        return first ^ (second + 0x9E3779B97F4A7C15ull + (first << 6u) +
                        (first >> 2u));
    }
};

bool node_less(LanNodeId left, LanNodeId right) noexcept {
    return std::tie(left.high, left.low) < std::tie(right.high, right.low);
}

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return right > maximum - left ? maximum : left + right;
}

constexpr LanProofs known_proofs =
    LanProof::Identity | LanProof::Depth | LanProof::Motion;

bool valid_request(const LanWorkRequest& request) noexcept {
    return request.frame_generation != 0 &&
           request.seed_generation != 0 &&
           feature_for(request.kind) != 0 &&
           valid_lan_digest(request.input_digest) &&
           request.lease_milliseconds != 0 &&
           request.lease_milliseconds <= 60'000 &&
           request.required_proofs != 0 &&
           (request.required_proofs & ~known_proofs) == 0 &&
           std::isfinite(request.maximum_error) &&
           request.maximum_error >= 0.0F;
}
} // namespace

struct LanAdministrator::Impl {
    struct NodeRecord {
        LanAdvertisement advertisement;
        std::uint64_t last_seen_milliseconds{};
        std::uint64_t expires_at_milliseconds{};
        std::uint32_t reserved_units{};
        std::array<std::uint64_t, coat_count> reserved_coat_bytes{};
    };

    struct LeaseRecord {
        LanLease lease;
        LanWorkRequest request;
    };

    using NodeMap = std::unordered_map<LanNodeId, NodeRecord, LanNodeIdHash>;
    using LeaseMap = std::unordered_map<std::uint64_t, LeaseRecord>;

    NodeMap nodes;
    LeaseMap leases;
    std::unordered_set<LanNodeId, LanNodeIdHash> trusted_nodes;
    std::unordered_set<std::uint64_t> completed_leases;
    std::deque<std::uint64_t> completed_order;
    std::vector<LanFallbackWork> fallbacks;
    std::uint64_t next_lease_id{1};

    void remember_completion(std::uint64_t lease_id) {
        constexpr std::size_t completion_window = 4096;
        completed_leases.insert(lease_id);
        completed_order.push_back(lease_id);
        if (completed_order.size() > completion_window) {
            completed_leases.erase(completed_order.front());
            completed_order.pop_front();
        }
    }

    void release_reservation(const LeaseRecord& record) noexcept {
        const auto node = nodes.find(record.lease.node);
        if (node == nodes.end() ||
            node->second.advertisement.boot_generation !=
                record.lease.boot_generation)
            return;
        auto& resource = node->second;
        if (resource.reserved_units != 0) --resource.reserved_units;
        for (std::size_t coat = 0; coat < coat_count; ++coat) {
            const auto bytes = record.request.coat_bytes[coat];
            resource.reserved_coat_bytes[coat] =
                bytes > resource.reserved_coat_bytes[coat]
                    ? 0
                    : resource.reserved_coat_bytes[coat] - bytes;
        }
    }

    LanResultStatus fail_lease(LeaseMap::iterator found,
                               LanResultStatus status,
                               LanFallbackReason reason) {
        const auto record = found->second;
        release_reservation(record);
        fallbacks.push_back({record.lease.lease_id, record.request, reason});
        remember_completion(record.lease.lease_id);
        leases.erase(found);
        return status;
    }

    void reclaim_node(LanNodeId node, LanFallbackReason reason) {
        for (auto found = leases.begin(); found != leases.end();) {
            if (found->second.lease.node != node) {
                ++found;
                continue;
            }
            const auto record = found->second;
            release_reservation(record);
            fallbacks.push_back({record.lease.lease_id, record.request, reason});
            remember_completion(record.lease.lease_id);
            found = leases.erase(found);
        }
    }

    std::size_t expire(std::uint64_t now_milliseconds) {
        std::vector<LanNodeId> expired_nodes;
        for (const auto& [node, record] : nodes)
            if (now_milliseconds >= record.expires_at_milliseconds)
                expired_nodes.push_back(node);

        for (const auto node : expired_nodes) {
            reclaim_node(node, LanFallbackReason::NodeExpired);
            nodes.erase(node);
        }

        for (auto found = leases.begin(); found != leases.end();) {
            if (now_milliseconds < found->second.lease.expires_at_milliseconds) {
                ++found;
                continue;
            }
            const auto record = found->second;
            release_reservation(record);
            fallbacks.push_back({record.lease.lease_id, record.request,
                                 LanFallbackReason::LeaseExpired});
            remember_completion(record.lease.lease_id);
            found = leases.erase(found);
        }
        return expired_nodes.size();
    }
};

LanAdministrator::LanAdministrator() : impl_(std::make_unique<Impl>()) {}
LanAdministrator::~LanAdministrator() = default;
LanAdministrator::LanAdministrator(LanAdministrator&&) noexcept = default;
LanAdministrator& LanAdministrator::operator=(LanAdministrator&&) noexcept = default;

void LanAdministrator::set_trusted(LanNodeId node, bool trusted) {
    if (!node.valid()) throw std::invalid_argument("cannot trust an invalid LAN node");
    if (trusted) {
        impl_->trusted_nodes.insert(node);
        return;
    }
    impl_->trusted_nodes.erase(node);
    impl_->reclaim_node(node, LanFallbackReason::TrustRevoked);
}

bool LanAdministrator::is_trusted(LanNodeId node) const {
    return impl_->trusted_nodes.contains(node);
}

LanAdvertisementStatus LanAdministrator::observe(
    const LanAdvertisement& advertisement,
    std::uint64_t now_milliseconds) {
    if (!valid_lan_advertisement(advertisement))
        return LanAdvertisementStatus::Invalid;

    const auto expires_at = saturating_add(
        now_milliseconds, advertisement.ttl_milliseconds);
    const auto found = impl_->nodes.find(advertisement.node);
    if (found == impl_->nodes.end()) {
        Impl::NodeRecord record;
        record.advertisement = advertisement;
        record.last_seen_milliseconds = now_milliseconds;
        record.expires_at_milliseconds = expires_at;
        impl_->nodes.emplace(advertisement.node, std::move(record));
        return LanAdvertisementStatus::Accepted;
    }

    auto& record = found->second;
    if (record.advertisement.boot_generation != advertisement.boot_generation) {
        impl_->reclaim_node(advertisement.node, LanFallbackReason::NodeRestarted);
        record = {};
        record.advertisement = advertisement;
        record.last_seen_milliseconds = now_milliseconds;
        record.expires_at_milliseconds = expires_at;
        return LanAdvertisementStatus::Restarted;
    }
    if (advertisement.sequence <= record.advertisement.sequence)
        return LanAdvertisementStatus::Stale;

    record.advertisement = advertisement;
    record.last_seen_milliseconds = now_milliseconds;
    record.expires_at_milliseconds = expires_at;
    return LanAdvertisementStatus::Refreshed;
}

std::size_t LanAdministrator::expire(std::uint64_t now_milliseconds) {
    return impl_->expire(now_milliseconds);
}

std::vector<LanResourceView> LanAdministrator::resources(
    std::uint64_t now_milliseconds) const {
    std::vector<LanResourceView> result;
    result.reserve(impl_->nodes.size());
    for (const auto& [node, record] : impl_->nodes) {
        if (now_milliseconds >= record.expires_at_milliseconds) continue;
        const auto& advertisement = record.advertisement;
        const auto advertised_slots = advertisement.max_units_in_flight -
                                      advertisement.active_units;
        LanResourceView view;
        view.node = node;
        view.boot_generation = advertisement.boot_generation;
        view.sequence = advertisement.sequence;
        view.features = advertisement.features;
        view.trusted = impl_->trusted_nodes.contains(node);
        view.remaining_unit_slots = record.reserved_units >= advertised_slots
            ? 0
            : advertised_slots - record.reserved_units;
        view.estimated_rtt_microseconds = advertisement.estimated_rtt_microseconds;
        view.expires_at_milliseconds = record.expires_at_milliseconds;
        for (std::size_t coat = 0; coat < coat_count; ++coat) {
            const auto available = advertisement.available_coat_bytes[coat];
            const auto reserved = record.reserved_coat_bytes[coat];
            view.remaining_coat_bytes[coat] =
                reserved >= available ? 0 : available - reserved;
        }
        result.push_back(view);
    }
    std::sort(result.begin(), result.end(),
              [](const LanResourceView& left, const LanResourceView& right) {
                  return node_less(left.node, right.node);
              });
    return result;
}

std::optional<LanLease> LanAdministrator::lease(
    const LanWorkRequest& request,
    std::uint64_t now_milliseconds) {
    if (!valid_request(request))
        throw std::invalid_argument("invalid LAN work request");
    impl_->expire(now_milliseconds);

    const auto required_features = request.required_features | feature_for(request.kind);
    Impl::NodeMap::iterator best = impl_->nodes.end();
    std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t> best_score{};

    for (auto found = impl_->nodes.begin(); found != impl_->nodes.end(); ++found) {
        const auto& node = found->first;
        const auto& record = found->second;
        const auto& advertisement = record.advertisement;
        if (!impl_->trusted_nodes.contains(node) ||
            now_milliseconds >= record.expires_at_milliseconds ||
            !supports(advertisement.features, required_features))
            continue;
        if (request.maximum_rtt_microseconds != 0 &&
            (advertisement.estimated_rtt_microseconds == 0 ||
             advertisement.estimated_rtt_microseconds >
                request.maximum_rtt_microseconds))
            continue;

        const auto occupied = static_cast<std::uint64_t>(advertisement.active_units) +
                              record.reserved_units;
        if (occupied >= advertisement.max_units_in_flight) continue;

        bool has_coats = true;
        for (std::size_t coat = 0; coat < coat_count; ++coat) {
            const auto available = advertisement.available_coat_bytes[coat];
            const auto reserved = record.reserved_coat_bytes[coat];
            if (reserved > available ||
                request.coat_bytes[coat] > available - reserved) {
                has_coats = false;
                break;
            }
        }
        if (!has_coats) continue;

        const auto load = occupied * 1'000'000u /
                          advertisement.max_units_in_flight;
        const auto rtt = advertisement.estimated_rtt_microseconds == 0
            ? std::numeric_limits<std::uint32_t>::max()
            : advertisement.estimated_rtt_microseconds;
        const auto score = std::tuple{
            load, static_cast<std::uint64_t>(rtt), node.high, node.low};
        if (best == impl_->nodes.end() || score < best_score) {
            best = found;
            best_score = score;
        }
    }

    if (best == impl_->nodes.end()) return std::nullopt;
    auto& resource = best->second;
    const auto requested_expiry = saturating_add(
        now_milliseconds, request.lease_milliseconds);
    const auto expires_at = std::min(requested_expiry,
                                     resource.expires_at_milliseconds);
    if (expires_at <= now_milliseconds) return std::nullopt;

    std::uint64_t lease_id = impl_->next_lease_id++;
    if (lease_id == 0) lease_id = impl_->next_lease_id++;
    LanLease lease;
    lease.lease_id = lease_id;
    lease.node = best->first;
    lease.kind = request.kind;
    lease.boot_generation = resource.advertisement.boot_generation;
    lease.frame_generation = request.frame_generation;
    lease.seed_generation = request.seed_generation;
    lease.unit_x = request.unit_x;
    lease.unit_y = request.unit_y;
    lease.input_digest = request.input_digest;
    lease.required_features = required_features;
    lease.coat_bytes = request.coat_bytes;
    lease.required_proofs = request.required_proofs;
    lease.maximum_error = request.maximum_error;
    lease.expires_at_milliseconds = expires_at;

    ++resource.reserved_units;
    for (std::size_t coat = 0; coat < coat_count; ++coat)
        resource.reserved_coat_bytes[coat] += request.coat_bytes[coat];
    impl_->leases.emplace(lease_id, Impl::LeaseRecord{lease, request});
    return lease;
}

LanResultStatus LanAdministrator::submit(
    const LanVerifiedResult& result,
    std::uint64_t now_milliseconds) {
    if (impl_->completed_leases.contains(result.lease_id))
        return LanResultStatus::Duplicate;
    const auto found = impl_->leases.find(result.lease_id);
    if (found == impl_->leases.end()) return LanResultStatus::UnknownLease;

    const auto& lease = found->second.lease;
    const auto& request = found->second.request;
    if (result.node != lease.node) return LanResultStatus::NodeMismatch;
    if (now_milliseconds >= lease.expires_at_milliseconds)
        return impl_->fail_lease(found, LanResultStatus::Expired,
                                 LanFallbackReason::LeaseExpired);
    if (result.boot_generation != lease.boot_generation)
        return impl_->fail_lease(found, LanResultStatus::BootMismatch,
                                 LanFallbackReason::BootMismatch);
    if (result.frame_generation != lease.frame_generation ||
        result.seed_generation != lease.seed_generation ||
        result.unit_x != lease.unit_x || result.unit_y != lease.unit_y)
        return impl_->fail_lease(found, LanResultStatus::GenerationMismatch,
                                 LanFallbackReason::GenerationMismatch);
    if (result.input_digest != lease.input_digest ||
        !valid_lan_digest(result.output_digest))
        return impl_->fail_lease(found, LanResultStatus::DigestMismatch,
                                 LanFallbackReason::DigestMismatch);
    if ((result.proofs & request.required_proofs) != request.required_proofs)
        return impl_->fail_lease(found, LanResultStatus::ProofIncomplete,
                                 LanFallbackReason::ProofIncomplete);
    if (result.unresolved_pixels != 0)
        return impl_->fail_lease(found, LanResultStatus::Unresolved,
                                 LanFallbackReason::Unresolved);
    if (!std::isfinite(result.maximum_error) || result.maximum_error < 0.0F ||
        result.maximum_error > request.maximum_error)
        return impl_->fail_lease(found, LanResultStatus::ErrorExceeded,
                                 LanFallbackReason::ErrorExceeded);

    const auto record = found->second;
    impl_->release_reservation(record);
    impl_->remember_completion(lease.lease_id);
    impl_->leases.erase(found);
    return LanResultStatus::Accepted;
}

std::vector<LanFallbackWork> LanAdministrator::drain_fallbacks() {
    auto result = std::move(impl_->fallbacks);
    impl_->fallbacks.clear();
    return result;
}

std::size_t LanAdministrator::active_lease_count() const noexcept {
    return impl_ ? impl_->leases.size() : 0;
}

LanFeatures feature_for(LanWorkKind kind) noexcept {
    switch (kind) {
        case LanWorkKind::ClassifyMassiveUnit:
            return static_cast<LanFeatures>(LanFeature::MassiveClassification);
        case LanWorkKind::SelectiveRefine:
            return static_cast<LanFeatures>(LanFeature::SelectiveRefine);
        case LanWorkKind::NativeRefine:
            return static_cast<LanFeatures>(LanFeature::NativeRefine);
    }
    return 0;
}

std::string_view to_string(LanAdvertisementStatus status) noexcept {
    switch (status) {
        case LanAdvertisementStatus::Accepted: return "accepted";
        case LanAdvertisementStatus::Refreshed: return "refreshed";
        case LanAdvertisementStatus::Restarted: return "restarted";
        case LanAdvertisementStatus::Stale: return "stale";
        case LanAdvertisementStatus::Invalid: return "invalid";
    }
    return "unknown";
}

std::string_view to_string(LanResultStatus status) noexcept {
    switch (status) {
        case LanResultStatus::Accepted: return "accepted";
        case LanResultStatus::UnknownLease: return "unknown-lease";
        case LanResultStatus::Duplicate: return "duplicate";
        case LanResultStatus::NodeMismatch: return "node-mismatch";
        case LanResultStatus::BootMismatch: return "boot-mismatch";
        case LanResultStatus::GenerationMismatch: return "generation-mismatch";
        case LanResultStatus::DigestMismatch: return "digest-mismatch";
        case LanResultStatus::ProofIncomplete: return "proof-incomplete";
        case LanResultStatus::Unresolved: return "unresolved";
        case LanResultStatus::ErrorExceeded: return "error-exceeded";
        case LanResultStatus::Expired: return "expired";
    }
    return "unknown";
}

std::string_view to_string(LanFallbackReason reason) noexcept {
    switch (reason) {
        case LanFallbackReason::LeaseExpired: return "lease-expired";
        case LanFallbackReason::NodeExpired: return "node-expired";
        case LanFallbackReason::NodeRestarted: return "node-restarted";
        case LanFallbackReason::TrustRevoked: return "trust-revoked";
        case LanFallbackReason::BootMismatch: return "boot-mismatch";
        case LanFallbackReason::GenerationMismatch: return "generation-mismatch";
        case LanFallbackReason::DigestMismatch: return "digest-mismatch";
        case LanFallbackReason::ProofIncomplete: return "proof-incomplete";
        case LanFallbackReason::Unresolved: return "unresolved";
        case LanFallbackReason::ErrorExceeded: return "error-exceeded";
    }
    return "unknown";
}

} // namespace ein
