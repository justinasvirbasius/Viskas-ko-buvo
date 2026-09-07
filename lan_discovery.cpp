#include "ein/lan_discovery.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ein {

namespace {
#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket invalid_socket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket invalid_socket = -1;
#endif

[[noreturn]] void throw_socket_error(std::string_view operation) {
#ifdef _WIN32
    const auto code = WSAGetLastError();
#else
    const auto code = errno;
#endif
    throw std::runtime_error(std::string(operation) + " failed (socket error " +
                             std::to_string(code) + ")");
}

void close_native_socket(NativeSocket socket) noexcept {
#ifdef _WIN32
    closesocket(socket);
#else
    ::close(socket);
#endif
}

const char* option_data(const void* value) noexcept {
    return static_cast<const char*>(value);
}
} // namespace

struct LanDiscoverySocket::Impl {
    NativeSocket socket{invalid_socket};
    std::uint16_t bound_port{};
#ifdef _WIN32
    bool winsock_started{};
#endif

    ~Impl() {
        if (socket != invalid_socket) close_native_socket(socket);
#ifdef _WIN32
        if (winsock_started) WSACleanup();
#endif
    }
};

LanDiscoverySocket::LanDiscoverySocket() : impl_(std::make_unique<Impl>()) {}

LanDiscoverySocket::~LanDiscoverySocket() {
    close();
}

LanDiscoverySocket::LanDiscoverySocket(LanDiscoverySocket&&) noexcept = default;
LanDiscoverySocket& LanDiscoverySocket::operator=(LanDiscoverySocket&&) noexcept = default;

void LanDiscoverySocket::open(std::uint16_t bind_port, bool join_multicast) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    close();

    try {
#ifdef _WIN32
        WSADATA winsock_data{};
        if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0)
            throw_socket_error("WSAStartup");
        impl_->winsock_started = true;
#endif
        impl_->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (impl_->socket == invalid_socket) throw_socket_error("socket");

        int reuse = 1;
        if (setsockopt(impl_->socket, SOL_SOCKET, SO_REUSEADDR,
                       option_data(&reuse), sizeof(reuse)) != 0)
            throw_socket_error("setsockopt(SO_REUSEADDR)");

        unsigned char multicast_ttl = 1;
        unsigned char multicast_loop = 1;
        if (setsockopt(impl_->socket, IPPROTO_IP, IP_MULTICAST_TTL,
                       option_data(&multicast_ttl), sizeof(multicast_ttl)) != 0)
            throw_socket_error("setsockopt(IP_MULTICAST_TTL)");
        if (setsockopt(impl_->socket, IPPROTO_IP, IP_MULTICAST_LOOP,
                       option_data(&multicast_loop), sizeof(multicast_loop)) != 0)
            throw_socket_error("setsockopt(IP_MULTICAST_LOOP)");

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(bind_port);
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(impl_->socket, reinterpret_cast<const sockaddr*>(&local),
                   sizeof(local)) != 0)
            throw_socket_error("bind");

        if (join_multicast) {
            ip_mreq membership{};
            const std::string multicast(lan_multicast_address);
            if (inet_pton(AF_INET, multicast.c_str(), &membership.imr_multiaddr) != 1)
                throw std::logic_error("invalid EIN LAN multicast address");
            membership.imr_interface.s_addr = htonl(INADDR_ANY);
            if (setsockopt(impl_->socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                           option_data(&membership), sizeof(membership)) != 0)
                throw_socket_error("setsockopt(IP_ADD_MEMBERSHIP)");
        }

        sockaddr_in actual{};
#ifdef _WIN32
        int actual_size = sizeof(actual);
#else
        socklen_t actual_size = sizeof(actual);
#endif
        if (getsockname(impl_->socket, reinterpret_cast<sockaddr*>(&actual),
                        &actual_size) != 0)
            throw_socket_error("getsockname");
        impl_->bound_port = ntohs(actual.sin_port);
    } catch (...) {
        close();
        throw;
    }
}

void LanDiscoverySocket::close() noexcept {
    if (!impl_) return;
    if (impl_->socket != invalid_socket) {
        close_native_socket(impl_->socket);
        impl_->socket = invalid_socket;
    }
    impl_->bound_port = 0;
#ifdef _WIN32
    if (impl_->winsock_started) {
        WSACleanup();
        impl_->winsock_started = false;
    }
#endif
}

bool LanDiscoverySocket::is_open() const noexcept {
    return impl_ && impl_->socket != invalid_socket;
}

std::uint16_t LanDiscoverySocket::bound_port() const noexcept {
    return impl_ ? impl_->bound_port : 0;
}

void LanDiscoverySocket::announce(const LanAdvertisement& advertisement) {
    send_to(advertisement, lan_multicast_address, lan_discovery_port);
}

void LanDiscoverySocket::send_to(const LanAdvertisement& advertisement,
                                 std::string_view ipv4,
                                 std::uint16_t port) {
    if (!is_open()) throw std::logic_error("LAN discovery socket is not open");
    const auto packet = encode_lan_advertisement(advertisement);

    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(port);
    const std::string address(ipv4);
    if (inet_pton(AF_INET, address.c_str(), &destination.sin_addr) != 1)
        throw std::invalid_argument("LAN discovery destination is not IPv4");

    const auto sent = ::sendto(
        impl_->socket,
        reinterpret_cast<const char*>(packet.data()),
        static_cast<int>(packet.size()),
        0,
        reinterpret_cast<const sockaddr*>(&destination),
        sizeof(destination));
    if (sent < 0 || static_cast<std::size_t>(sent) != packet.size())
        throw_socket_error("sendto");
}

std::optional<LanDatagram> LanDiscoverySocket::poll(
    std::chrono::milliseconds timeout) {
    if (!is_open()) throw std::logic_error("LAN discovery socket is not open");
    const auto total_milliseconds = std::clamp<std::int64_t>(
        timeout.count(), 0, 86'400'000);
    timeval wait{};
    wait.tv_sec = static_cast<long>(total_milliseconds / 1000);
    wait.tv_usec = static_cast<long>((total_milliseconds % 1000) * 1000);

    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(impl_->socket, &read_set);
#ifdef _WIN32
    const auto selected = select(0, &read_set, nullptr, nullptr, &wait);
#else
    const auto selected = select(impl_->socket + 1, &read_set, nullptr, nullptr, &wait);
#endif
    if (selected == 0) return std::nullopt;
    if (selected < 0) throw_socket_error("select");

    std::array<std::byte, 65'507> packet{};
    sockaddr_in source{};
#ifdef _WIN32
    int source_size = sizeof(source);
#else
    socklen_t source_size = sizeof(source);
#endif
    const auto received = ::recvfrom(
        impl_->socket,
        reinterpret_cast<char*>(packet.data()),
        static_cast<int>(packet.size()),
        0,
        reinterpret_cast<sockaddr*>(&source),
        &source_size);
    if (received < 0) throw_socket_error("recvfrom");

    std::array<char, INET_ADDRSTRLEN> source_text{};
    const char* converted = inet_ntop(AF_INET, &source.sin_addr,
                                      source_text.data(),
                                      static_cast<unsigned>(source_text.size()));
    const auto decoded = decode_lan_advertisement(
        std::span<const std::byte>(packet.data(), static_cast<std::size_t>(received)));
    return LanDatagram{
        decoded.advertisement,
        decoded.error,
        converted ? std::string(converted) : std::string{},
        ntohs(source.sin_port),
    };
}

} // namespace ein
