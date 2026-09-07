#include "ein/lan_administrator.hpp"
#include "ein/lan_discovery.hpp"
#include "ein/lan_protocol.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

ein::LanDigest digest(std::uint8_t seed) {
    ein::LanDigest result{};
    for (std::size_t index = 0; index < result.size(); ++index)
        result[index] = static_cast<std::uint8_t>(seed + index);
    return result;
}

ein::LanAdvertisement advertisement(ein::LanNodeId node = {0xA11CEu, 0xF00Du},
                                    std::uint64_t boot = 7,
                                    std::uint64_t sequence = 1) {
    ein::LanAdvertisement result;
    result.node = node;
    result.boot_generation = boot;
    result.sequence = sequence;
    result.ttl_milliseconds = 1000;
    result.estimated_rtt_microseconds = 700;
    result.max_units_in_flight = 4;
    result.active_units = 1;
    result.features = ein::LanFeature::MassiveClassification |
                      ein::LanFeature::SelectiveRefine |
                      ein::LanFeature::NativeRefine |
                      ein::LanFeature::IntegerIdentity |
                      ein::LanFeature::ProofReadback;
    for (std::size_t coat = 0; coat < result.available_coat_bytes.size(); ++coat)
        result.available_coat_bytes[coat] =
            (coat + 1u) * 1024u * 1024u;
    return result;
}

ein::LanWorkRequest work_request(std::uint64_t frame = 41,
                                 std::uint32_t seed = 9) {
    ein::LanWorkRequest request;
    request.kind = ein::LanWorkKind::ClassifyMassiveUnit;
    request.frame_generation = frame;
    request.seed_generation = seed;
    request.unit_x = 3;
    request.unit_y = 2;
    request.input_digest = digest(11);
    request.required_features =
        static_cast<ein::LanFeatures>(ein::LanFeature::IntegerIdentity) |
        ein::LanFeature::ProofReadback;
    request.coat_bytes[static_cast<std::size_t>(ein::Coat::Frame)] = 4096;
    request.coat_bytes[static_cast<std::size_t>(ein::Coat::Storage)] = 65536;
    request.coat_bytes[static_cast<std::size_t>(ein::Coat::Readback)] = 1024;
    request.lease_milliseconds = 50;
    request.maximum_rtt_microseconds = 2000;
    request.maximum_error = 1.0F / 255.0F;
    return request;
}

ein::LanVerifiedResult completed_result(const ein::LanLease& lease) {
    ein::LanVerifiedResult result;
    result.lease_id = lease.lease_id;
    result.node = lease.node;
    result.boot_generation = lease.boot_generation;
    result.frame_generation = lease.frame_generation;
    result.seed_generation = lease.seed_generation;
    result.unit_x = lease.unit_x;
    result.unit_y = lease.unit_y;
    result.input_digest = lease.input_digest;
    result.output_digest = digest(97);
    result.proofs = ein::LanProof::Identity |
                    ein::LanProof::Depth |
                    ein::LanProof::Motion;
    result.maximum_error = 0.001F;
    return result;
}

void test_wire_codec_rejects_corruption() {
    const auto source = advertisement(
        {0x0102030405060708ull, 0x1112131415161718ull});
    auto packet = ein::encode_lan_advertisement(source);
    expect(packet.size() == ein::lan_advertisement_packet_bytes,
           "advertisement packet size drifted");
    const auto decoded = ein::decode_lan_advertisement(packet);
    expect(decoded && *decoded.advertisement == source,
           "advertisement did not survive network encoding");
    expect(std::to_integer<unsigned>(packet[16]) == 0x01u &&
               std::to_integer<unsigned>(packet[23]) == 0x08u,
           "advertisement integers are not in network byte order");

    packet[31] ^= std::byte{0x01};
    const auto corrupted = ein::decode_lan_advertisement(packet);
    expect(!corrupted && corrupted.error == ein::LanDecodeError::Checksum,
           "corrupted discovery packet passed its checksum");
    expect(ein::to_string(source.node).size() == 32,
           "node identity is not a fixed 128-bit string");
}

void test_scheduler_prefers_lower_load() {
    ein::LanAdministrator administrator;
    auto busy = advertisement({1, 1});
    busy.active_units = 3;
    busy.estimated_rtt_microseconds = 100;
    auto open = advertisement({2, 2});
    open.active_units = 0;
    open.estimated_rtt_microseconds = 1000;
    administrator.set_trusted(busy.node, true);
    administrator.set_trusted(open.node, true);
    (void)administrator.observe(busy, 1500);
    (void)administrator.observe(open, 1500);
    const auto lease = administrator.lease(work_request(), 1501);
    expect(lease.has_value() && lease->node == open.node,
           "scheduler did not prefer the lower-utilization LAN node");
}

