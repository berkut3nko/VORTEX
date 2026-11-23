module;

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module vortex.net:rendezvous;

import :address;
import :stun;
import :messages;
import :traversal;
import :https;
import :enet;

// docs/design/rendezvous.md [nat-R1]: the client of the Worker (Rendezvous: the server's room;
// RendezvousJoin: one client join) over an HttpsClient, and the runners the programs call
// (RendezvousHost: vortex_server --rendezvous; JoinThroughRendezvous: Shredborne --join).

export namespace vortex::net {

    /// @brief 32 lower-case hex digits of RoomKey(token) / HostKey(token); 16 of a build hash.
    std::string RoomHex(const Token& token);
    std::string HostKeyHex(const Token& token);
    std::string BuildHex(uint64_t buildHash);
    /// @brief "https://..." or "http://127.0.0.1:PORT" / "http://localhost:PORT"; no trailing '/', no white
    /// space, at most 200 characters.
    bool ValidRendezvousUrl(std::string_view url);
    /// @brief The first line of dir/rendezvous.url, trimmed, when ValidRendezvousUrl; else nullopt and `error`.
    std::optional<std::string> ReadRendezvousUrlFile(const std::filesystem::path& dir, std::string& error);

    struct RendezvousSettings {
        std::string url;                       ///< ValidRendezvousUrl
        Token token{};
        uint64_t buildHash = 0;                ///< net::BuildHash / Server::BuildHashValue
        uint16_t protocol = kProtocolVersion;
    };
    inline constexpr uint32_t kRendezvousShortTimeoutMs = 10000;   ///< register, join
    inline constexpr uint32_t kRendezvousWaitTimeoutMs = 30000;    ///< the long poll (the Worker holds it 20 s)
    inline constexpr uint32_t kRendezvousDeleteTimeoutMs = 2000;   ///< the DELETE on exit
    inline constexpr double kRendezvousMaxBackoffSeconds = 20.0;

    enum class RendezvousState : uint8_t { Idle, Registering, Waiting, BackingOff, Closing, Closed };
    /// @brief "idle", "registering", "waiting", "backing off", "closing", "closed".
    const char* ToString(RendezvousState s);

    /// @brief The server's room: registers, long-polls for joins, registers again when its candidates or NAT
    /// class change, backs off on errors, deletes the room on Close. Never blocks: Update starts requests and
    /// takes answers.
    class Rendezvous {
    public:
        Rendezvous(HttpsClient& https, RendezvousSettings settings);
        /// @brief Every server pass once the first STUN round settled. `candidates`: this server's (ChooseCandidates).
        void Update(double now, std::span<const Candidate> candidates, NatClass nat);
        /// @brief Joins the long poll brought since the last call, oldest first.
        std::vector<ServerJoin> TakeJoins();
        /// @brief Cancels the long poll and starts the DELETE (2 s limit). Update then only takes its answer.
        void Close(double now);
        RendezvousState State() const { return m_State; }
        bool QuotaSpent() const { return m_QuotaLogged; }
        uint32_t Failures() const { return m_Failures; }
        std::vector<std::string> TakeLog();

    private:
        void Fail(double now, const HttpsResponse& r, std::string_view what);

        HttpsClient& m_Https;
        RendezvousSettings m_Settings;
        std::string m_Room, m_Auth;               ///< RoomHex, "Bearer " + HostKeyHex
        RendezvousState m_State = RendezvousState::Idle;
        std::string m_Registered, m_Pending;      ///< "<cands> <nat>" registered / in flight
        size_t m_PendingCount = 0;
        HttpsRequestId m_Post = 0, m_Wait = 0, m_Delete = 0;
        uint32_t m_After = 0, m_Failures = 0;
        double m_NextTry = 0.0;
        bool m_QuotaLogged = false, m_EverRegistered = false;
        std::vector<ServerJoin> m_Joins;
        std::vector<std::string> m_Log;
    };

