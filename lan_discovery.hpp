#pragma once

#include "ein/lan_protocol.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ein {

struct LanDatagram {
    std::optional<LanAdvertisement> advertisement;
    LanDecodeError error{LanDecodeError::None};
    std::string source_ipv4;
    std::uint16_t source_port{};
};

// UDP multicast is deliberately discovery-only. A received advertisement is
// never proof of identity and never grants permission to execute a work unit.
class LanDiscoverySocket {
public:
    LanDiscoverySocket();
    ~LanDiscoverySocket();

    LanDiscoverySocket(const LanDiscoverySocket&) = delete;
    LanDiscoverySocket& operator=(const LanDiscoverySocket&) = delete;
    LanDiscoverySocket(LanDiscoverySocket&&) noexcept;
    LanDiscoverySocket& operator=(LanDiscoverySocket&&) noexcept;

    void open(std::uint16_t bind_port = lan_discovery_port,
              bool join_multicast = true);
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::uint16_t bound_port() const noexcept;

    void announce(const LanAdvertisement& advertisement);
    void send_to(const LanAdvertisement& advertisement,
                 std::string_view ipv4,
                 std::uint16_t port);
    [[nodiscard]] std::optional<LanDatagram> poll(
        std::chrono::milliseconds timeout);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ein
