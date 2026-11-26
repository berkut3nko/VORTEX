module;

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module vortex.audio:system;

import vortex.core;
import :manifest;
import :mixer;

export namespace vortex::audio {

    /// @brief How the audio system starts (docs/design/audio.md "Devices").
    enum class AudioOutput : uint8_t {
        Device = 0,    ///< the default playback device; falls back to Offline when there is none
        Offline = 1,   ///< no device: the engine mixes in Update (headless runs: the whole path runs, nothing is heard)
        Off = 2,       ///< nothing is loaded or mixed (--no-audio)
    };

    struct AudioConfig {
        AudioOutput output = AudioOutput::Device;
        std::filesystem::path root;          ///< the folder of manifest.txt
        MixerConfig mixer;
        std::array<float, kBusCount> volumes{1.0f, 1.0f, 1.0f, 1.0f, 1.0f};   ///< by Bus
    };

    /// @brief The run's numbers for the exit line.
    struct AudioSummary {
        MixerStats mixer;
        uint32_t sounds = 0, files = 0, missingFiles = 0;
        double assetMegabytes = 0.0;
        uint32_t decodedFiles = 0;           ///< variants decoded into memory so far
        double decodedMegabytes = 0.0;
        double mixedSeconds = 0.0;           ///< Offline: sound mixed by Update
        double mixMs = 0.0;                  ///< Offline: time spent mixing it
        double updateMsMax = 0.0;            ///< main-thread cost of one Update (policy + backend calls)
        double playUsMax = 0.0;              ///< main-thread cost of one Play (first decode of a variant included)
    };

    /**
     * @brief The engine's sound: the manifest, the mixer and one backend (miniaudio: a device, or offline).
     * @details Presentation only, on the main thread. Never touched by `vortex.sim`: the game and the engine call
     * it from the places that make pictures (Engine::PresentBlast and friends). Every call is safe while Off.
     */
    class AudioSystem {
    public:
        AudioSystem();
        ~AudioSystem();
        AudioSystem(const AudioSystem&) = delete;
        AudioSystem& operator=(const AudioSystem&) = delete;

        /// @brief Loads the manifest and opens the output. Logs one `Audio:` line (sounds, files, MB, output).
        /// false only when the manifest is missing or bad; the system then stays Off.
        bool Initialize(const AudioConfig& config);
        void Shutdown();
        /// @brief The output actually running (Device can fall back to Offline, a failure to Off).
        AudioOutput Output() const;
        /// @brief The playback device's name ("" unless Device).
        const std::string& DeviceName() const;

        const Manifest& Sounds() const;
        SoundId Find(std::string_view id) const;
        Voice Play(SoundId sound, const PlayParams& params);
        Voice Play(std::string_view id, const PlayParams& params);
        void Stop(Voice voice);
        void SetVoiceGain(Voice voice, float gain);
        bool IsPlaying(Voice voice) const;

        void SetListener(const glm::dvec3& position, const glm::vec3& forward, const glm::vec3& up);
        const glm::dvec3& ListenerPosition() const;
        void SetBusVolume(Bus bus, float volume);
        float BusVolume(Bus bus) const;
        /// @brief Starts decoding every variant of the one-shots whose ID starts with one of `prefixes` on miniaudio's
        /// job thread (no decode on the main thread at their first play). Device output only; Offline (headless runs:
        /// no extra work beside timed gates) and Off do nothing.
        void Preload(std::span<const std::string_view> prefixes);

        void Update(float dt);
        uint32_t ActiveVoices() const;
        AudioSummary Summary() const;
        /// @brief The `Audio:` exit line.
        std::string SummaryLine() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };

    /**
     * @brief The ambience beds: a set of target weights (0..1), faded in and out over `fadeSeconds`; a bed starts
     * when its weight leaves 0 and stops when it returns there (docs/design/audio.md "Ambience").
     */
    class Ambience {
    public:
        explicit Ambience(float fadeSeconds = 2.0f) : m_Fade(fadeSeconds) {}
        /// @brief The beds wanted now; every bed not named fades out.
        void SetTargets(std::span<const std::pair<SoundId, float>> targets);
        void Update(float dt, AudioSystem& audio);
        void StopAll(AudioSystem& audio);
        /// @brief The current weight of a bed (0 when it is not playing).
        float Weight(SoundId sound) const;

    private:
        struct Bed {
            SoundId sound = kNoSound;
            float weight = 0.0f, target = 0.0f;
            Voice voice;
            float retry = 0.0f;   // seconds until a failed start is tried again
        };
        float m_Fade;
        std::vector<Bed> m_Beds;
    };
}
