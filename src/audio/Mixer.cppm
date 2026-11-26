module;

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

export module vortex.audio:mixer;

import vortex.core;
import :manifest;

export namespace vortex::audio {

    /// @brief A playing sound, as the mixer hands it out. Stale handles are ignored (the slot's serial moved on).
    struct Voice {
        uint32_t slot = 0xffffffffu;
        uint32_t serial = 0;
        bool Valid() const { return slot != 0xffffffffu; }
        friend bool operator==(const Voice&, const Voice&) = default;
    };

    /// @brief How to play a sound once.
    struct PlayParams {
        glm::dvec3 position{0.0};   ///< World metres (ignored when `flat`).
        /// Played at the listener, not placed: the interface, music, the own player's steps and casts (`2D`).
        bool flat = false;
        float volumeDb = 0.0f;      ///< Added to the sound's level (Size and Speed rules: sound-list.md section 2).
        float pitchSemitones = 0.0f;///< Added to the jitter (running steps, Size).
        float gain = 1.0f;          ///< Linear start value of the voice's own gain (Mixer::SetVoiceGain changes it later).
    };

    /// @brief What a backend sets on a voice: everything is final (bus gains are the backend's own).
    struct VoiceParams {
        float gain = 1.0f;          ///< linear, distance and the sound's level included, bus excluded
        float pitch = 1.0f;         ///< playback rate
        float pan = 0.0f;           ///< -1 left .. +1 right
        bool loop = false;
    };

    /**
     * @brief Where the mixer's decisions go: a device (miniaudio), or a test's recorder.
     * @details Slots are 0 .. MixerConfig::maxVoices - 1; the mixer never starts a slot that is busy.
     */
    class Backend {
    public:
        virtual ~Backend() = default;
        /// @brief The variant can start now: its file is there and, for a one-shot, decoded (a decode still running on a
        /// job thread is not ready: that play is skipped, so no voice ever waits on a decode). Asked before any voice
        /// is stolen for it.
        virtual bool Ready(SoundId sound, const SoundDesc& desc, uint32_t variant) = 0;
        /// @brief Starts variant `variant` (1-based) of `sound` in `slot`; false when it cannot (file missing or bad).
        virtual bool Start(uint32_t slot, SoundId sound, const SoundDesc& desc, uint32_t variant, const VoiceParams& params) = 0;
        virtual void Set(uint32_t slot, const VoiceParams& params) = 0;
        virtual void Stop(uint32_t slot) = 0;
        /// @brief Still sounding; false once a one-shot reached its end (the mixer then frees the slot).
        virtual bool Playing(uint32_t slot) const = 0;
        virtual void SetBusGain(Bus bus, float gain) = 0;
    };

    struct MixerConfig {
        uint32_t maxVoices = 32;          ///< all sounds together
        uint32_t maxPerSound = 4;         ///< voices of one ID
        float mergeSeconds = 0.05f;       ///< one ID within mergeMetres and this: one play, 2 dB louder
        float burstSeconds = 0.08f;       ///< one ID within mergeMetres and this: plays once
        float mergeMetres = 1.0f;
        float mergeBoostDb = 2.0f;
        float quietestDb = -40.0f;        ///< a sound quieter than this at the listener does not start
        /// Start budget of one ID: a bucket of `burstStarts` refilled at `startsPerSecond` (a collapse fires many
        /// breaks; sound-list.md section 18, the collapse budget).
        float startsPerSecond = 12.0f;
        float burstStarts = 6.0f;
        float farMetres = 80.0f;          ///< past this a `far` sound plays the far layer in its place
        uint64_t seed = 0x5eed5eedull;    ///< the audio random generator (never the simulation's)
    };

    /// @brief Counters for the exit line and the tests.
    struct MixerStats {
        uint64_t requests = 0;            ///< Play calls
        uint64_t started = 0;             ///< voices started
        uint64_t merged = 0;              ///< folded into a voice of the same ID (merge or burst rule)
        uint64_t stolen = 0;              ///< voices stopped for a louder or more important one
        uint64_t refusedFull = 0;         ///< no voice could be taken
        uint64_t refusedBudget = 0;       ///< the ID's start budget was spent
        uint64_t inaudible = 0;           ///< quieter than quietestDb at the listener
        uint64_t unknown = 0;             ///< not in the manifest
        uint64_t failed = 0;              ///< the backend could not start it, or its variant was not ready (missing, decoding)
        uint64_t farLayer = 0;            ///< played as the far layer
        uint32_t maxActive = 0;
    };

