module;

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

module vortex.core;

namespace vortex {

    namespace {
        std::shared_ptr<spdlog::logger> g_Logger;
        std::mutex g_InitMutex;
        std::atomic<int> g_Level{static_cast<int>(LogLevel::Info)};

        spdlog::level::level_enum ToSpd(LogLevel level) {
            switch (level) {
                case LogLevel::Trace:    return spdlog::level::trace;
                case LogLevel::Debug:    return spdlog::level::debug;
                case LogLevel::Info:     return spdlog::level::info;
                case LogLevel::Warn:     return spdlog::level::warn;
                case LogLevel::Error:    return spdlog::level::err;
                case LogLevel::Critical: return spdlog::level::critical;
                case LogLevel::Off:      return spdlog::level::off;
            }
            return spdlog::level::info;
        }
    }

    void Log::Init(LogLevel level, const std::string& logFile) {
        std::scoped_lock lock(g_InitMutex);
        g_Level.store(static_cast<int>(level));
        if (g_Logger) {
            g_Logger->set_level(ToSpd(level));
            return;
        }

        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        if (!logFile.empty()) {
            try {
                sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFile, true));
            } catch (const spdlog::spdlog_ex&) {
                // A read-only working directory must not prevent the engine from starting.
            }
        }

        g_Logger = std::make_shared<spdlog::logger>("VORTEX", sinks.begin(), sinks.end());
        g_Logger->set_pattern("%^[%T.%e] [%l]%$ %v");
        g_Logger->set_level(ToSpd(level));
        g_Logger->flush_on(spdlog::level::warn);
    }

    void Log::Shutdown() {
        std::scoped_lock lock(g_InitMutex);
        if (g_Logger) {
            g_Logger->flush();
            g_Logger.reset();
        }
    }

    void Log::SetLevel(LogLevel level) {
        g_Level.store(static_cast<int>(level));
        std::scoped_lock lock(g_InitMutex);
        if (g_Logger) g_Logger->set_level(ToSpd(level));
    }

    LogLevel Log::GetLevel() { return static_cast<LogLevel>(g_Level.load(std::memory_order_relaxed)); }

    bool Log::Enabled(LogLevel level) {
        return static_cast<int>(level) >= g_Level.load(std::memory_order_relaxed);
    }

    void Log::Write(LogLevel level, std::string_view message) {
        // Lazily create a default logger so early messages (before Engine start-up) are not lost.
        if (!g_Logger) Init(GetLevel());
        std::shared_ptr<spdlog::logger> logger;
        {
            std::scoped_lock lock(g_InitMutex);
            logger = g_Logger;
        }
        if (logger) logger->log(ToSpd(level), "{}", message);
    }

    void Verify(bool condition, std::string_view message, std::source_location location) {
        if (condition) [[likely]] return;
        Log::Write(LogLevel::Critical,
                   std::string("Invariant violated: ") + std::string(message) + "\n    at " +
                       location.file_name() + ":" + std::to_string(location.line()) + " (" +
                       location.function_name() + ")");
        Log::Shutdown();
        std::abort();
    }
}
