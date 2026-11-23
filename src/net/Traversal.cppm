module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module vortex.net:traversal;

import vortex.core;
import :address;
import :crypto;
import :stun;
import :messages;

// docs/design/rendezvous.md: candidates, the room and host keys, the punch datagram and its proof,
// and net::Traversal - the hole punching of one join as a pure state machine on both sides (no
// socket, no clock: datagrams and times go in, datagrams and outcomes come out). R0's pair test
// (PunchTest) puts it together with StunProbe and a pasted line in place of the Worker's join.

export namespace vortex::net {

    // --- Candidates --------------------------------------------------------------------------------

    /// @brief Where a candidate address came from. The order is the client's preference when two are
    /// proven (rendezvous.md "Choosing"): lan > given > stun > peer-reflexive.
    enum class CandidateKind : uint8_t { Lan = 0, Given = 1, Stun = 2, PeerReflexive = 3 };
    /// "lan", "given", "stun", "peer-reflexive".
    const char* ToString(CandidateKind kind);

    struct Candidate {
        Endpoint at;
        CandidateKind kind = CandidateKind::Stun;
        friend bool operator==(const Candidate&, const Candidate&) = default;
    };

    /// @brief Which kinds a side offers (`--candidates lan,stun,given`).
    struct CandidateKinds {
        bool lan = true, stun = true, given = true;
    };
    /// @brief "lan,stun,given" in any order and subset; false for an unknown or empty word, or empty text.
    bool ParseCandidateKinds(std::string_view text, CandidateKinds& out);

    /// @brief At most this many candidates in a list (the Worker's `cands`), and stun ones in a join.
    inline constexpr size_t kMaxCandidates = 8;
    inline constexpr size_t kMaxJoinStunCandidates = 2;

    /// @brief "a.b.c.d:port/kind,..." (kinds lan, given, stun; "-" for none). R0's pasted lines and R1's
    /// Worker carry candidates in this form; the client's string is hashed as written.
    std::string FormatCandidates(std::span<const Candidate> candidates);
    /// @brief The inverse of FormatCandidates: at most kMaxCandidates, no peer-reflexive.
    bool ParseCandidates(std::string_view text, std::vector<Candidate>& out);

    // --- Keys --------------------------------------------------------------------------------------

    using Key16 = std::array<uint8_t, 16>;
    /// @brief The room key: SHA-256("vortex-room-v1" || the token's 16 bytes), first 16 bytes.
    Key16 RoomKey(const Token& token);
    /// @brief The host key: HMAC-SHA256(token bytes, "vortex-host-v1"), first 16 bytes.
    Key16 HostKey(const Token& token);

    // --- The punch datagram (rendezvous.md "Punch datagram") ----------------------------------------

    inline constexpr size_t kPunchBytes = 39;
    inline constexpr uint8_t kPunchVersion = 1;
    enum class PunchKind : uint8_t { Probe = 1, Ack = 2 };
    enum class PunchRole : uint8_t { Server = 'S', Client = 'C' };
    using Tag8 = std::array<uint8_t, 8>;

    struct Punch {
        PunchKind kind = PunchKind::Probe;
        PunchRole sender = PunchRole::Client;
        Tag8 room{};                  ///< the room key's first 8 bytes
        uint64_t nonceClient = 0;     ///< big-endian in bytes 15-22
        uint64_t nonceServer = 0;     ///< big-endian in bytes 23-30; 0 in a client's Probe before it heard the server
        Tag8 proof{};
    };

    /// @brief The first 8 bytes of SHA-256 over the client's candidate string, as posted (pasted).
    Tag8 CandHash(std::string_view clientCandidates);
    Tag8 RoomTag(const Token& token);
    /// @brief HMAC-SHA256(token, "vxpn1" || bytes 4-30 || candHash), first 8 bytes.
    Tag8 PunchProof(const Token& token, const Punch& p, const Tag8& candHash);
    std::array<uint8_t, kPunchBytes> EncodePunch(const Punch& p);
    /// @brief 39 bytes, the magic and version 1, a known kind and sender; the proof is not checked.
    bool DecodePunch(std::span<const uint8_t> bytes, Punch& out);
    /// @brief The magic "VXPN" at offset 0 (what EnetTransport's intercept takes from ENet).
    bool HasPunchMagic(std::span<const uint8_t> bytes);

