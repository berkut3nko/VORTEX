module;

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

module vortex.net;

import vortex.core;

// docs/design/rendezvous.md [nat-R1]: the Worker's client. The wire of the Worker (requests JSON, replies
// text lines): docs/llm/nat-r1.md card 1. Nothing here blocks: requests are started and their answers taken.

namespace vortex::net {

    namespace {
        std::string HexOf(std::span<const uint8_t> bytes) {
            static constexpr char kDigits[] = "0123456789abcdef";
            std::string s;
            s.reserve(bytes.size() * 2);
            for (const uint8_t b : bytes) {
                s.push_back(kDigits[b >> 4]);
                s.push_back(kDigits[b & 15]);
            }
            return s;
        }

        std::vector<std::string_view> Words(std::string_view line) {
            std::vector<std::string_view> out;
            size_t i = 0;
            while (i < line.size()) {
                while (i < line.size() && line[i] == ' ') ++i;
                if (i >= line.size()) break;
                const size_t end = std::min(line.find(' ', i), line.size());
                out.push_back(line.substr(i, end - i));
                i = end;
            }
            return out;
        }

        /// The first line of a reply body, without '\r'.
        std::string_view FirstLine(std::string_view body) {
            std::string_view line = body.substr(0, std::min(body.find('\n'), body.size()));
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            return line;
        }

        template <typename T>
        bool ParseUnsigned(std::string_view text, T& out, int base = 10) {
            if (text.empty()) return false;
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out, base);
            return ec == std::errc() && end == text.data() + text.size();
        }

        bool IsLowerHex(std::string_view s) {
            return std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
        }

        /// "HTTP 409: error room-held" or the transfer's error.
        std::string Describe(const HttpsResponse& r) {
            if (r.failed) return r.error;
            return std::format("HTTP {}: {}", r.status, FirstLine(r.body));
        }

        /// Cloudflare's page for a spent daily quota ("Error 1027": rendezvous.md "Flow", its errors bullet).
        bool QuotaSpentReply(const HttpsResponse& r) {
            return !r.failed && r.status == 429 && r.body.find("1027") != std::string::npos;
        }

