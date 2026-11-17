module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

module vortex.net;

namespace vortex::net {

    uint32_t TickClock::TicksDue(double seconds, double tickSeconds, uint32_t maxTicks) {
        if (std::isfinite(seconds) && seconds > 0.0 && tickSeconds > 0.0) m_Accumulator += seconds * m_Rate;
        uint64_t due = 0;
        if (tickSeconds > 0.0 && m_Accumulator >= tickSeconds) {
            due = static_cast<uint64_t>(m_Accumulator / tickSeconds);
            m_Accumulator -= static_cast<double>(due) * tickSeconds;
        }
        due += m_PendingJump;
        m_PendingJump = 0;
        const uint64_t hold = std::min<uint64_t>(due, m_PendingHold);
        due -= hold;
        m_PendingHold -= static_cast<uint32_t>(hold);
        if (due > maxTicks) {
            m_PendingJump = static_cast<uint32_t>(std::min<uint64_t>(due - maxTicks, 1u << 20));
            due = maxTicks;
        }
        return static_cast<uint32_t>(due);
    }

    int32_t TickClock::Target() const {
        return m_Settings.baseMargin + std::max(0, m_Settings.packetTicks - 1) +
               static_cast<int32_t>(std::ceil(m_Settings.jitterFactor * m_Jitter - 1.0e-4f));
    }

    ClockAction TickClock::Report(int32_t margin, uint32_t inputTick, uint32_t nextTick, uint32_t snapshotTick) {
        ClockAction action;
        // An input made before the last jump or hold does not show it yet; a report about the same
        // input again (no newer input arrived: a snapshot repeats the last margin) says nothing new.
        if (inputTick < m_IgnoreBefore) return action;
        if (m_HasPrevious && inputTick <= m_LastInputTick) return action;
        m_LastInputTick = inputTick;
        if (m_HasPrevious) {
            const float change = static_cast<float>(std::abs(margin - m_Previous));
            m_Jitter += (std::min(change, 30.0f) - m_Jitter) / 16.0f;
        }
        m_HasPrevious = true;
        m_Previous = margin;
        const int32_t target = Target();

        // Late (below 0), or short of the target by shortBy or more, in lateReports consecutive
        // reports: jump forward at once - by what the latest late margin misses, or by what the
        // least short of the run misses (a ping step must not take seconds of catch-up).
        m_LateRun = margin < 0 ? m_LateRun + 1 : 0;
        const bool shortNow = margin <= target - m_Settings.shortBy;
        m_ShortHighest = shortNow ? (m_ShortRun == 0 ? margin : std::max(m_ShortHighest, margin)) : 0;
        m_ShortRun = shortNow ? m_ShortRun + 1 : 0;
        const bool late = m_LateRun >= m_Settings.lateReports;
        if (late || m_ShortRun >= m_Settings.lateReports) {
            action.jumpTicks = static_cast<uint32_t>(target - (late ? margin : m_ShortHighest));
            m_PendingJump += action.jumpTicks;
            m_LateRun = 0;
            m_ShortRun = 0;
            m_IgnoreBefore = nextTick;
            m_Window.clear();
            m_High = false;
            m_Rate = 1.0;
            ++(late ? m_LateJumps : m_ShortJumps);
            return action;
        }

        // A surplus: above the resync bound and above the target (with jitter the target can be
        // above the bound, and a margin under it is no surplus). The hold is the surplus, > 0.
        if (margin > std::max(m_Settings.resyncAbove, target)) {
            if (!m_High) {
                m_High = true;
                m_HighSince = snapshotTick;
            } else if (snapshotTick - m_HighSince >= m_Settings.resyncTicks) {
                action.holdTicks = static_cast<uint32_t>(margin - target);
                m_PendingHold += action.holdTicks;
                m_High = false;
                m_IgnoreBefore = nextTick;   // no tick runs until the hold is spent: the next input made shows it
                m_Window.clear();
                m_Rate = 1.0;
                return action;
            }
        } else {
            m_High = false;
        }

        m_Window.push_back(margin);
        while (m_Window.size() > m_Settings.surplusWindow) m_Window.pop_front();
        const int32_t lowest = *std::min_element(m_Window.begin(), m_Window.end());
        m_Rate = 1.0;
        if (m_Window.size() >= m_Settings.surplusWindow) {
            if (lowest > target) m_Rate = m_Settings.bleedRate;
            else if (lowest < target && lowest >= 0) m_Rate = m_Settings.catchUpRate;
        }
        return action;
    }