    // --- Traversal ---------------------------------------------------------------------------------

    /// @brief The schedule and caps of one join (rendezvous.md "Punch datagram", "Flow").
    struct TraversalLimits {
        double fastSeconds = 10.0;      ///< probe rounds every fastInterval for this long ...
        double fastInterval = 0.1;
        double slowInterval = 0.0;      ///< ... then every slowInterval (0: no more rounds)
        double windowSeconds = 10.0;    ///< not reached by then: failed
        double acceptSeconds = 15.0;    ///< the server takes a join's datagrams this long after the join came
        double chooseSeconds = 0.15;    ///< after the first proven Ack, wait this long for a better kind
        double staleServerSeconds = 25.0;   ///< a join reply with an older server last_seen: ServerStale
        uint32_t maxDatagrams = 100;    ///< per join and side, probes and acks together
        /// server: probes to an unverified join's stun candidates (Acks answer proven datagrams and count
        /// only toward maxDatagrams)
        uint32_t maxUnverifiedStun = 10;
        uint32_t maxLearned = 2;        ///< peer-reflexive addresses a join learns
        uint32_t maxJoins = 4;          ///< server: joins punched at once
    };
    /// @brief R0's pair test: 10 probe rounds a second for 10 s, then one a second, for 5 minutes.
    TraversalLimits PairTestLimits();

    enum class TraversalState : uint8_t { Punching, Reached, Failed };
    /// @brief Why a client's join failed (rendezvous.md "Timeouts").
    enum class TraversalFailure : uint8_t {
        None,
        ServerStale,   ///< the join reply showed the server's last_seen older than 25 s
        NoAnswer,      ///< no proven datagram from the server arrived
        CannotMeet,    ///< the server was heard, but no address was proven both ways
    };
    const char* ToString(TraversalFailure f);
    /// @brief ToString(f), except ServerStale: "the server has not reached the rendezvous for N s
    /// (restarting or backing off)".
    std::string FailureText(TraversalFailure f, double serverAgeSeconds);

    /// @brief What a client's join starts from: the Worker's reply (R1) or a pasted line (R0).
    struct ClientJoinSetup {
        Token token{};
        uint64_t nonce = 0;               ///< this join's client nonce (fresh, non-zero)
        std::string candidates;           ///< this client's candidate string as posted: candHash covers it
        std::vector<Candidate> server;    ///< the server's candidates
        double serverAgeSeconds = 0.0;    ///< the age of the server's last_seen in the join reply
    };

    /**
     * @brief The client side of one join: probes the server's candidates (and up to 2 peer-reflexive
     * addresses learned from proven server datagrams), answers proven server Probes with an Ack, and
     * chooses the best kind of address an Ack proved, 150 ms after the first (at once for lan). It takes
     * a server datagram only with its own nonce, a non-zero server nonce (the same after the first) and
     * a proof valid for its own candHash. An Ack proves the address only if it came from one this side
     * probed. After Reached it keeps answering Probes (the server proves its side), within the cap.
     */
    class ClientTraversal {
    public:
        ClientTraversal(ClientJoinSetup setup, TraversalLimits limits, double now);
        /// @brief true when `in` was a valid server datagram of this join (consumed).
        bool OnDatagram(const Datagram& in, double now);
        void Update(double now, std::vector<Datagram>& out);

        TraversalState State() const { return m_State; }
        TraversalFailure Failure() const { return m_Failure; }
        /// @brief The chosen address once Reached.
        const Candidate& Chosen() const { return m_Chosen; }
        /// @brief Seconds from the start to the choice.
        double ReachedAfter() const { return m_ReachedAfter; }
        bool HeardServer() const { return m_Heard; }
        uint32_t Sent() const { return m_Sent; }
        uint64_t Nonce() const { return m_Setup.nonce; }

    private:
        struct Target {
            Candidate candidate;
            bool probed = false;
        };
        void Send(Endpoint to, PunchKind kind, std::vector<Datagram>& out);
        Target* Find(Endpoint e);

