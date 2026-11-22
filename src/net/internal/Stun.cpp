module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

module vortex.net;

import vortex.core;

namespace vortex::net {

    namespace {
        constexpr uint16_t kBindingRequest = 0x0001;
        constexpr uint16_t kBindingSuccess = 0x0101;
        constexpr uint16_t kMappedAddress = 0x0001;
        constexpr uint16_t kXorMappedAddress = 0x0020;

        uint16_t Read16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
        uint32_t Read32(const uint8_t* p) {
            return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
        }
        void Write16(uint8_t* p, uint16_t v) {
            p[0] = static_cast<uint8_t>(v >> 8);
            p[1] = static_cast<uint8_t>(v);
        }
        void Write32(uint8_t* p, uint32_t v) {
            for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i));
        }
    }

    std::array<uint8_t, kStunHeaderBytes> MakeStunBindingRequest(const StunTransaction& transaction) {
        std::array<uint8_t, kStunHeaderBytes> m{};
        Write16(m.data(), kBindingRequest);
        Write16(m.data() + 2, 0);
        Write32(m.data() + 4, kStunMagicCookie);
        for (size_t i = 0; i < transaction.size(); ++i) m[8 + i] = transaction[i];
        return m;
    }

    bool LooksLikeStun(std::span<const uint8_t> bytes, StunTransaction* transaction) {
        if (bytes.size() < kStunHeaderBytes || (bytes[0] & 0xC0) != 0 || Read32(bytes.data() + 4) != kStunMagicCookie) return false;
        if (transaction != nullptr)
            for (size_t i = 0; i < transaction->size(); ++i) (*transaction)[i] = bytes[8 + i];
        return true;
    }

    bool ParseStunBindingRequest(std::span<const uint8_t> bytes, StunTransaction& transaction) {
        return LooksLikeStun(bytes, &transaction) && Read16(bytes.data()) == kBindingRequest &&
               static_cast<size_t>(Read16(bytes.data() + 2)) + kStunHeaderBytes == bytes.size();
    }

    std::vector<uint8_t> MakeStunBindingResponse(const StunTransaction& transaction, Endpoint mapped) {
        std::vector<uint8_t> m(kStunHeaderBytes + 12);
        Write16(m.data(), kBindingSuccess);
        Write16(m.data() + 2, 12);
        Write32(m.data() + 4, kStunMagicCookie);
        for (size_t i = 0; i < transaction.size(); ++i) m[8 + i] = transaction[i];
        uint8_t* a = m.data() + kStunHeaderBytes;
        Write16(a, kXorMappedAddress);
        Write16(a + 2, 8);
        a[4] = 0;
        a[5] = 0x01;   // IPv4
        Write16(a + 6, static_cast<uint16_t>(mapped.port ^ (kStunMagicCookie >> 16)));
        Write32(a + 8, mapped.ip ^ kStunMagicCookie);
        return m;
    }

    StunResult ParseStunBindingResponse(std::span<const uint8_t> bytes, const StunTransaction& transaction, Endpoint& mapped) {
        StunTransaction seen{};
        if (!LooksLikeStun(bytes, &seen)) return StunResult::NotStun;
        if (seen != transaction) return StunResult::WrongTransaction;
        if (Read16(bytes.data()) != kBindingSuccess) return StunResult::NotSuccess;
        const size_t length = Read16(bytes.data() + 2);
        if (length + kStunHeaderBytes > bytes.size() || length % 4 != 0) return StunResult::Malformed;
        std::optional<Endpoint> xorMapped, plain;
        size_t at = kStunHeaderBytes;
        const size_t end = kStunHeaderBytes + length;
        while (at + 4 <= end) {
            const uint16_t type = Read16(bytes.data() + at);
            const size_t size = Read16(bytes.data() + at + 2);
            const size_t value = at + 4;
            if (value + size > end) return StunResult::Malformed;
            if ((type == kXorMappedAddress || type == kMappedAddress) && size >= 8 && bytes[value + 1] == 0x01) {
                Endpoint e;
                e.port = Read16(bytes.data() + value + 2);
                e.ip = Read32(bytes.data() + value + 4);
                if (type == kXorMappedAddress) {
                    e.port = static_cast<uint16_t>(e.port ^ (kStunMagicCookie >> 16));
                    e.ip ^= kStunMagicCookie;
                    if (!xorMapped) xorMapped = e;
                } else if (!plain) {
                    plain = e;
                }
            }
            at = value + ((size + 3) & ~size_t{3});   // attributes are padded to 4 bytes
        }
        if (xorMapped) {
            mapped = *xorMapped;
            return StunResult::Ok;
        }
        if (plain) {
            mapped = *plain;
            return StunResult::Ok;
        }
        return StunResult::NoAddress;
    }

    const char* ToString(NatClass c) {
        switch (c) {
            case NatClass::Eim: return "eim";
            case NatClass::Symmetric: return "symmetric";
            case NatClass::Unknown: break;
        }
        return "unknown";
    }

    NatClass ClassifyNat(const std::optional<Endpoint>& first, const std::optional<Endpoint>& second) {
        if (!first || !second) return NatClass::Unknown;
        return *first == *second ? NatClass::Eim : NatClass::Symmetric;
    }

    // --- StunProbe ---------------------------------------------------------------------------------

    StunProbe::StunProbe(std::vector<Endpoint> servers, uint64_t seed, Settings settings)
        : m_Servers(std::move(servers)), m_State(m_Servers.size()), m_Mapped(m_Servers.size()), m_Settings(settings), m_Rng(seed) {}

    void StunProbe::Update(double now, std::vector<Datagram>& out) {
        for (size_t i = 0; i < m_Servers.size(); ++i) {
            Server& s = m_State[i];
            if (s.waiting && now - s.sentAt < m_Settings.retrySeconds) continue;
            if (s.waiting && s.tries >= m_Settings.triesPerRound) {
                // This round got no answer: the next one comes a refresh after its first try.
                s.waiting = false;
                s.settled = true;
                continue;
            }
            if (!s.waiting) {
                if (now < s.nextRound) continue;
                for (size_t k = 0; k < s.transaction.size(); k += 4) {
                    const uint32_t v = m_Rng.NextU32();
                    for (size_t b = 0; b < 4; ++b) s.transaction[k + b] = static_cast<uint8_t>(v >> (8 * b));
                }
                s.tries = 0;
                s.waiting = true;
                s.nextRound = now + m_Settings.refreshSeconds;
            }
            // A retry keeps the transaction id (RFC 5389 7.2.1).
            const auto request = MakeStunBindingRequest(s.transaction);
            out.push_back({m_Servers[i], std::vector<uint8_t>(request.begin(), request.end())});
            s.sentAt = now;
            ++s.tries;
        }
    }

    bool StunProbe::OnDatagram(const Datagram& in, double) {
        for (size_t i = 0; i < m_Servers.size(); ++i) {
            Server& s = m_State[i];
            if (!s.waiting || in.address != m_Servers[i]) continue;
            Endpoint mapped;
            if (ParseStunBindingResponse(in.bytes, s.transaction, mapped) != StunResult::Ok) continue;
            const std::optional<Endpoint> before = Outside();
            const bool wasSettled = Settled();
            m_Mapped[i] = mapped;
            s.waiting = false;
            s.settled = true;
            if (wasSettled && before && Outside() && *before != *Outside()) m_Changed = true;
            return true;
        }
        return false;
    }

    bool StunProbe::Settled() const {
        for (const Server& s : m_State)
            if (!s.settled) return false;
        return true;
    }

    std::optional<Endpoint> StunProbe::Outside() const {
        for (const std::optional<Endpoint>& m : m_Mapped)
            if (m) return m;
        return std::nullopt;
    }

    NatClass StunProbe::Class() const {
        if (m_Mapped.size() < 2) return NatClass::Unknown;
        return ClassifyNat(m_Mapped[0], m_Mapped[1]);
    }

    bool StunProbe::TakeChange() {
        const bool changed = m_Changed;
        m_Changed = false;
        return changed;
    }
}