void test_udp_loopback_discovery() {
    ein::LanDiscoverySocket receiver;
    receiver.open(0, false);
    ein::LanDiscoverySocket sender;
    sender.open(0, false);
    sender.send_to(advertisement(), "127.0.0.1", receiver.bound_port());
    const auto datagram = receiver.poll(std::chrono::milliseconds{500});
    expect(datagram.has_value(), "LAN discovery loopback timed out");
    expect(datagram->error == ein::LanDecodeError::None &&
               datagram->advertisement == advertisement(),
           "LAN discovery loopback changed the advertisement");
    expect(datagram->source_ipv4 == "127.0.0.1",
           "LAN discovery did not retain its source address");
}

void test_discovery_is_not_authority() {
    ein::LanAdministrator administrator;
    const auto resource = advertisement();
    expect(administrator.observe(resource, 1000) ==
               ein::LanAdvertisementStatus::Accepted,
           "valid resource advertisement was rejected");
    expect(!administrator.lease(work_request(), 1001).has_value(),
           "an untrusted discovery packet authorized work");

    administrator.set_trusted(resource.node, true);
    const auto lease = administrator.lease(work_request(), 1002);
    expect(lease.has_value(), "trusted capacity did not receive a work lease");
    expect(ein::supports(resource.features, lease->required_features),
           "lease exceeded the node's feature bits");
    const auto views = administrator.resources(1002);
    expect(views.size() == 1 && views[0].trusted &&
               views[0].remaining_unit_slots == 2,
           "lease reservation did not reduce advertised capacity");

    auto wrong_node = completed_result(*lease);
    wrong_node.node.low += 1;
    expect(administrator.submit(wrong_node, 1003) ==
               ein::LanResultStatus::NodeMismatch,
           "result from the wrong node was accepted");
    expect(administrator.active_lease_count() == 1,
           "spoofed node result cancelled the real lease");

    auto oversized = work_request(42);
    const auto recovery = static_cast<std::size_t>(ein::Coat::Recovery);
    oversized.coat_bytes[recovery] =
        resource.available_coat_bytes[recovery] + 1;
    expect(!administrator.lease(oversized, 1004).has_value(),
           "lease exceeded the node's advertised recovery coat");
    expect(administrator.submit(completed_result(*lease), 1005) ==
               ein::LanResultStatus::Accepted,
           "real node could not complete a lease after a spoofed result");
}

void test_complete_result_is_idempotent() {
    ein::LanAdministrator administrator;
    const auto resource = advertisement();
    administrator.set_trusted(resource.node, true);
    (void)administrator.observe(resource, 2000);
    const auto lease = administrator.lease(work_request(), 2001);
    expect(lease.has_value(), "complete-result test could not create lease");
    const auto result = completed_result(*lease);
    expect(administrator.submit(result, 2002) == ein::LanResultStatus::Accepted,
           "complete proven result was rejected");
    expect(administrator.submit(result, 2003) == ein::LanResultStatus::Duplicate,
           "duplicate result was not idempotent");
    expect(administrator.active_lease_count() == 0 &&
               administrator.drain_fallbacks().empty(),
           "accepted result left work active or scheduled fallback");
}

