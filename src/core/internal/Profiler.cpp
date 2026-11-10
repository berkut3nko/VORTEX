module;

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

module vortex.core;

namespace vortex {

    namespace {
        struct TimerState {
            Profiler::Timer timer;
            float pendingMs = 0.0f;   // accumulated during the current frame
            bool touched = false;
        };

        std::mutex g_Mutex;
        std::vector<TimerState> g_Timers;
        std::vector<Profiler::Counter> g_Counters;
    }

    void Profiler::AddSample(std::string_view name, float milliseconds) {
        std::scoped_lock lock(g_Mutex);
        for (auto& t : g_Timers) {
            if (t.timer.name == name) {
                t.pendingMs += milliseconds;
                t.touched = true;
                return;
            }
        }
        TimerState state;
        state.timer.name = std::string(name);
        state.pendingMs = milliseconds;
        state.touched = true;
        g_Timers.push_back(std::move(state));
    }

    void Profiler::SetCounter(std::string_view name, double value) {
        std::scoped_lock lock(g_Mutex);
        for (auto& c : g_Counters) {
            if (c.name == name) {
                c.value = value;
                return;
            }
        }
        g_Counters.push_back({std::string(name), value});
    }

    void Profiler::EndFrame() {
        std::scoped_lock lock(g_Mutex);
        for (auto& t : g_Timers) {
            Timer& timer = t.timer;
            const float sample = t.touched ? t.pendingMs : 0.0f;
            timer.lastMs = sample;
            timer.history[static_cast<size_t>(timer.cursor)] = sample;
            timer.cursor = (timer.cursor + 1) % kHistory;
            timer.samples = std::min(timer.samples + 1, kHistory);

            float sum = 0.0f;
            float peak = 0.0f;
            for (int i = 0; i < timer.samples; ++i) {
                sum += timer.history[static_cast<size_t>(i)];
                peak = std::max(peak, timer.history[static_cast<size_t>(i)]);
            }
            timer.avgMs = sum / static_cast<float>(timer.samples);
            timer.maxMs = peak;

            t.pendingMs = 0.0f;
            t.touched = false;
        }
    }

    std::vector<Profiler::Timer> Profiler::SnapshotTimers() {
        std::scoped_lock lock(g_Mutex);
        std::vector<Timer> out;
        out.reserve(g_Timers.size());
        for (const auto& t : g_Timers) out.push_back(t.timer);
        return out;
    }

    std::vector<Profiler::Counter> Profiler::SnapshotCounters() {
        std::scoped_lock lock(g_Mutex);
        return g_Counters;
    }
}
