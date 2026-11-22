module;

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module vortex.net:address;

// docs/design/rendezvous.md "The engine side": IPv4 endpoints as numbers, the datagrams STUN and the
// punch exchange (not ENet messages), and this machine's LAN interfaces (candidates of kind lan).

export namespace vortex::net {

    /// @brief An IPv4 address and UDP port, both in host byte order (ENet 1.3 is IPv4 only).
    struct Endpoint {
        uint32_t ip = 0;
        uint16_t port = 0;
        friend bool operator==(const Endpoint&, const Endpoint&) = default;
        friend auto operator<=>(const Endpoint&, const Endpoint&) = default;
    };

    constexpr uint32_t Ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
        return static_cast<uint32_t>(a) << 24 | static_cast<uint32_t>(b) << 16 | static_cast<uint32_t>(c) << 8 | d;
    }

    /// @brief One UDP datagram outside ENet's format: `address` is where it goes (sent) or where it came
    /// from (received).
    struct Datagram {
        Endpoint address;
        std::vector<uint8_t> bytes;
    };

    std::string FormatIpv4(uint32_t ip);
    /// @brief "a.b.c.d:port".
    std::string FormatEndpoint(Endpoint e);
    /// @brief A dotted IPv4 literal (four decimal parts 0-255); false otherwise.
    bool ParseIpv4(std::string_view text, uint32_t& ip);
    /// @brief "a.b.c.d:port" with a port 1-65535; false otherwise.
    bool ParseIpv4Endpoint(std::string_view text, Endpoint& out);

    /// @brief RFC 1918 (10/8, 172.16/12, 192.168/16): what a lan candidate may be.
    bool IsPrivateIpv4(uint32_t ip);

    /// @brief One IPv4 address of this machine with its netmask.
    struct LocalInterface {
        std::string name;
        uint32_t ip = 0;
        uint32_t mask = 0;
    };

    /// @brief Whether an interface gives a lan candidate: a private (RFC 1918) address that is not
    /// loopback, link-local or 100.64.0.0/10 (Tailscale, a carrier's NAT), on an adapter that is not a
    /// docker, VPN or virtual machine one (judged by its name).
    bool IsLanInterface(const LocalInterface& i);
    /// @brief Whether `ip` lies in the subnet of one of `interfaces`.
    bool InSubnetOf(uint32_t ip, std::span<const LocalInterface> interfaces);
    /// @brief The IPv4 addresses of this machine's interfaces that are up (getifaddrs on Linux,
    /// GetAdaptersAddresses on Windows); unfiltered (IsLanInterface picks the candidates).
    std::vector<LocalInterface> LocalInterfaces();
}
