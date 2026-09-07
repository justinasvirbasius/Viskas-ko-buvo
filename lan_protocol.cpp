#include "ein/lan_protocol.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace ein {

namespace {
constexpr std::array<std::byte, 4> packet_magic{
    std::byte{'E'}, std::byte{'I'}, std::byte{'N'}, std::byte{'L'}};
constexpr std::uint16_t advertisement_packet_type = 1;
constexpr std::size_t header_bytes = 16;
constexpr std::size_t payload_bytes = lan_advertisement_packet_bytes - header_bytes;
constexpr std::size_t checksum_offset = 12;

void append_u16(std::vector<std::byte>& output, std::uint16_t value) {
    output.push_back(static_cast<std::byte>((value >> 8u) & 0xFFu));
    output.push_back(static_cast<std::byte>(value & 0xFFu));
}

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        output.push_back(static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFu));
}

void append_u64(std::vector<std::byte>& output, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        output.push_back(static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFu));
}

std::uint16_t read_u16(std::span<const std::byte> input, std::size_t& cursor) {
    const auto result = static_cast<std::uint16_t>(
        (std::to_integer<std::uint16_t>(input[cursor]) << 8u) |
        std::to_integer<std::uint16_t>(input[cursor + 1]));
    cursor += 2;
    return result;
}

std::uint32_t read_u32(std::span<const std::byte> input, std::size_t& cursor) {
    std::uint32_t result{};
    for (int i = 0; i < 4; ++i)
        result = (result << 8u) | std::to_integer<std::uint32_t>(input[cursor++]);
    return result;
}

std::uint64_t read_u64(std::span<const std::byte> input, std::size_t& cursor) {
    std::uint64_t result{};
    for (int i = 0; i < 8; ++i)
        result = (result << 8u) | std::to_integer<std::uint64_t>(input[cursor++]);
    return result;
}

std::uint32_t packet_crc32(std::span<const std::byte> packet) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t index = 0; index < packet.size(); ++index) {
        const auto byte = index >= checksum_offset && index < checksum_offset + 4
            ? 0u
            : std::to_integer<std::uint32_t>(packet[index]);
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1u) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void overwrite_u32(std::vector<std::byte>& output, std::size_t offset,
                   std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        output[offset++] = static_cast<std::byte>(
            (value >> static_cast<unsigned>(shift)) & 0xFFu);
}
} // namespace

bool valid_lan_advertisement(const LanAdvertisement& advertisement) noexcept {
    return advertisement.node.valid() &&
           advertisement.boot_generation != 0 &&
           advertisement.sequence != 0 &&
           advertisement.ttl_milliseconds >= 100 &&
           advertisement.ttl_milliseconds <= 60'000 &&
           advertisement.max_units_in_flight != 0 &&
           advertisement.active_units <= advertisement.max_units_in_flight &&
           advertisement.features != 0;
}

bool valid_lan_digest(const LanDigest& digest) noexcept {
    return std::any_of(digest.begin(), digest.end(),
                       [](std::uint8_t value) { return value != 0; });
}

std::vector<std::byte> encode_lan_advertisement(
    const LanAdvertisement& advertisement) {
    if (!valid_lan_advertisement(advertisement))
        throw std::invalid_argument("invalid LAN advertisement");

    std::vector<std::byte> output;
    output.reserve(lan_advertisement_packet_bytes);
    output.insert(output.end(), packet_magic.begin(), packet_magic.end());
    append_u16(output, lan_protocol_version);
    append_u16(output, advertisement_packet_type);
    append_u32(output, static_cast<std::uint32_t>(payload_bytes));
    append_u32(output, 0);

    append_u64(output, advertisement.node.high);
    append_u64(output, advertisement.node.low);
    append_u64(output, advertisement.boot_generation);
    append_u64(output, advertisement.sequence);
    append_u32(output, advertisement.ttl_milliseconds);
    append_u32(output, advertisement.estimated_rtt_microseconds);
    append_u32(output, advertisement.max_units_in_flight);
    append_u32(output, advertisement.active_units);
    append_u64(output, advertisement.features);
    for (const auto bytes : advertisement.available_coat_bytes)
        append_u64(output, bytes);

    if (output.size() != lan_advertisement_packet_bytes)
        throw std::logic_error("LAN advertisement packet layout drifted");
    overwrite_u32(output, checksum_offset, packet_crc32(output));
    return output;
}

LanDecodeResult decode_lan_advertisement(
    std::span<const std::byte> packet) noexcept {
    if (packet.size() != lan_advertisement_packet_bytes)
        return {std::nullopt, LanDecodeError::PacketSize};
    if (!std::equal(packet_magic.begin(), packet_magic.end(), packet.begin()))
        return {std::nullopt, LanDecodeError::Magic};

    std::size_t cursor = packet_magic.size();
    if (read_u16(packet, cursor) != lan_protocol_version)
        return {std::nullopt, LanDecodeError::Version};
    if (read_u16(packet, cursor) != advertisement_packet_type)
        return {std::nullopt, LanDecodeError::Type};
    if (read_u32(packet, cursor) != payload_bytes)
        return {std::nullopt, LanDecodeError::PayloadLength};
    const auto expected_crc = read_u32(packet, cursor);
    if (expected_crc != packet_crc32(packet))
        return {std::nullopt, LanDecodeError::Checksum};

    LanAdvertisement advertisement;
    advertisement.node.high = read_u64(packet, cursor);
    advertisement.node.low = read_u64(packet, cursor);
    advertisement.boot_generation = read_u64(packet, cursor);
    advertisement.sequence = read_u64(packet, cursor);
    advertisement.ttl_milliseconds = read_u32(packet, cursor);
    advertisement.estimated_rtt_microseconds = read_u32(packet, cursor);
    advertisement.max_units_in_flight = read_u32(packet, cursor);
    advertisement.active_units = read_u32(packet, cursor);
    advertisement.features = read_u64(packet, cursor);
    for (auto& bytes : advertisement.available_coat_bytes)
        bytes = read_u64(packet, cursor);

    if (cursor != packet.size() || !valid_lan_advertisement(advertisement))
        return {std::nullopt, LanDecodeError::InvalidAdvertisement};
    return {advertisement, LanDecodeError::None};
}

std::string to_string(LanNodeId node) {
    std::ostringstream output;
    output << std::hex << std::setfill('0')
           << std::setw(16) << node.high
           << std::setw(16) << node.low;
    return output.str();
}

std::string_view to_string(LanDecodeError error) noexcept {
    switch (error) {
        case LanDecodeError::None: return "none";
        case LanDecodeError::PacketSize: return "packet-size";
        case LanDecodeError::Magic: return "magic";
        case LanDecodeError::Version: return "version";
        case LanDecodeError::Type: return "type";
        case LanDecodeError::PayloadLength: return "payload-length";
        case LanDecodeError::Checksum: return "checksum";
        case LanDecodeError::InvalidAdvertisement: return "invalid-advertisement";
    }
    return "unknown";
}

} // namespace ein
