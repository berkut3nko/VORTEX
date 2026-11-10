module;

#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

export module vortex.core:log;

export namespace vortex {

    enum class LogLevel { Trace, Debug, Info, Warn, Error, Critical, Off };

    /**
     * @brief Engine logger (colored console + optional file), thread safe.
     * @details Formatting uses std::format: `Log::Info("loaded {} chunks in {:.1f} ms", n, ms);`
     */
    class Log {
    public:
        /// @brief Creates the sinks. Safe to call more than once.
        static void Init(LogLevel level = LogLevel::Info, const std::string& logFile = {});
        static void Shutdown();
        static void SetLevel(LogLevel level);
        static LogLevel GetLevel();

        /// @brief Writes an already formatted message.
        static void Write(LogLevel level, std::string_view message);

        template <typename... Args>
        static void Trace(std::format_string<Args...> fmt, Args&&... args) {
            if (Enabled(LogLevel::Trace)) Write(LogLevel::Trace, std::format(fmt, std::forward<Args>(args)...));
        }
        template <typename... Args>
        static void Debug(std::format_string<Args...> fmt, Args&&... args) {
            if (Enabled(LogLevel::Debug)) Write(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
        }
        template <typename... Args>
        static void Info(std::format_string<Args...> fmt, Args&&... args) {
            if (Enabled(LogLevel::Info)) Write(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
        }
        template <typename... Args>
        static void Warn(std::format_string<Args...> fmt, Args&&... args) {
            if (Enabled(LogLevel::Warn)) Write(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
        }
        template <typename... Args>
        static void Error(std::format_string<Args...> fmt, Args&&... args) {
            if (Enabled(LogLevel::Error)) Write(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
        }
        template <typename... Args>
        static void Critical(std::format_string<Args...> fmt, Args&&... args) {
            Write(LogLevel::Critical, std::format(fmt, std::forward<Args>(args)...));
        }

        static bool Enabled(LogLevel level);
    };

    /**
     * @brief Always-on invariant check. Logs the failure with its source location and aborts.
     * @details Use for conditions that would otherwise corrupt memory or GPU state
     * (index out of range, null device ...). It is cheap: one predictable branch.
     */
    void Verify(bool condition, std::string_view message = {},
                std::source_location location = std::source_location::current());

    /// @brief Debug-only check (compiled out when NDEBUG is defined).
    inline void Assert([[maybe_unused]] bool condition, [[maybe_unused]] std::string_view message = {},
                       [[maybe_unused]] std::source_location location = std::source_location::current()) {
#ifndef NDEBUG
        Verify(condition, message, location);
#endif
    }
}