        ClientJoinSetup m_Setup;
        TraversalLimits m_Limits;
        Tag8 m_Room{}, m_CandHash{};
        std::vector<Target> m_Targets;
        uint32_t m_Learned = 0;
        uint64_t m_NonceServer = 0;
        bool m_Heard = false;
        double m_Start = 0.0, m_NextRound = 0.0;
        std::optional<Candidate> m_Best;
        double m_FirstProven = 0.0;
        std::vector<Endpoint> m_Acks;   ///< Acks to send at the next Update
        std::vector<Endpoint> m_Probes; ///< learned addresses to probe at the next Update
        TraversalState m_State = TraversalState::Punching;
        TraversalFailure m_Failure = TraversalFailure::None;
        Candidate m_Chosen{};
        double m_ReachedAfter = 0.0;
        uint32_t m_Sent = 0;
    };

    /// @brief A client's join as the server gets it (R1: from the Worker's long poll; R0: pasted).
    struct ServerJoin {
        uint32_t id = 0;
        uint64_t nonceClient = 0;
        std::string candidates;   ///< the client's string as posted: candHash covers it
        bool verified = true;     ///< the Worker saw the stun candidates' IP as the caller's
        std::string route;        ///< G20 only (local.mjs): where to punch instead of `candidates` (same count); candHash still covers `candidates`
    };

    /// @brief How a server's join ended (once per join).
    struct ServerJoinOutcome {
        uint32_t id = 0;
        bool reached = false;
        Candidate at{};           ///< the client's address an Ack proved (reached)
        double seconds = 0.0;     ///< from the join to the outcome
        bool heard = false;       ///< a proven client datagram arrived
    };

    enum class AddJoinResult : uint8_t { Added, DuplicateNonce, Busy, BadCandidates };

    /**
     * @brief The server side: up to 4 joins at once. Each join probes the client's candidates (lan only
     * in one of the server's own subnets, an unverified join's stun ones at most 10 times) and up to 2
     * learned addresses, answers proven client Probes with an Ack, and is reached when an Ack from an
     * address it probed arrives. It takes a client datagram only with a valid proof, within
     * acceptSeconds of the join, and with a server nonce of 0 or its own; a second join with a client
     * nonce it holds is refused. Everything else is dropped silently. A join holds its slot until
     * acceptSeconds after it came, finished or not.
     */
    class ServerTraversal {
    public:
        ServerTraversal(const Token& token, std::vector<LocalInterface> subnets, TraversalLimits limits, uint64_t seed);
        AddJoinResult AddJoin(const ServerJoin& join, double now);
        bool OnDatagram(const Datagram& in, double now);
        void Update(double now, std::vector<Datagram>& out);
        /// @brief Joins that reached or failed since the last call.
        std::vector<ServerJoinOutcome> TakeOutcomes();
        size_t ActiveJoins() const { return m_Joins.size(); }
        /// @brief Datagrams sent for join `id` (0 for an unknown one); tests.
        uint32_t SentFor(uint32_t id) const;
        /// @brief Probes of join `id` to its stun candidates (Acks are not counted; 0 for an unknown one).
        uint32_t SentToStunFor(uint32_t id) const;
        /// @brief The server nonce of join `id` (0 for an unknown one).
        uint64_t NonceFor(uint32_t id) const;

    private:
        struct Target {
            Candidate candidate;
            bool probed = false;
        };
        struct Join {
            ServerJoin join;
            Tag8 candHash{};
            uint64_t nonceServer = 0;
            std::vector<Target> targets;
            uint32_t learned = 0;
            double start = 0.0, nextRound = 0.0;
            uint32_t sent = 0, sentToStun = 0;
            bool heard = false;
            std::optional<Candidate> best;
            double firstProven = 0.0;
            bool done = false;        ///< its outcome was reported
            std::vector<Endpoint> acks;
        };
        bool Send(Join& j, Endpoint to, PunchKind kind, std::vector<Datagram>& out);
        static Target* Find(Join& j, Endpoint e);

