module;

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

module vortex.audio;

namespace vortex::audio {

    float DbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
    float GainToDb(float gain) { return gain <= 1.0e-6f ? -120.0f : 20.0f * std::log10(gain); }

    Mixer::Mixer(const Manifest& manifest, Backend& backend, const MixerConfig& config)
        : m_Manifest(manifest), m_Backend(backend), m_Config(config), m_Rng(config.seed) {
        m_Slots.resize(std::max(1u, config.maxVoices));
        m_PerSound.resize(manifest.Size());
        m_FarLayer = manifest.Find("sfx.blast.distant");
    }

    void Mixer::SetListener(const glm::dvec3& position, const glm::vec3& forward, const glm::vec3& up) {
        m_ListenerPosition = position;
        const glm::vec3 right = glm::cross(forward, up);
        const float length = glm::length(right);
        if (length > 1.0e-4f) m_ListenerRight = right / length;
    }

    void Mixer::SetBusVolume(Bus bus, float volume) {
        const float v = std::clamp(volume, 0.0f, 1.0f);
        m_BusVolume[static_cast<size_t>(bus)] = v;
        m_Backend.SetBusGain(bus, v);
    }

    uint32_t Mixer::ChooseVariant(SoundId sound) {
        const uint32_t n = m_Manifest.Get(sound).variants;
        const PerSound& ps = m_PerSound[sound];
        if (n <= 1) return 1;
        uint32_t candidates[32];
        uint32_t count = 0;
        for (uint32_t v = 1; v <= n && v <= 32; ++v) {
            if (v == ps.last) continue;
            if (n >= 4 && v == ps.beforeLast) continue;
            candidates[count++] = v;
        }
        return candidates[m_Rng.NextBounded(count)];
    }

    void Mixer::CommitVariant(SoundId sound, uint32_t variant) {
        PerSound& ps = m_PerSound[sound];
        ps.beforeLast = ps.last;
        ps.last = variant;
    }

    uint32_t Mixer::PickVariant(SoundId sound) {
        const uint32_t chosen = ChooseVariant(sound);
        CommitVariant(sound, chosen);
        return chosen;
    }

    glm::vec2 Mixer::DrawJitter(SoundId sound) {
        const SoundDesc& desc = m_Manifest.Get(sound);
        const float pitch = desc.pitchRange > 0.0f ? m_Rng.Range(-desc.pitchRange, desc.pitchRange) : 0.0f;
        const float volume = desc.volumeRange > 0.0f ? m_Rng.Range(-desc.volumeRange, desc.volumeRange) : 0.0f;
        return {pitch, volume};
    }

    float Mixer::AudibleGain(const Slot& slot) const {
        const float metres = slot.flat ? 0.0f : static_cast<float>(glm::distance(slot.position, m_ListenerPosition));
        const Bus bus = m_Manifest.Get(slot.sound).bus;
        return DbToGain(slot.levelDb) * slot.extraGain * DistanceGain(slot.flat ? Distance::Flat : slot.distance, metres) *
               m_BusVolume[static_cast<size_t>(bus)] * m_BusVolume[static_cast<size_t>(Bus::Master)];
    }

    VoiceParams Mixer::ParamsOf(const Slot& slot) const {
        VoiceParams p;
        p.pitch = slot.pitch;
        p.loop = slot.loop;
        float metres = 0.0f;
        if (!slot.flat) {
            const glm::dvec3 offset = slot.position - m_ListenerPosition;
            metres = static_cast<float>(glm::length(offset));
            // Pan by the side the sound is on; a source at the listener (or above it) stays centred. Never fully to
            // one ear: 0.85 keeps some of a side sound in the other ear, as a head does.
            if (metres > 0.5f) p.pan = std::clamp(glm::dot(glm::vec3(offset / static_cast<double>(metres)), m_ListenerRight), -1.0f, 1.0f) * 0.85f;
        }
        p.gain = DbToGain(slot.levelDb) * slot.extraGain * DistanceGain(slot.flat ? Distance::Flat : slot.distance, metres);
        return p;
    }