        constexpr std::string_view kQuotaLine = "Rendezvous: quota spent until 00:00 UTC";
    }

    // --- Helpers -----------------------------------------------------------------------------------

    std::string RoomHex(const Token& token) { return HexOf(RoomKey(token)); }
    std::string HostKeyHex(const Token& token) { return HexOf(HostKey(token)); }
    std::string BuildHex(uint64_t buildHash) { return std::format("{:016x}", buildHash); }

    bool ValidRendezvousUrl(std::string_view url) {
        if (url.size() < 9 || url.size() > 200) return false;
        if (url.find_first_of(" \t\n") != std::string_view::npos || url.back() == '/') return false;
        if (url.starts_with("https://")) return url.size() > 8;
        for (const std::string_view local : {std::string_view("http://127.0.0.1:"), std::string_view("http://localhost:")}) {
            if (!url.starts_with(local)) continue;
            const std::string_view port = url.substr(local.size());
            return !port.empty() && port.size() <= 5 && std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; });
        }
        return false;
    }

    std::optional<std::string> ReadRendezvousUrlFile(const std::filesystem::path& dir, std::string& error) {
        std::ifstream file(dir / "rendezvous.url");
        if (!file) {
            error = "no rendezvous.url next to the program";
            return std::nullopt;
        }
        std::string line;
        std::getline(file, line);
        const size_t first = line.find_first_not_of(" \r\n");
        const size_t last = line.find_last_not_of(" \r\n");
        line = first == std::string::npos ? std::string() : line.substr(first, last - first + 1);
        if (!ValidRendezvousUrl(line)) {
            error = "rendezvous.url does not hold an https:// URL";
            return std::nullopt;
        }
        return line;
    }

    const char* ToString(RendezvousState s) {
        switch (s) {
            case RendezvousState::Idle: return "idle";
            case RendezvousState::Registering: return "registering";
            case RendezvousState::Waiting: return "waiting";
            case RendezvousState::BackingOff: return "backing off";
            case RendezvousState::Closing: return "closing";
            case RendezvousState::Closed: return "closed";
        }
        return "?";
    }

    const char* ToString(JoinFailure f) {
        switch (f) {
            case JoinFailure::None: return "none";
            case JoinFailure::NoRoom: return "no server has this room (is it running with --rendezvous, and is the token right?)";
            case JoinFailure::AnotherBuild: return "the server runs another build";
            case JoinFailure::IpMismatch: return "the Worker saw another outside address than STUN (a VPN?)";
            case JoinFailure::NonceUsed: return "the rendezvous refused the join's nonce";
            case JoinFailure::TooManyJoins: return "too many joins to this room in a minute";
            case JoinFailure::Network: return "the rendezvous cannot be reached";
            case JoinFailure::BadReply: return "the rendezvous answered something unexpected";
        }
        return "?";
    }

    // --- Rendezvous (the server's room) -------------------------------------------------------------

    Rendezvous::Rendezvous(HttpsClient& https, RendezvousSettings settings)
        : m_Https(https), m_Settings(std::move(settings)), m_Room(RoomHex(m_Settings.token)), m_Auth("Bearer " + HostKeyHex(m_Settings.token)) {}

    void Rendezvous::Fail(double now, const HttpsResponse& r, std::string_view what) {
        ++m_Failures;
        const double delay = std::min(kRendezvousMaxBackoffSeconds, static_cast<double>(1u << std::min(m_Failures - 1, 5u)));
        m_NextTry = now + delay;
        m_State = RendezvousState::BackingOff;
        if (QuotaSpentReply(r)) {
            if (!m_QuotaLogged) {
                m_Log.emplace_back(kQuotaLine);
                m_QuotaLogged = true;
            }
            return;
        }
        m_Log.push_back(std::format("Rendezvous: {} failed ({}); trying again in {:.0f} s", what, Describe(r), delay));
    }

    void Rendezvous::Update(double now, std::span<const Candidate> candidates, NatClass nat) {
        if (m_State == RendezvousState::Closing) {
            if (m_Delete != 0) {
                if (const std::optional<HttpsResponse> r = m_Https.Take(m_Delete)) {
                    m_Delete = 0;
                    if (!r->failed && r->status == 200)
                        m_Log.push_back(std::format("Rendezvous: room {} deleted", m_Room.substr(0, 8)));
                    else
                        m_Log.push_back(std::format("Rendezvous: the delete got {}", r->failed ? r->error : std::to_string(r->status)));
                    m_State = RendezvousState::Closed;
                }
            }
            return;
        }
        if (m_State == RendezvousState::Closed) return;

        // The register answer.
        if (m_Post != 0) {
            if (const std::optional<HttpsResponse> r = m_Https.Take(m_Post)) {
                m_Post = 0;
                if (!r->failed && r->status == 200) {
                    m_Registered = m_Pending;
                    m_Failures = 0;
                    m_QuotaLogged = false;
                    if (!m_EverRegistered)
                        m_Log.push_back(std::format("Rendezvous: room {} registered, {} candidates", m_Room.substr(0, 8), m_PendingCount));
                    else
                        m_Log.push_back(std::format("Rendezvous: registered again, {} candidates (the candidates changed)", m_PendingCount));
                    m_EverRegistered = true;
                    m_State = RendezvousState::Waiting;
                } else {
                    Fail(now, *r, "register");
                }
            }
        }

        // The long poll's answer.
        if (m_Wait != 0) {
            if (const std::optional<HttpsResponse> r = m_Https.Take(m_Wait)) {
                m_Wait = 0;
                if (!r->failed && r->status == 200) {
                    std::string_view body = r->body;
                    while (!body.empty()) {
                        const size_t end = std::min(body.find('\n'), body.size());
                        std::string_view line = body.substr(0, end);
                        body.remove_prefix(std::min(end + 1, body.size()));
                        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                        if (line.empty()) continue;
                        const std::vector<std::string_view> w = Words(line);
                        ServerJoin join;
                        bool ok = (w.size() == 5 || w.size() == 6) && w[0] == "join" && ParseUnsigned(w[1], join.id) && join.id > 0 &&
                                  w[2].size() == 16 && IsLowerHex(w[2]) && ParseUnsigned(w[2], join.nonceClient, 16) &&
                                  join.nonceClient != 0 && (w[3] == "0" || w[3] == "1");
                        if (!ok) {
                            m_Log.emplace_back("Rendezvous: a join line was malformed");
                            continue;
                        }
                        if (join.id <= m_After) continue;
                        join.verified = w[3] == "1";
                        join.candidates = std::string(w[4]);
                        if (w.size() == 6) join.route = std::string(w[5]);
                        m_After = join.id;
                        m_Joins.push_back(std::move(join));
                    }
                    m_Failures = 0;
                    m_QuotaLogged = false;   // a streak of spent quota ends on any answer, the wait's too
                } else if (!r->failed && r->status == 404) {
                    m_Log.emplace_back("Rendezvous: the Worker no longer has the room; registering again");
                    m_Registered.clear();
                    m_State = RendezvousState::Registering;
                } else {
                    Fail(now, *r, "wait");
                }
            }
        }

        // Register, or register again when the candidates or the NAT class changed. Never waits for the
        // long poll (a POST runs on the short-call lane).
        const std::string desired = FormatCandidates(candidates) + " " + ToString(nat);
        if (m_Post == 0 && now >= m_NextTry && desired != m_Registered) {
            HttpsRequest q;
            q.method = "POST";
            q.url = m_Settings.url + "/v1/host";
            q.body = std::format(R"({{"room":"{}","cands":"{}","nat":"{}","build":"{}","protocol":{}}})", m_Room,
                                 FormatCandidates(candidates), ToString(nat), BuildHex(m_Settings.buildHash), m_Settings.protocol);
            q.headers.emplace_back("Authorization", m_Auth);
            q.timeoutMs = kRendezvousShortTimeoutMs;
            q.longPoll = false;
            m_Post = m_Https.Start(std::move(q));
            m_Pending = desired;
            m_PendingCount = candidates.size();
            if (m_Registered.empty()) m_State = RendezvousState::Registering;
        }

        // The long poll, once something is registered.
        if (m_Wait == 0 && !m_Registered.empty() && now >= m_NextTry) {
            HttpsRequest q;
            q.method = "GET";
            q.url = std::format("{}/v1/host/wait?room={}&after={}", m_Settings.url, m_Room, m_After);
            q.headers.emplace_back("Authorization", m_Auth);
            q.timeoutMs = kRendezvousWaitTimeoutMs;
            q.longPoll = true;
            m_Wait = m_Https.Start(std::move(q));
            m_State = RendezvousState::Waiting;
        }
    }

    void Rendezvous::Close(double /*now*/) {
        if (m_State == RendezvousState::Closing || m_State == RendezvousState::Closed) return;
        if (m_Wait != 0) {
            m_Https.Cancel(m_Wait);
            m_Wait = 0;
        }
        // A register POST in flight shares the short-call lane with the DELETE: cancelled, it aborts at its
        // next progress call and the DELETE starts at once (RendezvousHost::Closed gives it 2 s).
        if (m_Post != 0) {
            m_Https.Cancel(m_Post);
            m_Post = 0;
        }
        HttpsRequest q;
        q.method = "DELETE";
        q.url = m_Settings.url + "/v1/host";
        q.body = std::format(R"({{"room":"{}"}})", m_Room);
        q.headers.emplace_back("Authorization", m_Auth);
        q.timeoutMs = kRendezvousDeleteTimeoutMs;
        q.longPoll = false;
        m_Delete = m_Https.Start(std::move(q));
        m_State = RendezvousState::Closing;
    }

    std::vector<ServerJoin> Rendezvous::TakeJoins() { return std::exchange(m_Joins, {}); }
    std::vector<std::string> Rendezvous::TakeLog() { return std::exchange(m_Log, {}); }

    // --- RendezvousJoin (one client join) ----------------------------------------------------------

    RendezvousJoin::RendezvousJoin(HttpsClient& https, RendezvousSettings settings, std::span<const Candidate> candidates,
                                   uint64_t nonce, double now)
        : m_Https(https), m_Settings(std::move(settings)), m_Nonce(nonce), m_NextTry(now) {
        std::vector<Candidate> kept;
        size_t stun = 0;
        for (const Candidate& c : candidates) {
            if (c.kind == CandidateKind::Lan || (c.kind == CandidateKind::Stun && stun++ < kMaxJoinStunCandidates)) kept.push_back(c);
        }
        m_Posted = FormatCandidates(kept);
        m_Body = std::format(R"({{"room":"{}","cands":"{}","nonce":"{:016x}","build":"{}","protocol":{}}})", RoomHex(m_Settings.token),
                             m_Posted, m_Nonce, BuildHex(m_Settings.buildHash), m_Settings.protocol);
        HttpsRequest q;
        q.method = "POST";
        q.url = m_Settings.url + "/v1/join";
        q.body = m_Body;
        q.timeoutMs = kRendezvousShortTimeoutMs;
        m_Request = m_Https.Start(std::move(q));
        m_Tries = 1;
    }

    void RendezvousJoin::Update(double now) {
        if (m_Done) return;
        if (m_Request == 0) {
            if (now >= m_NextTry) {
                HttpsRequest q;
                q.method = "POST";
                q.url = m_Settings.url + "/v1/join";
                q.body = m_Body;
                q.timeoutMs = kRendezvousShortTimeoutMs;
                m_Request = m_Https.Start(std::move(q));
                ++m_Tries;
            }
            return;
        }
        const std::optional<HttpsResponse> r = m_Https.Take(m_Request);
        if (!r) return;
        m_Request = 0;
        const auto finish = [&](JoinFailure f) {
            m_Failure = f;
            m_Done = true;
        };

        if (r->failed) {
            if (m_Tries < 3) {
                m_NextTry = now + (m_Tries == 1 ? 1.0 : 2.0);
                m_Log.push_back(std::format("Rendezvous: the join got no answer ({}); trying again", r->error));
            } else {
                m_Log.push_back(std::format("Rendezvous: the join got no answer ({})", r->error));
                finish(JoinFailure::Network);
            }
            return;
        }

        const std::string_view line = FirstLine(r->body);
        if (r->status == 200) {
            // server <join id> <age s> <nat> <server cands> <verified 0|1> [<route>]
            const std::vector<std::string_view> w = Words(line);
            uint32_t id = 0;
            uint64_t age = 0;
            std::vector<Candidate> cands, route;
            const bool ok = (w.size() == 6 || w.size() == 7) && w[0] == "server" && ParseUnsigned(w[1], id) && id > 0 &&
                            ParseUnsigned(w[2], age) && (w[3] == "eim" || w[3] == "symmetric" || w[3] == "unknown") &&
                            ParseCandidates(w[4], cands) && (w[5] == "0" || w[5] == "1") &&
                            (w.size() == 6 || (ParseCandidates(w[6], route) && route.size() == cands.size()));
            if (!ok) {
                m_Log.push_back(std::format("Rendezvous: the join reply was malformed: {}", line.substr(0, 200)));
                finish(JoinFailure::BadReply);
                return;
            }
            m_JoinId = id;
            ClientJoinSetup setup;
            setup.token = m_Settings.token;
            setup.nonce = m_Nonce;
            setup.candidates = m_Posted;
            setup.server = w.size() == 7 ? std::move(route) : std::move(cands);
            setup.serverAgeSeconds = static_cast<double>(age);
            m_Result = std::move(setup);
            if (w[5] == "0") m_Log.emplace_back("Rendezvous: the Worker saw IPv6; the HTTPS client should force IPv4");
            m_Done = true;
            return;
        }

        const std::vector<std::string_view> w = Words(line);
        const std::string_view word = w.size() >= 2 && w[0] == "error" ? w[1] : std::string_view();
        if (r->status == 404 && word == "no-room") {
            finish(JoinFailure::NoRoom);
        } else if (r->status == 409 && word == "another-build") {
            m_Log.push_back(std::format("Rendezvous: the server runs another build ({}; this is {})", w.size() >= 3 ? w[2] : "?",
                                        BuildHex(m_Settings.buildHash)));
            finish(JoinFailure::AnotherBuild);
        } else if (r->status == 409 && word == "nonce-used") {
            finish(JoinFailure::NonceUsed);
        } else if (r->status == 403 && word == "ip-mismatch") {
            std::string stun = "-";
            std::vector<Candidate> posted;
            if (ParseCandidates(m_Posted, posted))
                for (const Candidate& c : posted)
                    if (c.kind == CandidateKind::Stun) {
                        stun = FormatIpv4(c.at.ip);
                        break;
                    }
            m_Log.push_back(std::format("Rendezvous: the Worker saw this machine as {}, STUN as {}: a VPN or a second outside address",
                                        w.size() >= 3 ? w[2] : "?", stun));
            finish(JoinFailure::IpMismatch);
        } else if (QuotaSpentReply(*r)) {
            m_Log.emplace_back(kQuotaLine);
            finish(JoinFailure::Network);
        } else if (r->status == 429) {
            finish(JoinFailure::TooManyJoins);
        } else {
            m_Log.push_back(std::format("Rendezvous: the join got {}", Describe(*r).substr(0, 200)));
            finish(JoinFailure::BadReply);
        }
    }

    std::vector<std::string> RendezvousJoin::TakeLog() { return std::exchange(m_Log, {}); }
}
