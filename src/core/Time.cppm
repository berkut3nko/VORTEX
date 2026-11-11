module;

#include <chrono>

export module vortex.core:time;

export namespace vortex {

    /// @brief Monotonic time in seconds since the first call.
    inline double NowSeconds() {
        using Clock = std::chrono::steady_clock;
        static const Clock::time_point start = Clock::now();
        return std::chrono::duration<double>(Clock::now() - start).count();
    }

    /// @brief Simple stopwatch.
    class Stopwatch {
    public:
        Stopwatch() : m_Start(std::chrono::steady_clock::now()) {}
        void Reset() { m_Start = std::chrono::steady_clock::now(); }
        double Seconds() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_Start).count(); }
        double Milliseconds() const { return Seconds() * 1000.0; }
    private:
        std::chrono::steady_clock::time_point m_Start;
    };
}