    uint32_t Mixer::ActiveVoices() const {
        uint32_t n = 0;
        for (const Slot& s : m_Slots) n += s.used ? 1u : 0u;
        return n;
    }

    uint32_t Mixer::ActiveVoicesOf(SoundId sound) const {
        uint32_t n = 0;
        for (const Slot& s : m_Slots) n += (s.used && s.sound == sound) ? 1u : 0u;
        return n;
    }

    uint32_t Mixer::FindVictim(uint8_t priority, float gain, SoundId sameSound) const {
        uint32_t best = 0xffffffffu;
        uint8_t bestPriority = 0xff;
        float bestGain = 0.0f;
        for (uint32_t i = 0; i < m_Slots.size(); ++i) {
            const Slot& s = m_Slots[i];
            if (!s.used) continue;
            if (sameSound != kNoSound && s.sound != sameSound) continue;
            const float g = AudibleGain(s);
            // A lower priority always gives way; the same priority only to a louder sound; a loop (a bed, a winch)
            // only to a higher priority.
            // Within one ID (its cap) only loudness counts.
            const bool takeable = sameSound != kNoSound ? (!s.loop && g < gain)
                                                        : (s.priority < priority || (s.priority == priority && !s.loop && g < gain));
            if (!takeable) continue;
            if (best == 0xffffffffu || s.priority < bestPriority || (s.priority == bestPriority && g < bestGain)) {
                best = i;
                bestPriority = s.priority;
                bestGain = g;
            }
        }
        return best;
    }

    void Mixer::Free(uint32_t slot) {
        m_Slots[slot].used = false;
        ++m_Slots[slot].serial;
    }

