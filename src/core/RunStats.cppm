module;

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module vortex.core:runstats;

export namespace vortex {

    /// @brief Run-wide statistics of one timed quantity (ms): exact nearest-rank percentiles over the samples
    /// kept (at most kMaxSamples), plus count, max and budget counts. GPU-free; not thread safe.
    class RunStats {
    public:
        static constexpr uint64_t kMaxSamples = uint64_t{1} << 20;

        /// @brief Adds one sample in milliseconds. A non-finite value is ignored.
        void Add(double ms);
        /// @brief Samples added (also those past kMaxSamples).
        uint64_t Count() const;
        /// @brief Nearest-rank percentile, p in 0..100 (clamped), over the stored samples; 0 when empty.
        double Percentile(uint32_t p) const;
        /// @brief Largest sample ever added; 0 when empty.
        double Max() const;
        /// @brief Stored samples strictly greater than `ms`.
        uint64_t CountOver(double ms) const;
        void Clear();

    private:
        std::vector<float> m_Samples;
        uint64_t m_Count = 0;
        double m_Max = 0.0;
    };

    /// @brief What the `Perf:` exit line reports (Engine::Run fills it, one sample per frame).
    struct PerfSummary {
        RunStats cpuFrame;        ///< Wall time of one pass of the frame loop.
        RunStats cpuWorld;        ///< The "CPU: world" section.
        RunStats cpuSimulation;   ///< The "CPU: simulation" section.
        RunStats fenceWait;       ///< Renderer: the wait for the frame slot's fence.
        /// GPU passes in the order first seen (the renderer's pass order), "upload" included.
        std::vector<std::pair<std::string, RunStats>> gpuPasses;

        /// @brief The statistics of GPU pass `name`, appended at the end the first time.
        RunStats& GpuPass(std::string_view name);
        /// @brief nullptr if `name` was never added.
        const RunStats* FindGpuPass(std::string_view name) const;
    };

    /**
     * @brief The `Perf:` line printed at the end of every run (docs/verify.md):
     * `Perf: F frames; CPU frame p50 a p99 b max c ms, world p50 .. p99 .. ms, simulation p50 .. p99 .. ms,
     * fence wait p50 .. p99 .. ms; over 25 ms j, over 33 ms k; GPU sum p50 S ms (pass x, ...); upload p50 u ms;
     * present MODE`. The GPU list skips "upload" (its own field); S is the sum of the listed p50 values.
     * `presentMode`: "headless", "fifo", "fifo-relaxed", ...
     */
    std::string FormatPerfLine(const PerfSummary& perf, std::string_view presentMode);

    /// @brief What the `Fx:` exit line reports (Engine fills it; one sample per blast or per frame as noted).
    struct FxSummary {
        uint32_t blasts = 0;            ///< Engine::PresentBlast calls (late searches included).
        uint32_t skipped = 0;           ///< --stress-fx blasts whose 15 rays all missed.
        RunStats carve, search, neck, ops, objects, present;   ///< per blast, from sim::BlastTimes and the present time
        RunStats particlesUpdate;       ///< per frame: ParticleSystem::Update
        RunStats particlesBuild;        ///< per frame: ParticleSystem::BuildRenderData
        uint32_t particlesMax = 0;      ///< most particles alive in one frame
        uint32_t particlesCapacity = 0; ///< the particle pool's capacity
        uint32_t effectLightsMax = 0;   ///< most effect lights after the merge in one frame ([wp-8] card 4)
    };

    /**
     * @brief The `Fx:` line printed at the end of a run that presented at least one blast:
     * `Fx: B blasts (S skipped); carve p50 a p99 b max c ms, search ..., neck ..., ops ..., objects ..., present
     * ...; particles update p50 .. p99 .. ms, build p50 .. p99 .. ms, alive at most M of C; effect lights at most E`.
     */
    std::string FormatFxLine(const FxSummary& fx);
}
