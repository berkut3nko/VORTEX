module;

#include <string>

export module vortex.core:profiller;

namespace vortex::core {

    /**
     * @brief Static class for performance profiling and timing.
     * @details Collects execution times of named sections and renders them using ImGui.
     * Stores history for real-time graphing.
     */
    export class Profiler {
    public:
        /**
         * @brief Clears frame data and prepares for new measurements.
         * @details Should be called at the very beginning of the frame.
         */
        static void BeginFrame();

        /**
         * @brief Starts measuring time for a named section (CPU).
         * @param name Unique name of the section (used for display).
         */
        static void Begin(const std::string& name);

        /**
         * @brief Stops measuring time for the named section (CPU).
         * @details Calculates duration and adds it to the history.
         * @param name Name of the section to stop.
         */
        static void End(const std::string& name);

        /**
         * @brief Adds a manual time sample (e.g., for GPU timers queries).
         * @param name Name of the metric.
         * @param timeMs Time in milliseconds.
         */
        static void AddSample(const std::string& name, float timeMs);

        /**
         * @brief Renders the profiling data as ImGui graphs.
         * @details Creates a new ImGui window "Performance Profiler".
         */
        static void Render();
    };

    /**
     * @brief RAII wrapper for automatic profiling scopes.
     * @details Automatically calls Profiler::Begin in constructor and Profiler::End in destructor.
     * Usage: `ProfileScope scope("MyFunction");`
     */
    export struct ProfileScope {
        std::string name;

        /**
         * @brief Starts a profiling scope.
         * @param n Name of the scope.
         */
        explicit ProfileScope(const std::string& n) : name(n) {
            Profiler::Begin(name);
        }

        /**
         * @brief Ends the profiling scope automatically.
         */
        ~ProfileScope() {
            Profiler::End(name);
        }
    };
}