module;

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

export module vortex.core:profiler;

export namespace vortex {

    /**
     * @brief Lightweight frame profiler: named timers (CPU scopes, GPU pass timings) with a
     * short history, plus named counters. Thread safe; presentation lives in the editor.
     */
    class Profiler {
    public:
        static constexpr int kHistory = 120;

        struct Timer {
            std::string name;
            float lastMs = 0.0f;
            float avgMs = 0.0f;
            float maxMs = 0.0f;
            std::array<float, kHistory> history{};
            int cursor = 0;      ///< Index of the next sample to be written.
            int samples = 0;     ///< Number of valid samples (<= kHistory).
        };

        struct Counter {
            std::string name;
            double value = 0.0;
        };

        /// @brief Records a duration in milliseconds. Multiple samples per frame are summed.
        static void AddSample(std::string_view name, float milliseconds);
        /// @brief Sets a named value shown next to the timers (chunk counts, memory ...).
        static void SetCounter(std::string_view name, double value);
        /// @brief Closes the current frame: per-frame sums become history samples.
        static void EndFrame();

        static std::vector<Timer> SnapshotTimers();
        static std::vector<Counter> SnapshotCounters();
    };

    /**
     * @brief RAII CPU timer. `name` must outlive the scope (use a string literal).
     * @code ProfileScope scope("World::Update"); @endcode
     */
    class ProfileScope {
    public:
        explicit ProfileScope(const char* name) : m_Name(name), m_Start(std::chrono::steady_clock::now()) {}
        ~ProfileScope() {
            const auto end = std::chrono::steady_clock::now();
            Profiler::AddSample(m_Name, std::chrono::duration<float, std::milli>(end - m_Start).count());
        }
        ProfileScope(const ProfileScope&) = delete;
        ProfileScope& operator=(const ProfileScope&) = delete;

    private:
        const char* m_Name;
        std::chrono::steady_clock::time_point m_Start;
    };
}