void test_unproven_result_fails_closed() {
    ein::LanAdministrator administrator;
    const auto resource = advertisement();
    administrator.set_trusted(resource.node, true);
    (void)administrator.observe(resource, 3000);
    auto lease = administrator.lease(work_request(), 3001);
    expect(lease.has_value(), "unproven-result test could not create lease");
    auto result = completed_result(*lease);
    result.unresolved_pixels = 1;
    expect(administrator.submit(result, 3002) == ein::LanResultStatus::Unresolved,
           "unresolved LAN work was accepted");
    auto fallback = administrator.drain_fallbacks();
    expect(fallback.size() == 1 &&
               fallback[0].reason == ein::LanFallbackReason::Unresolved &&
               fallback[0].request.frame_generation == 41,
           "unresolved LAN work was not returned to the local path");

    lease = administrator.lease(work_request(42), 3003);
    result = completed_result(*lease);
    result.seed_generation += 1;
    expect(administrator.submit(result, 3004) ==
               ein::LanResultStatus::GenerationMismatch,
           "generation-mismatched LAN work was accepted");
    fallback = administrator.drain_fallbacks();
    expect(fallback.size() == 1 && fallback[0].reason ==
               ein::LanFallbackReason::GenerationMismatch,
           "generation mismatch did not schedule local fallback");

    lease = administrator.lease(work_request(43), 3005);
    result = completed_result(*lease);
    result.maximum_error = 0.1F;
    expect(administrator.submit(result, 3006) ==
               ein::LanResultStatus::ErrorExceeded,
           "out-of-bound LAN work was accepted");
    fallback = administrator.drain_fallbacks();
    expect(fallback.size() == 1 && fallback[0].reason ==
               ein::LanFallbackReason::ErrorExceeded,
           "error-bound failure did not schedule local fallback");

    lease = administrator.lease(work_request(44), 3007);
    result = completed_result(*lease);
    result.proofs = ein::LanProof::Identity | ein::LanProof::Depth;
    expect(administrator.submit(result, 3008) ==
               ein::LanResultStatus::ProofIncomplete,
           "incomplete locally verified proof was accepted");
    fallback = administrator.drain_fallbacks();
    expect(fallback.size() == 1 && fallback[0].reason ==
               ein::LanFallbackReason::ProofIncomplete,
           "missing proof did not schedule local fallback");

    lease = administrator.lease(work_request(45), 3009);
    result = completed_result(*lease);
    result.output_digest = {};
    expect(administrator.submit(result, 3010) ==
               ein::LanResultStatus::DigestMismatch,
           "missing locally computed output digest was accepted");
    fallback = administrator.drain_fallbacks();
    expect(fallback.size() == 1 && fallback[0].reason ==
               ein::LanFallbackReason::DigestMismatch,
           "digest failure did not schedule local fallback");
}

void test_expiry_restart_and_stale_sequence() {
    ein::LanAdministrator administrator;
    auto resource = advertisement();
    resource.ttl_milliseconds = 100;
    administrator.set_trusted(resource.node, true);
    (void)administrator.observe(resource, 4000);
    expect(administrator.observe(resource, 4090) ==
               ein::LanAdvertisementStatus::Stale,
           "duplicate sequence was treated as fresh capacity");
    expect(administrator.expire(4100) == 1,
           "stale packet incorrectly extended resource lifetime");

    resource.boot_generation += 1;
    resource.sequence = 1;
    resource.ttl_milliseconds = 1000;
    expect(administrator.observe(resource, 4200) ==
               ein::LanAdvertisementStatus::Accepted,
           "expired resource could not reappear after restart");
    const auto lease = administrator.lease(work_request(52), 4201);
    expect(lease.has_value(), "restart test could not create lease");
    resource.boot_generation += 1;
    resource.sequence = 1;
    expect(administrator.observe(resource, 4202) ==
               ein::LanAdvertisementStatus::Restarted,
           "new boot generation was not detected");
    const auto fallback = administrator.drain_fallbacks();
    expect(fallback.size() == 1 && fallback[0].reason ==
               ein::LanFallbackReason::NodeRestarted,
           "node restart did not reclaim its outstanding unit");

    const auto replacement = administrator.lease(work_request(53), 4203);
    expect(replacement.has_value(), "restarted trusted node did not regain capacity");
    administrator.set_trusted(resource.node, false);
    const auto revoked = administrator.drain_fallbacks();
    expect(revoked.size() == 1 && revoked[0].reason ==
               ein::LanFallbackReason::TrustRevoked,
           "trust revocation did not reclaim outstanding work");
}

void test_lease_timeout_returns_unit_locally() {
    ein::LanAdministrator administrator;
    const auto resource = advertisement();
    administrator.set_trusted(resource.node, true);
    (void)administrator.observe(resource, 5000);
    const auto lease = administrator.lease(work_request(61), 5001);
    expect(lease.has_value(), "timeout test could not create lease");
    (void)administrator.expire(lease->expires_at_milliseconds);
    const auto fallback = administrator.drain_fallbacks();
    expect(fallback.size() == 1 && fallback[0].reason ==
               ein::LanFallbackReason::LeaseExpired,
           "expired lease was not returned to local native work");
    expect(administrator.active_lease_count() == 0,
           "expired lease remained active");
}

} // namespace

int main() {
    try {
        test_wire_codec_rejects_corruption();
        test_udp_loopback_discovery();
        test_discovery_is_not_authority();
        test_scheduler_prefers_lower_load();
        test_complete_result_is_idempotent();
        test_unproven_result_fails_closed();
        test_expiry_restart_and_stale_sequence();
        test_lease_timeout_returns_unit_locally();
        std::cout << "all LAN administrator tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failure: " << error.what() << '\n';
        return 1;
    }
}