    void ServerTime::Advance(double ticks) {
        if (!std::isfinite(ticks) || ticks <= 0.0) return;
        m_Local += ticks;
        if (!m_Started) return;
        const double error = m_Local + m_Offset - m_Now;
        m_Now += ticks * std::clamp(1.0 + kGain * error, kMinRate, kMaxRate);
    }

    void ServerTime::Observe(uint32_t snapshotTick) {
        const double sample = static_cast<double>(snapshotTick) - m_Local;
        if (!m_Started) {
            m_Started = true;
            m_Offset = sample;
            m_Now = static_cast<double>(snapshotTick);
            return;
        }
        m_Offset += (sample - m_Offset) * kSmoothing;
        const double estimate = m_Local + m_Offset;
        if (estimate - m_Now > kSnapTicks) m_Now = estimate;
    }

    void RemotePlayers::Add(uint32_t tick, const std::vector<RemotePlayer>& others) {
        for (auto it = m_Tracks.begin(); it != m_Tracks.end();) {
            const bool present = std::any_of(others.begin(), others.end(), [&it](const RemotePlayer& p) { return p.slot == it->first; });
            it = present ? std::next(it) : m_Tracks.erase(it);
        }
        for (const RemotePlayer& p : others) {
            Track& t = m_Tracks[p.slot];
            if (!t.samples.empty()) {
                if (tick <= t.samples.back().tick) continue;
                const double gap = static_cast<double>(tick - t.samples.back().tick);
                t.interval += (std::min(gap, 60.0) - t.interval) / 8.0;
            }
            t.samples.push_back({tick, p.position, p.yaw, p.flags});
            while (t.samples.size() > kSamples) t.samples.pop_front();
        }
    }

    std::vector<RemotePlayerView> RemotePlayers::Sample(double serverTick) const {
        std::vector<RemotePlayerView> out;
        for (const auto& [slot, t] : m_Tracks) {
            if (t.samples.empty()) continue;
            const double delay = std::clamp(2.0 * t.interval, kMinDelayTicks, kMaxDelayTicks);
            const double at = serverTick - delay;
            RemotePlayerView v;
            v.slot = slot;
            const RemoteSample& first = t.samples.front();
            const RemoteSample& last = t.samples.back();
            if (at <= static_cast<double>(first.tick)) {
                v.position = first.position;
                v.yaw = first.yaw;
                v.flags = first.flags;
            } else if (at >= static_cast<double>(last.tick)) {
                // Extrapolate along the last motion for up to one update interval, then hold.
                v.position = last.position;
                v.yaw = last.yaw;
                v.flags = last.flags;
                if (t.samples.size() >= 2) {
                    const RemoteSample& prev = t.samples[t.samples.size() - 2];
                    const double span = static_cast<double>(last.tick - prev.tick);
                    const double ahead = std::min(at - static_cast<double>(last.tick), t.interval);
                    if (span > 0.0) v.position += (last.position - prev.position) * static_cast<float>(ahead / span);
                }
            } else {
                for (size_t i = 1; i < t.samples.size(); ++i) {
                    const RemoteSample& b = t.samples[i];
                    if (static_cast<double>(b.tick) < at) continue;
                    const RemoteSample& a = t.samples[i - 1];
                    const float f = static_cast<float>((at - static_cast<double>(a.tick)) / static_cast<double>(b.tick - a.tick));
                    v.position = glm::mix(a.position, b.position, f);
                    v.yaw = f < 0.5f ? a.yaw : b.yaw;
                    v.flags = f < 0.5f ? a.flags : b.flags;
                    break;
                }
            }
            out.push_back(v);
        }
        return out;
    }

    bool RemoteBodies::Add(uint32_t netId, uint32_t tick, const BodyPose& pose) {
        Track& t = m_Tracks[netId];
        if (t.rested && tick <= t.restTick) return false;
        if (!t.samples.empty()) {
            const uint32_t newest = t.samples.back().tick;
            if (tick <= newest) return false;
            const uint32_t gap = tick - newest;
            t.maxGap = std::max(t.maxGap, gap);
            m_MaxGap = std::max(m_MaxGap, gap);
            t.interval += (std::min(static_cast<double>(gap), 60.0) - t.interval) / 8.0;
        }
        t.samples.push_back({tick, pose});
        ++t.received;
        while (t.samples.size() > kSamples) t.samples.pop_front();
        return true;
    }

