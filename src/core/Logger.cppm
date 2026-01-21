module;

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>
#include <string>

export module vortex.log;

namespace vortex {
    // Глобальний вказівник на логер (внутрішній для модуля або namespace)
    static std::shared_ptr<spdlog::logger> s_CoreLogger;

    export class Log {
    public:
        static void Init() {
            if (s_CoreLogger) return; // Вже ініціалізовано

            // Створюємо логер з кольоровим виводом
            auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            s_CoreLogger = std::make_shared<spdlog::logger>("VORTEX", sink);
            
            // Налаштування формату: [Час] Ім'я: Повідомлення
            s_CoreLogger->set_pattern("%^[%T] %n: %v%$");
            s_CoreLogger->set_level(spdlog::level::trace);
        }

        static void Info(const std::string& msg) {
            if (s_CoreLogger) s_CoreLogger->info(msg);
        }

        static void Warn(const std::string& msg) {
            if (s_CoreLogger) s_CoreLogger->warn(msg);
        }

        static void Error(const std::string& msg) {
            if (s_CoreLogger) s_CoreLogger->error(msg);
        }

        static std::shared_ptr<spdlog::logger>& GetCoreLogger() {
            return s_CoreLogger;
        }
    };
}