    Voice Mixer::Play(SoundId sound, const PlayParams& params) {
        ++m_Stats.requests;
        if (sound >= m_Manifest.Size()) {
            ++m_Stats.unknown;
            return {};
        }
        SoundId playing = sound;
        Distance distance = m_Manifest.Get(sound).distance;
        const bool flat = params.flat || distance == Distance::Flat;
        if (!flat && m_Manifest.Get(sound).far && m_FarLayer != kNoSound &&
            glm::distance(params.position, m_ListenerPosition) > static_cast<double>(m_Config.farMetres)) {
            playing = m_FarLayer;
            distance = Distance::Huge;
            ++m_Stats.farLayer;
        }
        const SoundDesc& desc = m_Manifest.Get(playing);

        // One ID at one place within the burst time plays once; within the merge time the voice gets louder.
        for (const Recent& r : m_Recent) {
            if (r.sound != playing || m_Now - r.time > static_cast<double>(m_Config.burstSeconds)) continue;
            if (!flat && glm::distance(r.position, params.position) > static_cast<double>(m_Config.mergeMetres)) continue;
            ++m_Stats.merged;
            if (m_Now - r.time <= static_cast<double>(m_Config.mergeSeconds) && IsPlaying(r.voice)) {
                Slot& s = m_Slots[r.voice.slot];
                if (!s.boosted) {
                    s.boosted = true;
                    s.levelDb += m_Config.mergeBoostDb;
                    m_Backend.Set(r.voice.slot, ParamsOf(s));
                }
            }
            return r.voice;
        }

        // The start budget of the ID (loops are started by hand, once).
        PerSound& ps = m_PerSound[playing];
        if (!desc.loop) {
            if (ps.tokens < 0.0f) {
                ps.tokens = m_Config.burstStarts;
                ps.tokensTime = m_Now;
            }
            ps.tokens = std::min(m_Config.burstStarts, ps.tokens + static_cast<float>(m_Now - ps.tokensTime) * m_Config.startsPerSecond);
            ps.tokensTime = m_Now;
            if (ps.tokens < 1.0f) {
                ++m_Stats.refusedBudget;
                return {};
            }
        }

        const glm::vec2 jitter = DrawJitter(playing);
        Slot candidate;
        candidate.used = true;
        candidate.sound = playing;
        candidate.priority = desc.priority;
        candidate.flat = flat;
        candidate.loop = desc.loop;
        candidate.position = params.position;
        candidate.distance = distance;
        candidate.levelDb = desc.gainDb + params.volumeDb + jitter.y;
        candidate.extraGain = std::max(0.0f, params.gain);
        candidate.pitch = std::pow(2.0f, (jitter.x + params.pitchSemitones) / 12.0f);
        candidate.started = m_Now;
        const float gain = AudibleGain(candidate);
        // A loop (a bed fading in from nothing, a winch) starts whatever its level: it is started once and faded.
        if (!desc.loop && GainToDb(gain) < m_Config.quietestDb) {
            ++m_Stats.inaudible;
            return {};
        }

        // The caps: at most maxPerSound voices of the ID, at most maxVoices in all. When full, the quietest of what
        // may give way stops; if nothing may, the new sound does not start.
        uint32_t victim = 0xffffffffu;
        if (ActiveVoicesOf(playing) >= m_Config.maxPerSound) {
            victim = FindVictim(desc.priority, gain, playing);
            if (victim == 0xffffffffu) {
                ++m_Stats.refusedFull;
                return {};
            }
        } else if (ActiveVoices() >= m_Slots.size()) {
            victim = FindVictim(desc.priority, gain, kNoSound);
            if (victim == 0xffffffffu) {
                ++m_Stats.refusedFull;
                return {};
            }
        }
        // Nothing is stolen for a variant that cannot start (missing, or still decoding on the job thread).
        const uint32_t variant = ChooseVariant(playing);
        if (!m_Backend.Ready(playing, desc, variant)) {
            ++m_Stats.failed;
            return {};
        }
        if (victim != 0xffffffffu) {
            m_Backend.Stop(victim);
            Free(victim);
            ++m_Stats.stolen;
        }
        uint32_t slot = 0;
        while (slot < m_Slots.size() && m_Slots[slot].used) ++slot;

        candidate.serial = m_Slots[slot].serial + 1;
        if (!m_Backend.Start(slot, playing, desc, variant, ParamsOf(candidate))) {
            ++m_Stats.failed;
            return {};
        }
        CommitVariant(playing, variant);
        if (!desc.loop) ps.tokens -= 1.0f;
        m_Slots[slot] = candidate;
        const Voice voice{slot, candidate.serial};
        m_Recent[m_RecentNext] = Recent{playing, params.position, m_Now, voice};
        m_RecentNext = (m_RecentNext + 1) % static_cast<uint32_t>(m_Recent.size());
        ++m_Stats.started;
        m_Stats.maxActive = std::max(m_Stats.maxActive, ActiveVoices());
        return voice;
    }

    bool Mixer::IsPlaying(Voice voice) const {
        return voice.slot < m_Slots.size() && m_Slots[voice.slot].used && m_Slots[voice.slot].serial == voice.serial;
    }

    void Mixer::Stop(Voice voice) {
        if (!IsPlaying(voice)) return;
        m_Backend.Stop(voice.slot);
        Free(voice.slot);
    }

    void Mixer::SetVoiceGain(Voice voice, float gain) {
        if (!IsPlaying(voice)) return;
        Slot& s = m_Slots[voice.slot];
        s.extraGain = std::max(0.0f, gain);
        m_Backend.Set(voice.slot, ParamsOf(s));
    }

    void Mixer::Update(float dt) {
        m_Now += static_cast<double>(std::max(dt, 0.0f));
        for (uint32_t i = 0; i < m_Slots.size(); ++i) {
            Slot& s = m_Slots[i];
            if (!s.used) continue;
            if (!m_Backend.Playing(i)) {
                m_Backend.Stop(i);
                Free(i);
                continue;
            }
            if (!s.flat) m_Backend.Set(i, ParamsOf(s));   // the listener moved
        }
    }
}
