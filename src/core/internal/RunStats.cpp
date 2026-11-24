module;

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

module vortex.core;

namespace vortex {

    void RunStats::Add(double ms) {
        if (!std::isfinite(ms)) return;
        ++m_Count;
        if (m_Count == 1 || ms > m_Max) m_Max = ms;
        if (m_Samples.size() < kMaxSamples) m_Samples.push_back(static_cast<float>(ms));
    }

    uint64_t RunStats::Count() const { return m_Count; }

    double RunStats::Percentile(uint32_t p) const {
        const uint64_t n = m_Samples.size();
        if (n == 0) return 0.0;
        p = std::min<uint32_t>(p, 100);
        // Nearest rank: ceil(p * n / 100), at least 1.
        uint64_t rank = (static_cast<uint64_t>(p) * n + 99) / 100;
        if (rank < 1) rank = 1;
        std::vector<float> copy = m_Samples;
        const auto nth = copy.begin() + static_cast<std::ptrdiff_t>(rank - 1);
        std::nth_element(copy.begin(), nth, copy.end());
        return static_cast<double>(*nth);
    }

    double RunStats::Max() const { return m_Count != 0 ? m_Max : 0.0; }

    uint64_t RunStats::CountOver(double ms) const {
        return static_cast<uint64_t>(std::count_if(m_Samples.begin(), m_Samples.end(), [ms](float s) { return static_cast<double>(s) > ms; }));
    }

    void RunStats::Clear() {
        m_Samples.clear();
        m_Count = 0;
        m_Max = 0.0;
    }

    RunStats& PerfSummary::GpuPass(std::string_view name) {
        for (auto& [passName, stats] : gpuPasses)
            if (passName == name) return stats;
        return gpuPasses.emplace_back(std::string(name), RunStats{}).second;
    }

    const RunStats* PerfSummary::FindGpuPass(std::string_view name) const {
        for (const auto& [passName, stats] : gpuPasses)
            if (passName == name) return &stats;
        return nullptr;
    }

    std::string FormatPerfLine(const PerfSummary& perf, std::string_view presentMode) {
        std::string passes;
        double sum = 0.0;
        for (const auto& [name, stats] : perf.gpuPasses) {
            if (name == "upload") continue;
            const double p50 = stats.Percentile(50);
            sum += p50;
            if (!passes.empty()) passes += ", ";
            passes += std::format("{} {:.2f}", name, p50);
        }
        if (passes.empty()) passes = "none";
        const RunStats* upload = perf.FindGpuPass("upload");
        const RunStats& f = perf.cpuFrame;
        return std::format("Perf: {} frames; CPU frame p50 {:.2f} p99 {:.2f} max {:.2f} ms, world p50 {:.2f} p99 {:.2f} ms, simulation p50 {:.2f} "
                           "p99 {:.2f} ms, fence wait p50 {:.2f} p99 {:.2f} ms; over 25 ms {}, over 33 ms {}; GPU sum p50 {:.2f} ms ({}); "
                           "upload p50 {:.2f} ms; present {}",
                           f.Count(), f.Percentile(50), f.Percentile(99), f.Max(), perf.cpuWorld.Percentile(50), perf.cpuWorld.Percentile(99),
                           perf.cpuSimulation.Percentile(50), perf.cpuSimulation.Percentile(99), perf.fenceWait.Percentile(50),
                           perf.fenceWait.Percentile(99), f.CountOver(25.0), f.CountOver(33.0), sum, passes,
                           upload != nullptr ? upload->Percentile(50) : 0.0, presentMode);
    }

    namespace {
        /// `p50 a p99 b max c ms` of one per-blast phase of the `Fx:` line.
        std::string BlastPhase(const RunStats& r) {
            return std::format("p50 {:.2f} p99 {:.2f} max {:.2f} ms", r.Percentile(50), r.Percentile(99), r.Max());
        }
    }

    std::string FormatFxLine(const FxSummary& fx) {
        return std::format("Fx: {} blasts ({} skipped); carve {}, search {}, neck {}, ops {}, objects {}, present {}; particles update p50 {:.2f} "
                           "p99 {:.2f} ms, build p50 {:.2f} p99 {:.2f} ms, alive at most {} of {}; effect lights at most {}",
                           fx.blasts, fx.skipped, BlastPhase(fx.carve), BlastPhase(fx.search), BlastPhase(fx.neck), BlastPhase(fx.ops),
                           BlastPhase(fx.objects), BlastPhase(fx.present), fx.particlesUpdate.Percentile(50), fx.particlesUpdate.Percentile(99),
                           fx.particlesBuild.Percentile(50), fx.particlesBuild.Percentile(99), fx.particlesMax, fx.particlesCapacity,
                           fx.effectLightsMax);
    }
}