    /**
     * @brief The voice policy of the audio module (docs/design/audio.md "Voices"): variation, distance, merging,
     * budgets, the voice cap and stealing. It owns no device and decodes nothing: a Backend does that, so the
     * policy is unit-tested without an audio device.
     */
    class Mixer {
    public:
        Mixer(const Manifest& manifest, Backend& backend, const MixerConfig& config = {});

        void SetListener(const glm::dvec3& position, const glm::vec3& forward, const glm::vec3& up);
        const glm::dvec3& ListenerPosition() const { return m_ListenerPosition; }
        /// @brief Bus volume 0..1 (Master scales all).
        void SetBusVolume(Bus bus, float volume);
        float BusVolume(Bus bus) const { return m_BusVolume[static_cast<size_t>(bus)]; }

        Voice Play(SoundId sound, const PlayParams& params);
        void Stop(Voice voice);
        /// @brief Extra linear gain of a voice (fades of ambience beds); 1 = as started.
        void SetVoiceGain(Voice voice, float gain);
        bool IsPlaying(Voice voice) const;

        /// @brief Advances the clock, frees finished voices and re-places the others for the listener.
        void Update(float dt);

        uint32_t ActiveVoices() const;
        uint32_t ActiveVoicesOf(SoundId sound) const;
        const MixerStats& Stats() const { return m_Stats; }
        const MixerConfig& Config() const { return m_Config; }
        double Now() const { return m_Now; }

        /// @brief The variant (1-based) the next play of `sound` picks: never the last one; with 4 or more
        /// variants never the one before it either. Advances the generator (for tests).
        uint32_t PickVariant(SoundId sound);
        /// @brief Pitch (semitones) and volume (dB) jitter drawn for one play of `sound`.
        glm::vec2 DrawJitter(SoundId sound);

    private:
        struct Slot {
            bool used = false;
            uint32_t serial = 0;
            SoundId sound = kNoSound;
            uint8_t priority = 0;
            bool flat = false;
            bool loop = false;
            glm::dvec3 position{0.0};
            Distance distance = Distance::Flat;
            float levelDb = 0.0f;      // the sound's level plus the play's change and jitter
            float extraGain = 1.0f;    // SetVoiceGain
            float pitch = 1.0f;
            double started = 0.0;
            bool boosted = false;
        };
        struct Recent {
            SoundId sound = kNoSound;
            glm::dvec3 position{0.0};
            double time = -1.0e9;
            Voice voice;
        };
        struct PerSound {
            uint32_t last = 0, beforeLast = 0;   // variants played last (1-based; 0 = none)
            float tokens = -1.0f;                // start budget (-1: not yet filled)
            double tokensTime = 0.0;
        };

        uint32_t ChooseVariant(SoundId sound);       // PickVariant without recording it
        void CommitVariant(SoundId sound, uint32_t variant);
        float AudibleGain(const Slot& slot) const;   // linear, at the listener, buses included
        VoiceParams ParamsOf(const Slot& slot) const;
        uint32_t FindVictim(uint8_t priority, float gain, SoundId sameSound) const;
        void Free(uint32_t slot);

        const Manifest& m_Manifest;
        Backend& m_Backend;
        MixerConfig m_Config;
        Rng m_Rng;
        std::vector<Slot> m_Slots;
        std::vector<PerSound> m_PerSound;
        std::array<Recent, 64> m_Recent{};
        uint32_t m_RecentNext = 0;
        std::array<float, kBusCount> m_BusVolume{1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        glm::dvec3 m_ListenerPosition{0.0};
        glm::vec3 m_ListenerRight{1.0f, 0.0f, 0.0f};
        double m_Now = 0.0;
        SoundId m_FarLayer = kNoSound;
        MixerStats m_Stats;
    };

    /// @brief dB to a linear gain (and back; 0 gives -120 dB).
    float DbToGain(float db);
    float GainToDb(float gain);
}
