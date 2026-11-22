module;

#include <cstdint>

export module vortex.net:punch;

import :traversal;

// docs/design/rendezvous.md stage [nat-R0]: the NAT probe and the pair test as programs run them, over
// an EnetTransport's own socket (the one ENet then uses). Only a person runs these against real STUN
// servers and hosts; tests drive PunchTest over NatSim instead.

export namespace vortex::net {

    /// @brief The two STUN servers asked when no --stun is given (on different IPs: the NAT class).
    inline constexpr const char* kDefaultStunServers[] = {"stun.cloudflare.com:3478", "stun.l.google.com:19302"};

    /// @brief Whether the command line asks for `--nat-probe` or `--punch-test`.
    bool WantsNatTool(int argc, const char* const* argv);

    /**
     * @brief Runs `--nat-probe` or `--punch-test` and returns the exit code (0 done / reached and ENet
     * connected; 1 failed; 2 a bad command line). The role is fixed by the program: vortex_server is
     * the server side, the game the client side.
     * @details Flags: `--token HEX` (the pair test), `--stun HOST[:PORT]` (repeatable; replaces the two
     * default servers), `--candidates lan,stun,given`, `--bind ADDR`; the server side also `--listen
     * PORT` (default: any free port) and `--public ADDR:PORT` (a `given` candidate); the client side
     * `--port PORT` (default: any). Logs `NAT:` and `Punch:` lines (docs/verify.md). After a reach the
     * client connects ENet to the chosen address and the server takes that connection: the punched
     * path carries ENet too.
     */
    int RunNatTool(int argc, const char* const* argv, PunchRole role);
}
