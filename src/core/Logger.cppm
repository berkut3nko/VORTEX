module;
#include <spdlog/spdlog.h>
#include <string>
export module vortex.core:logger;

namespace vortex
{
    /**
     * @brief Static logger wrapper to provide clean API in modules.
     */
    export class Log {
    public:
        static void Init();
        
        static void Info(const std::string& msg);
        static void Warn(const std::string& msg);
        static void Error(const std::string& msg);
        
        // Expose underlying spdlog logger if advanced usage is needed
        static std::shared_ptr<spdlog::logger>& GetCoreLogger();
    };
}