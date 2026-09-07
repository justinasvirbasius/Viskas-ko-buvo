#pragma once

#include "ein/graphics_shell.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ein {

inline constexpr std::uint16_t lan_protocol_version = 1;
inline constexpr std::uint16_t lan_discovery_port = 46973;
inline constexpr std::string_view lan_multicast_address = "239.255.69.73";
inline constexpr std::size_t lan_advertisement_packet_bytes = 176;

struct LanNodeId {
    std::uint64_t high{};
    std::uint64_t low{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return high != 0 || low != 0;
    }

    friend constexpr bool operator==(LanNodeId, LanNodeId) noexcept = default;
};

using LanFeatures = std::uint64_t;

enum class LanFeature : LanFeatures {
    MassiveClassification = LanFeatures{1} << 0,
    SelectiveRefine = LanFeatures{1} << 1,
    NativeRefine = LanFeatures{1} << 2,
    Glsl460 = LanFeatures{1} << 3,
    IntegerIdentity = LanFeatures{1} << 4,
    ProofReadback = LanFeatures{1} << 5,
};

constexpr LanFeatures operator|(LanFeature left, LanFeature right) noexcept {
    return static_cast<LanFeatures>(left) | static_cast<LanFeatures>(right);
}

constexpr LanFeatures operator|(LanFeatures left, LanFeature right) noexcept {
    return left | static_cast<LanFeatures>(right);
}

[[nodiscard]] constexpr bool supports(LanFeatures available,
                                      LanFeatures required) noexcept {
    return (available & required) == required;
}

using LanDigest = std::array<std::uint8_t, 32>;

struct LanAdvertisement {
    LanNodeId node;
    std::uint64_t boot_generation{};
    std::uint64_t sequence{};
    std::uint32_t ttl_milliseconds{3000};
    std::uint32_t estimated_rtt_microseconds{};
    std::uint32_t max_units_in_flight{};
    std::uint32_t active_units{};
    LanFeatures features{};
    std::array<std::uint64_t, coat_count> available_coat_bytes{};

    friend bool operator==(const LanAdvertisement&,
                           const LanAdvertisement&) noexcept = default;
};

enum class LanDecodeError : std::uint8_t {
    None,
    PacketSize,
    Magic,
    Version,
    Type,
    PayloadLength,
    Checksum,
    InvalidAdvertisement,
};

struct LanDecodeResult {
    std::optional<LanAdvertisement> advertisement;
    LanDecodeError error{LanDecodeError::None};

    [[nodiscard]] explicit operator bool() const noexcept {
        return advertisement.has_value();
    }
};

[[nodiscard]] bool valid_lan_advertisement(
    const LanAdvertisement& advertisement) noexcept;
[[nodiscard]] bool valid_lan_digest(const LanDigest& digest) noexcept;
[[nodiscard]] std::vector<std::byte> encode_lan_advertisement(
    const LanAdvertisement& advertisement);
[[nodiscard]] LanDecodeResult decode_lan_advertisement(
    std::span<const std::byte> packet) noexcept;
[[nodiscard]] std::string to_string(LanNodeId node);
[[nodiscard]] std::string_view to_string(LanDecodeError error) noexcept;

} // namespace ein
