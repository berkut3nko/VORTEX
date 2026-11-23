module;

// LocalInterfaces: getifaddrs on Linux, GetAdaptersAddresses on Windows (rendezvous.md "The engine
// side"). Only this file and Enet.cpp see platform socket headers.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

module vortex.net;

namespace vortex::net {

    std::string FormatIpv4(uint32_t ip) {
        return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 0xFF) + "." + std::to_string((ip >> 8) & 0xFF) + "." +
               std::to_string(ip & 0xFF);
    }

    std::string FormatEndpoint(Endpoint e) { return FormatIpv4(e.ip) + ":" + std::to_string(e.port); }

    bool ParseIpv4(std::string_view text, uint32_t& ip) {
        uint32_t value = 0;
        for (int part = 0; part < 4; ++part) {
            if (part > 0) {
                if (text.empty() || text.front() != '.') return false;
                text.remove_prefix(1);
            }
            size_t digits = 0;
            while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') ++digits;
            if (digits == 0 || digits > 3) return false;
            unsigned byte = 0;
            std::from_chars(text.data(), text.data() + digits, byte);
            if (byte > 255) return false;
            value = value << 8 | byte;
            text.remove_prefix(digits);
        }
        if (!text.empty()) return false;
        ip = value;
        return true;
    }

    bool ParseIpv4Endpoint(std::string_view text, Endpoint& out) {
        const size_t colon = text.rfind(':');
        if (colon == std::string_view::npos) return false;
        uint32_t ip = 0;
        if (!ParseIpv4(text.substr(0, colon), ip)) return false;
        const std::string_view digits = text.substr(colon + 1);
        if (digits.empty() || digits.size() > 5 || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return false;
        unsigned port = 0;
        std::from_chars(digits.data(), digits.data() + digits.size(), port);
        if (port < 1 || port > 65535) return false;
        out = {ip, static_cast<uint16_t>(port)};
        return true;
    }

    bool IsPrivateIpv4(uint32_t ip) {
        return (ip >> 24) == 10 || (ip >> 20) == ((172u << 4) | 1u) || (ip >> 16) == ((192u << 8) | 168u);
    }

    bool IsLanInterface(const LocalInterface& i) {
        if (!IsPrivateIpv4(i.ip)) return false;   // excludes loopback, link-local 169.254/16 and 100.64/10 too
        std::string name = i.name;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // Docker, bridges for virtual machines, VPNs (rendezvous.md: "docker or virtual adapters").
        for (const char* prefix : {"docker", "br-", "veth", "virbr", "vmnet", "vboxnet", "tun", "tap", "wg", "zt", "tailscale", "lxc", "lxd", "vnet", "utun"})
            if (name.rfind(prefix, 0) == 0) return false;
        for (const char* part : {"virtualbox", "vmware", "hyper-v", "vethernet", "wsl", "zerotier", "tailscale", "openvpn", "wireguard", "loopback", "tap-windows", "vpn", "npcap"})
            if (name.find(part) != std::string::npos) return false;
        return true;
    }

    bool InSubnetOf(uint32_t ip, std::span<const LocalInterface> interfaces) {
        for (const LocalInterface& i : interfaces)
            if (i.mask != 0 && (ip & i.mask) == (i.ip & i.mask)) return true;
        return false;
    }

#if defined(_WIN32)
    std::vector<LocalInterface> LocalInterfaces() {
        std::vector<LocalInterface> out;
        ULONG size = 16 * 1024;
        std::vector<unsigned char> buffer;
        ULONG result = ERROR_BUFFER_OVERFLOW;
        for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
            buffer.resize(size);
            result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                          reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
        }
        if (result != NO_ERROR) return out;
        for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a != nullptr; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
            std::string name;
            if (a->FriendlyName != nullptr)
                for (const wchar_t* c = a->FriendlyName; *c != 0; ++c) name += *c < 128 ? static_cast<char>(*c) : '?';
            if (a->Description != nullptr) {
                name += " / ";
                for (const wchar_t* c = a->Description; *c != 0; ++c) name += *c < 128 ? static_cast<char>(*c) : '?';
            }
            for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
                if (u->Address.lpSockaddr == nullptr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
                const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
                LocalInterface i;
                i.name = name;
                i.ip = ntohl(sin->sin_addr.s_addr);
                const unsigned prefix = std::min<unsigned>(u->OnLinkPrefixLength, 32);
                i.mask = prefix == 0 ? 0u : ~0u << (32 - prefix);
                out.push_back(std::move(i));
            }
        }
        return out;
    }
#else
    std::vector<LocalInterface> LocalInterfaces() {
        std::vector<LocalInterface> out;
        ifaddrs* list = nullptr;
        if (getifaddrs(&list) != 0) return out;
        for (const ifaddrs* a = list; a != nullptr; a = a->ifa_next) {
            if (a->ifa_addr == nullptr || a->ifa_addr->sa_family != AF_INET) continue;
            if ((a->ifa_flags & IFF_UP) == 0 || (a->ifa_flags & IFF_LOOPBACK) != 0) continue;
            LocalInterface i;
            i.name = a->ifa_name != nullptr ? a->ifa_name : "";
            i.ip = ntohl(reinterpret_cast<const sockaddr_in*>(a->ifa_addr)->sin_addr.s_addr);
            i.mask = a->ifa_netmask != nullptr ? ntohl(reinterpret_cast<const sockaddr_in*>(a->ifa_netmask)->sin_addr.s_addr) : 0u;
            out.push_back(std::move(i));
        }
        freeifaddrs(list);
        return out;
    }
#endif
}
