module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

export module vortex.net:stun;

import vortex.core;
import :address;

// docs/design/rendezvous.md "The engine side": an RFC 5389 Binding request from the ENet host's own
// socket tells it its outside address; two STUN servers on different IPs give the NAT class.

export namespace vortex::net {

    inline constexpr uint32_t kStunMagicCookie = 0x2112A442;
    inline constexpr size_t kStunHeaderBytes = 20;
    using StunTransaction = std::array<uint8_t, 12>;

    /// @brief A Binding request (20 bytes, no attributes).
    std::array<uint8_t, kStunHeaderBytes> MakeStunBindingRequest(const StunTransaction& transaction);
    /// @brief The header of a STUN message: at least 20 bytes, the two top bits 0, the magic cookie at
    /// offset 4. `transaction` gets its id.
    bool LooksLikeStun(std::span<const uint8_t> bytes, StunTransaction* transaction = nullptr);
    /// @brief Whether `bytes` is a Binding request (what a STUN server answers); `transaction` gets its id.
    bool ParseStunBindingRequest(std::span<const uint8_t> bytes, StunTransaction& transaction);
    /// @brief A Binding success response carrying XOR-MAPPED-ADDRESS (fake STUN servers in tests and
    /// R1's vortex_natbox).
    std::vector<uint8_t> MakeStunBindingResponse(const StunTransaction& transaction, Endpoint mapped);

    enum class StunResult : uint8_t { Ok, NotStun, WrongTransaction, NotSuccess, NoAddress, Malformed };
    /// @brief A Binding success response to `transaction`: the magic cookie, the transaction id, then
    /// XOR-MAPPED-ADDRESS (IPv4), or MAPPED-ADDRESS when the former is absent.
    StunResult ParseStunBindingResponse(std::span<const uint8_t> bytes, const StunTransaction& transaction, Endpoint& mapped);

    /// @brief The NAT class from two STUN servers on different IPs, asked from one socket.
    enum class NatClass : uint8_t {
        Unknown,     ///< no answer from one of them (or both): UDP blocked, the servers unreachable
        Eim,         ///< the same outside IP and port from both: endpoint-independent mapping
        Symmetric,   ///< a different IP or port: a new mapping per destination
    };
    const char* ToString(NatClass c);
    NatClass ClassifyNat(const std::optional<Endpoint>& first, const std::optional<Endpoint>& second);

    /**
     * @brief Asks STUN servers for this socket's outside address, again and again (rendezvous.md
     * "Keeping mappings"): a round every `refreshSeconds` (keeps a CGNAT's mapping open with nobody
     * connected), a retry after `retrySeconds` without an answer, `triesPerRound` tries. Sends nothing
     * itself: Update gives the datagrams, OnDatagram takes the answers. No socket, no clock.
     */
    class StunProbe {
    public:
        struct Settings {
            double retrySeconds = 2.0;
            double refreshSeconds = 15.0;
            uint32_t triesPerRound = 3;
        };

        StunProbe(std::vector<Endpoint> servers, uint64_t seed, Settings settings);
        StunProbe(std::vector<Endpoint> servers, uint64_t seed) : StunProbe(std::move(servers), seed, Settings{}) {}

        void Update(double now, std::vector<Datagram>& out);
        /// @brief true when `in` answered one of this probe's requests (it is then consumed).
        bool OnDatagram(const Datagram& in, double now);

        /// @brief The first round is over: every server answered or ran out of tries.
        bool Settled() const;
        /// @brief Each server's latest answer (nullopt: none yet).
        const std::vector<std::optional<Endpoint>>& Mapped() const { return m_Mapped; }
        /// @brief The first server's answer, else the second's ...
        std::optional<Endpoint> Outside() const;
        /// @brief From the first two servers' latest answers.
        NatClass Class() const;
        /// @brief true once after a later round saw another outside address than the one before.
        bool TakeChange();
        const std::vector<Endpoint>& Servers() const { return m_Servers; }

    private:
        struct Server {
            StunTransaction transaction{};
            double sentAt = -1.0;       ///< last send of this round
            double nextRound = 0.0;
            uint32_t tries = 0;
            bool waiting = false;       ///< a request of this round is unanswered
            bool settled = false;       ///< its first round is over
        };
        std::vector<Endpoint> m_Servers;
        std::vector<Server> m_State;
        std::vector<std::optional<Endpoint>> m_Mapped;
        Settings m_Settings;
        Rng m_Rng;
        bool m_Changed = false;
    };
}