    bool RemoteBodies::Rest(uint32_t netId, uint32_t tick) {
        Track& t = m_Tracks[netId];
        // The tail: from the last sample to the rest (a body that stopped being sent long before it
        // came to rest shows here).
        if (!t.samples.empty() && tick > t.samples.back().tick) {
            const uint32_t gap = tick - t.samples.back().tick;
            t.maxGap = std::max(t.maxGap, gap);
            m_MaxGap = std::max(m_MaxGap, gap);
        }
        while (!t.samples.empty() && t.samples.front().tick <= tick) t.samples.pop_front();
        t.rested = true;
        t.restTick = std::max(t.restTick, tick);
        return t.samples.empty();
    }

    uint32_t RemoteBodies::Received(uint32_t netId) const {
        const auto it = m_Tracks.find(netId);
        return it != m_Tracks.end() ? it->second.received : 0u;
    }

    void RemoteBodies::Remove(uint32_t netId) { m_Tracks.erase(netId); }

    void RemoteBodies::Shift(uint32_t netId, const glm::vec3& localOffset) {
        const auto it = m_Tracks.find(netId);
        if (it == m_Tracks.end()) return;
        for (Sample_& s : it->second.samples) s.pose.position += s.pose.rotation * localOffset;
    }

    bool RemoteBodies::HasSamples(uint32_t netId) const {
        const auto it = m_Tracks.find(netId);
        return it != m_Tracks.end() && !it->second.samples.empty();
    }

    bool RemoteBodies::Oldest(uint32_t netId, uint32_t& tick, BodyPose& pose) const {
        const auto it = m_Tracks.find(netId);
        if (it == m_Tracks.end() || it->second.samples.empty()) return false;
        tick = it->second.samples.front().tick;
        pose = it->second.samples.front().pose;
        return true;
    }

    uint32_t RemoteBodies::MaxGap(uint32_t netId) const {
        const auto it = m_Tracks.find(netId);
        return it != m_Tracks.end() ? it->second.maxGap : 0u;
    }

    bool RemoteBodies::Sample(uint32_t netId, double serverTick, double tickSeconds, BodyPose& out) const {
        const auto it = m_Tracks.find(netId);
        if (it == m_Tracks.end() || it->second.samples.empty()) return false;
        const Track& t = it->second;
        const double delay = std::clamp(2.0 * t.interval, kMinDelayTicks, kMaxDelayTicks);
        const double at = serverTick - delay;
        const Sample_& first = t.samples.front();
        const Sample_& last = t.samples.back();
        if (at <= static_cast<double>(first.tick)) {
            out = first.pose;
            return true;
        }
        if (at >= static_cast<double>(last.tick)) {
            // Along the last velocity for up to one update interval, then hold.
            const double ahead = std::min(at - static_cast<double>(last.tick), t.interval);
            out = last.pose;
            out.position += last.pose.velocity * static_cast<float>(ahead * tickSeconds);
            return true;
        }
        for (size_t i = 1; i < t.samples.size(); ++i) {
            const Sample_& b = t.samples[i];
            if (static_cast<double>(b.tick) < at) continue;
            const Sample_& a = t.samples[i - 1];
            const double span = static_cast<double>(b.tick - a.tick);
            const float f = static_cast<float>((at - static_cast<double>(a.tick)) / span);
            // Cubic Hermite on position with the sampled velocities as tangents (in metres per span).
            const float h = static_cast<float>(span * tickSeconds);
            const float f2 = f * f, f3 = f2 * f;
            const float h00 = 2.0f * f3 - 3.0f * f2 + 1.0f, h10 = f3 - 2.0f * f2 + f, h01 = -2.0f * f3 + 3.0f * f2, h11 = f3 - f2;
            out.position = h00 * a.pose.position + h10 * h * a.pose.velocity + h01 * b.pose.position + h11 * h * b.pose.velocity;
            out.rotation = glm::slerp(a.pose.rotation, b.pose.rotation, f);
            out.velocity = glm::mix(a.pose.velocity, b.pose.velocity, f);
            return true;
        }
        out = last.pose;
        return true;
    }
}
