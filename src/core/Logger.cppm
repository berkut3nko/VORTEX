module;

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>
#include <string>

export module vortex.log;

namespace vortex {
    // Internal shared pointer to the logger instance
    static std::shared_ptr<spdlog::logger> s_CoreLogger;

    /**
     * @brief Static logging utility wrapper around spdlog.
     * @details Provides thread-safe, colored console output for various severity levels.
     */
    export class Log {
    public:
        /**
         * @brief Initializes the logging subsystem.
         * @details Sets up the console sink and default formatting pattern.
         * Pattern: "[Time] LoggerName: Message"
         */
        static void Init() {
            if (s_CoreLogger) return; // Already initialized

            // Create color sink
            auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            s_CoreLogger = std::make_shared<spdlog::logger>("VORTEX", sink);
            
            // Format: [Time] Name: Message
            s_CoreLogger->set_pattern("%^[%T] %n: %v%$");
            s_CoreLogger->set_level(spdlog::level::trace);
        }

        /**
         * @brief Logs an informational message (Green/White).
         * @param msg The message string.
         */
        static void Info(const std::string& msg) {
            if (s_CoreLogger) s_CoreLogger->info(msg);
        }

        /**
         * @brief Logs a warning message (Yellow).
         * @param msg The message string.
         */
        static void Warn(const std::string& msg) {
            if (s_CoreLogger) s_CoreLogger->warn(msg);
        }

        /**
         * @brief Logs an error message (Red).
         * @param msg The message string.
         */
        static void Error(const std::string& msg) {
            if (s_CoreLogger) s_CoreLogger->error(msg);
        }

        /**
         * @brief Access the underlying spdlog logger.
         * @return Reference to the shared pointer of the logger.
         */
        static std::shared_ptr<spdlog::logger>& GetCoreLogger() {
            return s_CoreLogger;
        }
    };
}