module;

#include <unordered_map>
#include <map>
#include <vector>
#include <string>
#include <chrono>
#include <imgui.h>
#include <algorithm>
#include <mutex>

module vortex.core;

namespace vortex::core {

    struct ProfilerData {
        using Clock = std::chrono::high_resolution_clock;
        
        // Active timers for the current frame
        std::unordered_map<std::string, Clock::time_point> startTimes;
        
        // History of durations for graphs (Name -> Array of milliseconds)
        std::map<std::string, std::vector<float>> history;
        
        // Max frames to keep in history graph
        static constexpr int HISTORY_SIZE = 100;
    };

    static ProfilerData s_Data;

    void Profiler::BeginFrame() {
        // Optional: Reset per-frame counters if needed
    }

    void Profiler::Begin(const std::string& name) {
        s_Data.startTimes[name] = ProfilerData::Clock::now();
    }

    void Profiler::End(const std::string& name) {
        auto endTime = ProfilerData::Clock::now();
        auto it = s_Data.startTimes.find(name);
        
        if (it != s_Data.startTimes.end()) {
            float duration = std::chrono::duration<float, std::milli>(endTime - it->second).count();
            AddSample(name, duration);
            s_Data.startTimes.erase(it);
        }
    }

    void Profiler::AddSample(const std::string& name, float timeMs) {
        auto& histVec = s_Data.history[name];
        if (histVec.size() >= ProfilerData::HISTORY_SIZE) {
            histVec.erase(histVec.begin());
        }
        histVec.push_back(timeMs);
    }

    void Profiler::Render() {
        if (ImGui::Begin("Performance Profiler")) {
            if (s_Data.history.empty()) {
                ImGui::Text("No profiling data available.");
            } else {
                for (const auto& [name, values] : s_Data.history) {
                    if (values.empty()) continue;

                    float current = values.back();
                    float avg = 0.0f;
                    float maxVal = 0.0f;
                    for (float v : values) {
                        avg += v;
                        if (v > maxVal) maxVal = v;
                    }
                    avg /= values.size();

                    ImGui::Text("%s: %.3f ms (Avg: %.3f ms)", name.c_str(), current, avg);
                    
                    ImGui::PlotLines(
                        ("##" + name).c_str(), 
                        values.data(), 
                        (int)values.size(), 
                        0, 
                        nullptr, 
                        0.0f, 
                        maxVal * 1.1f, 
                        ImVec2(0, 40)
                    );
                    ImGui::Separator();
                }
            }
        }
        ImGui::End();
    }

}