    enum class JoinFailure : uint8_t { None, NoRoom, AnotherBuild, IpMismatch, NonceUsed, TooManyJoins, Network, BadReply };
    /// @brief The client's words: "no server has this room (is it running with --rendezvous, and is the token
    /// right?)", "the server runs another build", "the Worker saw another outside address than STUN (a VPN?)",
    /// "the rendezvous refused the join's nonce", "too many joins to this room in a minute", "the rendezvous
    /// cannot be reached", "the rendezvous answered something unexpected"; None: "none".
    const char* ToString(JoinFailure f);

    /// @brief One client join: POST /v1/join, retried after 1 and 2 s when it got no HTTP answer.
    class RendezvousJoin {
    public:
        /// @brief `candidates`: this client's lan and stun ones (the rest are dropped, at most 2 stun); `nonce` non-zero.
        RendezvousJoin(HttpsClient& https, RendezvousSettings settings, std::span<const Candidate> candidates, uint64_t nonce, double now);
        void Update(double now);
        bool Done() const { return m_Done; }
        /// @brief Done without a failure: what ClientTraversal starts from.
        const std::optional<ClientJoinSetup>& Result() const { return m_Result; }
        uint32_t JoinId() const { return m_JoinId; }
        JoinFailure Failure() const { return m_Failure; }
        std::vector<std::string> TakeLog();

    private:
        HttpsClient& m_Https;
        RendezvousSettings m_Settings;
        std::string m_Body, m_Posted;             ///< the request body; the candidate string as posted
        uint64_t m_Nonce = 0;
        HttpsRequestId m_Request = 0;
        uint32_t m_Tries = 0, m_JoinId = 0;
        double m_NextTry = 0.0;
        bool m_Done = false;
        JoinFailure m_Failure = JoinFailure::None;
        std::optional<ClientJoinSetup> m_Result;
        std::vector<std::string> m_Log;
    };

    struct RendezvousHostSettings {
        RendezvousSettings rendezvous;
        std::vector<Endpoint> stunServers;
        CandidateKinds kinds;
        std::optional<Endpoint> given;            ///< --public
        std::vector<LocalInterface> interfaces;   ///< LocalInterfaces()
        uint16_t port = 0;                        ///< the listening port
        uint64_t seed = 1;                        ///< STUN transactions, server nonces
    };

    /// @brief vortex_server --rendezvous: STUN, the room and punching over the listening transport's socket.
    /// Logs NAT:, Rendezvous: and Punch: lines.
    class RendezvousHost {
    public:
        RendezvousHost(HttpsClient& https, RendezvousHostSettings settings, double now);
        /// @brief Every server pass, after the transport was polled (the server's Advance).
        void Update(double now, EnetTransport& udp);
        void Close(double now);
        /// @brief The DELETE was answered, or 2 s passed since Close.
        bool Closed(double now);

    private:
        RendezvousHostSettings m_Settings;
        StunProbe m_Stun;
        Rendezvous m_Rendezvous;
        ServerTraversal m_Traversal;
        double m_Start = 0.0, m_ClosedAt = -1.0;
        bool m_Gathered = false, m_CloseTimedOut = false;
        std::vector<Candidate> m_Candidates;
        std::vector<Datagram> m_Out;
    };

    struct JoinSettings {
        RendezvousSettings rendezvous;
        std::vector<Endpoint> stunServers;
        CandidateKinds kinds;
        std::vector<LocalInterface> interfaces;
        uint16_t port = 0;                        ///< the transport's port (lan candidates)
        uint64_t seed = 1;
    };

    /// @brief Shredborne --join: STUN on udp's socket, POST /v1/join, punching, 1 s of answering after the reach.
    /// Blocks (a 5 ms loop). The proven server address, or nullopt and `error` (the words of "Timeouts").
    std::optional<Endpoint> JoinThroughRendezvous(HttpsClient& https, EnetTransport& udp, const JoinSettings& settings, std::string& error);
}