        Token m_Token{};
        Tag8 m_Room{};
        std::vector<LocalInterface> m_Subnets;
        TraversalLimits m_Limits;
        Rng m_Rng;
        std::vector<Join> m_Joins;
        std::vector<ServerJoinOutcome> m_Outcomes;
    };

    /// @brief The candidates a side offers (PunchTest's line, R1's register and join): lan
    /// (IsLanInterface, at most 4, interface order, at `port`), then given (server only), then each
    /// distinct answered stun mapping (at most 2); at most kMaxCandidates, no address twice.
    std::vector<Candidate> ChooseCandidates(PunchRole role, std::span<const LocalInterface> interfaces, uint16_t port,
                                            const CandidateKinds& kinds, const std::optional<Endpoint>& given,
                                            std::span<const std::optional<Endpoint>> stunMapped);

    // --- R0's pair test ----------------------------------------------------------------------------

    /// @brief How one side of the pair test is set up (rendezvous.md "R0's pair test").
    struct PunchTestSettings {
        PunchRole role = PunchRole::Client;
        Token token{};
        std::vector<Endpoint> stunServers;
        std::vector<LocalInterface> interfaces;   ///< lan candidates and, on the server, its subnets
        uint16_t port = 0;                        ///< this side's UDP port (lan candidates use it)
        std::optional<Endpoint> given;            ///< server: an extra `given` candidate (`--public`)
        CandidateKinds kinds;
        uint64_t seed = 1;                        ///< STUN transaction ids, the nonces
        TraversalLimits limits = PairTestLimits();
        StunProbe::Settings stun;
        /// The first round of STUN gives up after this long (both roles print their line then).
        double gatherSeconds = 7.0;
    };

    /**
     * @brief One side of R0's pair test without the Worker: STUN (kept every 15 s) gives the
     * candidates; Line() is what to paste into the other program, and Paste takes the other's line in
     * place of the Worker's join. From its own paste this side probes (10 rounds a second for 10 s,
     * then one a second) and answers proven probes until it has reached the peer or 5 minutes passed.
     * Logs `NAT:` and `Punch:` lines (TakeLog). No socket and no clock: the runner (RunNatTool) or a
     * test (over NatSim) moves datagrams and time.
     */
    class PunchTest {
    public:
        enum class Phase : uint8_t { Gathering, WaitingForPaste, Punching, Reached, Failed };

        PunchTest(PunchTestSettings settings, double now);
        /// @brief true when `in` was a STUN answer or a punch datagram of this test (consumed).
        bool OnDatagram(const Datagram& in, double now);
        void Update(double now, std::vector<Datagram>& out);
        /// @brief The other program's line (any text containing it). false and `error` for a line of
        /// this role, of another token, malformed, or a second paste.
        bool Paste(std::string_view text, double now, std::string& error);

        Phase CurrentPhase() const { return m_Phase; }
        /// @brief This side's line, once STUN has settled (else empty).
        const std::string& Line() const { return m_Line; }
        /// @brief The address reached (client: chosen; server: the client's address proven).
        const Candidate& ReachedAt() const { return m_Reached; }
        TraversalFailure Failure() const { return m_Failure; }
        NatClass Class() const { return m_Stun.Class(); }
        std::vector<std::string> TakeLog();
        const std::vector<Candidate>& Candidates() const { return m_Candidates; }

    private:
        void MakeLine();
        void Log(std::string line) { m_Log.push_back(std::move(line)); }

        PunchTestSettings m_Settings;
        StunProbe m_Stun;
        double m_Start = 0.0, m_PastedAt = 0.0;
        uint64_t m_Nonce = 0;
        Phase m_Phase = Phase::Gathering;
        std::vector<Candidate> m_Candidates;
        std::string m_CandidateText, m_Line;
        std::optional<ClientTraversal> m_Client;
        std::optional<ServerTraversal> m_Server;
        Candidate m_Reached{};
        TraversalFailure m_Failure = TraversalFailure::None;
        std::vector<std::string> m_Log;
    };

    /// @brief "VXPT1": what every pair-test line starts with.
    inline constexpr std::string_view kPunchLinePrefix = "VXPT1";
}
