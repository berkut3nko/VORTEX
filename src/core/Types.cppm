module;

#include <cstdint>
#include <cstddef>
#include <functional>
#include <limits>

export module vortex.core:types;

export namespace vortex {

    /**
     * @brief Generational handle: an index plus a generation counter.
     * @details A stale handle (its slot was freed and reused) never compares equal to the
     * handle of the new occupant, which turns use-after-free into a detectable "invalid handle".
     */
    template <typename Tag>
    struct Handle {
        static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

        uint32_t index = kInvalidIndex;
        uint32_t generation = 0;

        constexpr bool IsValid() const { return index != kInvalidIndex; }
        constexpr explicit operator bool() const { return IsValid(); }
        friend constexpr bool operator==(const Handle&, const Handle&) = default;
    };

    /// @brief Base for types that own a unique resource.
    struct NonCopyable {
        NonCopyable() = default;
        NonCopyable(const NonCopyable&) = delete;
        NonCopyable& operator=(const NonCopyable&) = delete;
        NonCopyable(NonCopyable&&) = default;
        NonCopyable& operator=(NonCopyable&&) = default;
    };

    /// @brief Runs a callable when it goes out of scope.
    template <typename F>
    class ScopeExit {
    public:
        explicit ScopeExit(F fn) : m_Fn(std::move(fn)) {}
        ~ScopeExit() { m_Fn(); }
        ScopeExit(const ScopeExit&) = delete;
        ScopeExit& operator=(const ScopeExit&) = delete;
    private:
        F m_Fn;
    };

    constexpr size_t operator""_KiB(unsigned long long v) { return static_cast<size_t>(v) * 1024u; }
    constexpr size_t operator""_MiB(unsigned long long v) { return static_cast<size_t>(v) * 1024u * 1024u; }
}
