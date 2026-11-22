module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

export module vortex.net:crypto;

// docs/design/rendezvous.md "The engine side": SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104) in
// plain C++. They serve the room key, the host key and the punch proof only, never anything that
// decides voxels.

export namespace vortex::net {

    using Sha256Digest = std::array<uint8_t, 32>;

    /// @brief Incremental SHA-256.
    class Sha256 {
    public:
        Sha256();
        void Update(std::span<const uint8_t> bytes);
        void Update(std::string_view text);
        /// @brief The digest; the object is spent afterwards (make a new one to hash again).
        Sha256Digest Finish();

        static Sha256Digest Of(std::span<const uint8_t> bytes);
        static Sha256Digest Of(std::string_view text);

    private:
        void Block(const uint8_t* block);
        std::array<uint32_t, 8> m_State{};
        std::array<uint8_t, 64> m_Buffer{};
        size_t m_Buffered = 0;
        uint64_t m_Length = 0;   ///< bytes hashed so far
    };

    /// @brief HMAC-SHA256 of the concatenation of `parts` under `key`.
    Sha256Digest HmacSha256(std::span<const uint8_t> key, std::span<const std::span<const uint8_t>> parts);
    Sha256Digest HmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> message);
}
