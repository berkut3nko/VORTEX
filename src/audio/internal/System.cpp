module;

#include <miniaudio.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

module vortex.audio;

namespace vortex::audio {

    namespace {
        double MsSince(std::chrono::steady_clock::time_point start) {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }

        ma_result InitFromFile(ma_engine* engine, const std::filesystem::path& path, ma_uint32 flags, ma_sound_group* group, ma_sound* sound) {
#ifdef _WIN32
            return ma_sound_init_from_file_w(engine, path.c_str(), flags, group, nullptr, sound);
#else
            return ma_sound_init_from_file(engine, path.c_str(), flags, group, nullptr, sound);
#endif
        }

        /// The miniaudio side: one engine (with a device, or pumped by Update), a group per bus, a decoded prototype
        /// per played variant of a one-shot, a ma_sound per voice. Beds and other loops stream from their file.
        class MiniaudioBackend final : public Backend {
        public:
            MiniaudioBackend(std::filesystem::path root, uint32_t voices) : m_Root(std::move(root)), m_Voices(voices) {}
            ~MiniaudioBackend() override { Shutdown(); }

            /// device: open the default playback device; otherwise an engine without one (pumped by Mix).
            bool Initialize(bool device, std::string& deviceName) {
                ma_resource_manager_config rmc = ma_resource_manager_config_init();
                rmc.decodedFormat = ma_format_s16;   // half the memory of f32; the engine converts while mixing
                rmc.decodedSampleRate = 48000;
                rmc.jobThreadCount = 1;   // streams (beds) load their pages on it, offline too
                if (ma_resource_manager_init(&rmc, &m_ResourceManager) != MA_SUCCESS) return false;
                m_ResourceManagerReady = true;
                ma_engine_config ec = ma_engine_config_init();
                ec.pResourceManager = &m_ResourceManager;
                ec.channels = 2;
                ec.sampleRate = 48000;
                ec.listenerCount = 1;
                ec.noDevice = device ? MA_FALSE : MA_TRUE;
                if (ma_engine_init(&ec, &m_Engine) != MA_SUCCESS) return false;
                m_EngineReady = true;
                for (uint32_t b = 1; b < kBusCount; ++b) {
                    if (ma_sound_group_init(&m_Engine, 0, nullptr, &m_Groups[b]) != MA_SUCCESS) return false;
                    m_GroupReady[b] = true;
                }
                if (device) {
                    if (ma_device* d = ma_engine_get_device(&m_Engine)) {
                        char name[256] = {};
                        if (ma_device_get_name(d, ma_device_type_playback, name, sizeof(name), nullptr) == MA_SUCCESS) deviceName = name;
                    }
                }
                return true;
            }

            void Shutdown() {
                for (uint32_t i = 0; i < m_Voices.size(); ++i) Stop(i);
                for (auto& [key, proto] : m_Prototypes)
                    if (proto) ma_sound_uninit(proto.get());
                m_Prototypes.clear();
                for (uint32_t b = 1; b < kBusCount; ++b)
                    if (m_GroupReady[b]) ma_sound_group_uninit(&m_Groups[b]);
                m_GroupReady.fill(false);
                if (m_EngineReady) ma_engine_uninit(&m_Engine);
                m_EngineReady = false;
                if (m_ResourceManagerReady) ma_resource_manager_uninit(&m_ResourceManager);
                m_ResourceManagerReady = false;
            }

            /// The decoded one-shot of a variant, made on its first play (decoded now) or by Preload (`async`: decoded on
            /// the resource manager's job thread; a voice copied from it meanwhile starts once the data is there).
            /// nullptr when the file cannot be read.
            ma_sound* Prototype(SoundId sound, const SoundDesc& desc, uint32_t variant, bool async = false) {
                const uint64_t key = (static_cast<uint64_t>(sound) << 8) | variant;
                auto it = m_Prototypes.find(key);
                if (it != m_Prototypes.end()) return it->second.get();
                auto proto = std::make_unique<ma_sound>();
                const std::filesystem::path path = m_Root / Manifest::FileOf(desc, variant);
                const ma_uint32 flags = MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION | (async ? MA_SOUND_FLAG_ASYNC : 0u);
                if (InitFromFile(&m_Engine, path, flags, nullptr, proto.get()) != MA_SUCCESS) {
                    if (m_Reported.size() < 16 && std::find(m_Reported.begin(), m_Reported.end(), key) == m_Reported.end()) {
                        m_Reported.push_back(key);
                        Log::Warn("Audio: cannot decode {}", path.string());
                    }
                    m_Prototypes.emplace(key, nullptr);
                    return nullptr;
                }
                ++m_DecodedFiles;
                ma_sound* raw = proto.get();
                m_Prototypes.emplace(key, std::move(proto));
                return raw;
            }

            bool Ready(SoundId sound, const SoundDesc& desc, uint32_t variant) override {
                if (desc.loop) {
                    // A loop streams from its file: there must be one (looked up once per variant).
                    const uint64_t key = (static_cast<uint64_t>(sound) << 8) | variant;
                    auto it = m_StreamFound.find(key);
                    if (it == m_StreamFound.end()) {
                        std::error_code ec;
                        it = m_StreamFound.emplace(key, std::filesystem::is_regular_file(m_Root / Manifest::FileOf(desc, variant), ec)).first;
                    }
                    return it->second;
                }
                ma_sound* proto = Prototype(sound, desc, variant);
                // A copy of a prototype still decoding on the job thread would, when stopped, wait for that thread.
                return proto != nullptr && ma_resource_manager_data_source_result(proto->pResourceManagerDataSource) == MA_SUCCESS;
            }

            bool Start(uint32_t slot, SoundId sound, const SoundDesc& desc, uint32_t variant, const VoiceParams& params) override {
                Stop(slot);
                ma_sound_group* group = &m_Groups[static_cast<size_t>(desc.bus)];
                auto voice = std::make_unique<ma_sound>();
                ma_result result = MA_ERROR;
                if (desc.loop) {
                    result = InitFromFile(&m_Engine, m_Root / Manifest::FileOf(desc, variant), MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION,
                                          group, voice.get());
                } else if (ma_sound* proto = Prototype(sound, desc, variant)) {
                    result = ma_sound_init_copy(&m_Engine, proto, MA_SOUND_FLAG_NO_SPATIALIZATION, group, voice.get());
                }
                if (result != MA_SUCCESS) return false;
                m_Voices[slot] = std::move(voice);
                Set(slot, params);
                ma_sound_set_looping(m_Voices[slot].get(), params.loop ? MA_TRUE : MA_FALSE);
                ma_sound_start(m_Voices[slot].get());
                return true;
            }

            void Set(uint32_t slot, const VoiceParams& params) override {
                ma_sound* v = m_Voices[slot].get();
                if (!v) return;
                ma_sound_set_volume(v, params.gain);
                ma_sound_set_pitch(v, params.pitch);
                ma_sound_set_pan(v, params.pan);
            }

            void Stop(uint32_t slot) override {
                if (slot >= m_Voices.size() || !m_Voices[slot]) return;
                ma_sound_uninit(m_Voices[slot].get());
                m_Voices[slot].reset();
            }

            bool Playing(uint32_t slot) const override {
                const ma_sound* v = m_Voices[slot].get();
                return v != nullptr && !ma_sound_at_end(v);
            }

            void SetBusGain(Bus bus, float gain) override {
                if (!m_EngineReady) return;
                if (bus == Bus::Master) ma_engine_set_volume(&m_Engine, gain);
                else ma_sound_group_set_volume(&m_Groups[static_cast<size_t>(bus)], gain);
            }

            /// Offline only: mixes `seconds` of sound into a scratch buffer (what a device would have pulled).
            void Mix(double seconds) {
                m_MixCarry += seconds * 48000.0;
                ma_uint64 frames = static_cast<ma_uint64>(m_MixCarry);
                m_MixCarry -= static_cast<double>(frames);
                float buffer[2 * 1024];
                while (frames > 0) {
                    const ma_uint64 n = std::min<ma_uint64>(frames, 1024);
                    ma_uint64 read = 0;
                    ma_engine_read_pcm_frames(&m_Engine, buffer, n, &read);
                    frames -= n;
                    m_MixedFrames += n;
                }
            }

            uint32_t DecodedFiles() const { return m_DecodedFiles; }
            /// Bytes of decoded (16-bit) sound held now; a variant still loading counts once it is there.
            double DecodedBytes() {
                double bytes = 0.0;
                for (auto& [key, proto] : m_Prototypes) {
                    if (!proto) continue;
                    ma_uint64 frames = 0;
                    ma_uint32 channels = 0;
                    if (ma_sound_get_length_in_pcm_frames(proto.get(), &frames) != MA_SUCCESS) continue;
                    if (ma_sound_get_data_format(proto.get(), nullptr, &channels, nullptr, nullptr, 0) != MA_SUCCESS) continue;
                    bytes += static_cast<double>(frames) * channels * 2.0;
                }
                return bytes;
            }
            uint64_t MixedFrames() const { return m_MixedFrames; }

        private:
            std::filesystem::path m_Root;
            ma_resource_manager m_ResourceManager{};
            ma_engine m_Engine{};
            bool m_ResourceManagerReady = false, m_EngineReady = false;
            std::array<ma_sound_group, kBusCount> m_Groups{};
            std::array<bool, kBusCount> m_GroupReady{};
            std::vector<std::unique_ptr<ma_sound>> m_Voices;
            std::unordered_map<uint64_t, std::unique_ptr<ma_sound>> m_Prototypes;
            std::vector<uint64_t> m_Reported;
            std::unordered_map<uint64_t, bool> m_StreamFound;
            uint32_t m_DecodedFiles = 0;
            double m_MixCarry = 0.0;
            uint64_t m_MixedFrames = 0;
        };
    }

    struct AudioSystem::Impl {
        AudioConfig config;
        AudioOutput output = AudioOutput::Off;
        std::string deviceName;
        Manifest manifest;
        std::unique_ptr<MiniaudioBackend> backend;
        std::unique_ptr<Mixer> mixer;
        uint32_t missingFiles = 0;
        double assetBytes = 0.0;
        double mixMs = 0.0;
        double updateMsMax = 0.0;
        double playUsMax = 0.0;
        std::array<float, kBusCount> volumes{1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        glm::dvec3 listener{0.0};
    };

    AudioSystem::AudioSystem() : m_Impl(std::make_unique<Impl>()) {}
    AudioSystem::~AudioSystem() { Shutdown(); }

    bool AudioSystem::Initialize(const AudioConfig& config) {
        Shutdown();
        Impl& s = *m_Impl;
        s.config = config;
        s.volumes = config.volumes;
        s.missingFiles = 0;
        s.assetBytes = 0.0;
        if (config.output == AudioOutput::Off) {
            Log::Info("Audio: off (--no-audio)");
            return true;
        }
        const std::filesystem::path manifestPath = config.root / "manifest.txt";
        std::string error;
        if (!s.manifest.Load(manifestPath, &error)) {
            Log::Warn("Audio: off: {}", error);
            return false;
        }
        // Every listed file must be there (sound-list.md section 15: variants 1..n, no gaps); their size is the
        // shipped set's.
        std::error_code ec;
        for (const SoundDesc& sound : s.manifest.Sounds()) {
            for (uint32_t v = 1; v <= sound.variants; ++v) {
                const auto size = std::filesystem::file_size(config.root / Manifest::FileOf(sound, v), ec);
                if (ec) {
                    if (s.missingFiles++ < 4) Log::Warn("Audio: missing {}", (config.root / Manifest::FileOf(sound, v)).string());
                } else {
                    s.assetBytes += static_cast<double>(size);
                }
            }
        }
        bool device = config.output == AudioOutput::Device;
        s.backend = std::make_unique<MiniaudioBackend>(config.root, config.mixer.maxVoices);
        if (!s.backend->Initialize(device, s.deviceName)) {
            s.backend.reset();
            if (device) {
                // No playback device (a server box, a CI machine, a broken driver): mix offline so the game runs the same.
                device = false;
                s.backend = std::make_unique<MiniaudioBackend>(config.root, config.mixer.maxVoices);
                std::string unused;
                if (!s.backend->Initialize(false, unused)) s.backend.reset();
            }
        }
        if (!s.backend) {
            Log::Warn("Audio: off: miniaudio could not start");
            return true;
        }
        s.output = device ? AudioOutput::Device : AudioOutput::Offline;
        s.mixer = std::make_unique<Mixer>(s.manifest, *s.backend, config.mixer);
        for (uint32_t b = 0; b < kBusCount; ++b) s.mixer->SetBusVolume(static_cast<Bus>(b), s.volumes[b]);
        Log::Info("Audio: {} sounds, {} files ({} missing), {:.2f} MB of assets from {}; output {}{}", s.manifest.Size(), s.manifest.FileCount(),
                  s.missingFiles, s.assetBytes / 1.0e6, config.root.string(), device ? "device" : "offline (no device: mixed, not heard)",
                  device && !s.deviceName.empty() ? std::format(" '{}'", s.deviceName) : std::string());
        return true;
    }

    void AudioSystem::Shutdown() {
        Impl& s = *m_Impl;
        s.mixer.reset();
        s.backend.reset();
        s.output = AudioOutput::Off;
    }

    AudioOutput AudioSystem::Output() const { return m_Impl->output; }
    const std::string& AudioSystem::DeviceName() const { return m_Impl->deviceName; }
    const Manifest& AudioSystem::Sounds() const { return m_Impl->manifest; }
    SoundId AudioSystem::Find(std::string_view id) const { return m_Impl->manifest.Find(id); }

    Voice AudioSystem::Play(SoundId sound, const PlayParams& params) {
        Impl& s = *m_Impl;
        if (!s.mixer) return {};
        const auto start = std::chrono::steady_clock::now();
        const Voice voice = s.mixer->Play(sound, params);
        s.playUsMax = std::max(s.playUsMax, MsSince(start) * 1000.0);
        return voice;
    }

    Voice AudioSystem::Play(std::string_view id, const PlayParams& params) {
        if (!m_Impl->mixer) return {};
        return Play(m_Impl->manifest.Find(id), params);
    }

    void AudioSystem::Stop(Voice voice) {
        if (m_Impl->mixer) m_Impl->mixer->Stop(voice);
    }
    void AudioSystem::SetVoiceGain(Voice voice, float gain) {
        if (m_Impl->mixer) m_Impl->mixer->SetVoiceGain(voice, gain);
    }
    bool AudioSystem::IsPlaying(Voice voice) const { return m_Impl->mixer && m_Impl->mixer->IsPlaying(voice); }

    void AudioSystem::SetListener(const glm::dvec3& position, const glm::vec3& forward, const glm::vec3& up) {
        m_Impl->listener = position;
        if (m_Impl->mixer) m_Impl->mixer->SetListener(position, forward, up);
    }
    const glm::dvec3& AudioSystem::ListenerPosition() const { return m_Impl->listener; }

    void AudioSystem::SetBusVolume(Bus bus, float volume) {
        m_Impl->volumes[static_cast<size_t>(bus)] = std::clamp(volume, 0.0f, 1.0f);
        if (m_Impl->mixer) m_Impl->mixer->SetBusVolume(bus, volume);
    }
    float AudioSystem::BusVolume(Bus bus) const { return m_Impl->volumes[static_cast<size_t>(bus)]; }

    void AudioSystem::Preload(std::span<const std::string_view> prefixes) {
        Impl& s = *m_Impl;
        if (!s.backend || s.output != AudioOutput::Device) return;
        for (SoundId id = 0; id < s.manifest.Size(); ++id) {
            const SoundDesc& desc = s.manifest.Get(id);
            if (desc.loop) continue;
            if (std::none_of(prefixes.begin(), prefixes.end(), [&](std::string_view p) { return desc.id.starts_with(p); })) continue;
            for (uint32_t v = 1; v <= desc.variants; ++v) s.backend->Prototype(id, desc, v, true);
        }
    }

    void AudioSystem::Update(float dt) {
        Impl& s = *m_Impl;
        if (!s.mixer) return;
        const auto start = std::chrono::steady_clock::now();
        s.mixer->Update(dt);
        s.updateMsMax = std::max(s.updateMsMax, MsSince(start));
        if (s.output == AudioOutput::Offline) {
            const auto mixStart = std::chrono::steady_clock::now();
            s.backend->Mix(static_cast<double>(dt));
            s.mixMs += MsSince(mixStart);
        }
    }

    uint32_t AudioSystem::ActiveVoices() const { return m_Impl->mixer ? m_Impl->mixer->ActiveVoices() : 0u; }

    AudioSummary AudioSystem::Summary() const {
        const Impl& s = *m_Impl;
        AudioSummary sum;
        if (s.mixer) sum.mixer = s.mixer->Stats();
        sum.sounds = static_cast<uint32_t>(s.manifest.Size());
        sum.files = s.manifest.FileCount();
        sum.missingFiles = s.missingFiles;
        sum.assetMegabytes = s.assetBytes / 1.0e6;
        if (s.backend) {
            sum.decodedFiles = s.backend->DecodedFiles();
            sum.decodedMegabytes = s.backend->DecodedBytes() / 1.0e6;
            sum.mixedSeconds = static_cast<double>(s.backend->MixedFrames()) / 48000.0;
        }
        sum.mixMs = s.mixMs;
        sum.updateMsMax = s.updateMsMax;
        sum.playUsMax = s.playUsMax;
        return sum;
    }

    std::string AudioSystem::SummaryLine() const {
        const AudioSummary a = Summary();
        const MixerStats& m = a.mixer;
        std::string mix;
        if (a.mixedSeconds > 0.0)
            mix = std::format("; mixed {:.1f} s offline in {:.1f} ms ({:.2f} % of one core)", a.mixedSeconds, a.mixMs,
                              a.mixMs / (a.mixedSeconds * 10.0));
        return std::format("Audio: {} plays, {} started, {} merged, {} stolen, {} refused full, {} over budget, {} inaudible, {} unknown, "
                           "{} failed, {} far; max {} voices; {} variants decoded ({:.1f} MB); update max {:.3f} ms, play max {:.0f} us{}",
                           m.requests, m.started, m.merged, m.stolen, m.refusedFull, m.refusedBudget, m.inaudible, m.unknown, m.failed,
                           m.farLayer, m.maxActive, a.decodedFiles, a.decodedMegabytes, a.updateMsMax, a.playUsMax, mix);
    }

    // --- Ambience ----------------------------------------------------------------------------------------------

    void Ambience::SetTargets(std::span<const std::pair<SoundId, float>> targets) {
        for (Bed& bed : m_Beds) bed.target = 0.0f;
        for (const auto& [sound, weight] : targets) {
            if (sound == kNoSound) continue;
            auto it = std::find_if(m_Beds.begin(), m_Beds.end(), [&](const Bed& b) { return b.sound == sound; });
            if (it == m_Beds.end()) {
                m_Beds.push_back(Bed{sound, 0.0f, 0.0f, {}, 0.0f});
                it = m_Beds.end() - 1;
            }
            it->target = std::clamp(weight, 0.0f, 1.0f);
        }
    }

    void Ambience::Update(float dt, AudioSystem& audio) {
        const float step = m_Fade > 0.0f ? dt / m_Fade : 1.0f;
        for (Bed& bed : m_Beds) {
            if (bed.weight < bed.target) bed.weight = std::min(bed.target, bed.weight + step);
            else bed.weight = std::max(bed.target, bed.weight - step);
            bed.retry = std::max(0.0f, bed.retry - dt);
            if (bed.weight > 0.0f && !audio.IsPlaying(bed.voice) && bed.retry <= 0.0f) {
                PlayParams p;
                p.flat = true;
                p.gain = std::sin(bed.weight * 1.5707964f);
                bed.voice = audio.Play(bed.sound, p);
                if (!bed.voice.Valid()) bed.retry = 2.0f;   // a missing file, or no voice free: try again later
            }
            if (bed.weight <= 0.0f && audio.IsPlaying(bed.voice)) {
                audio.Stop(bed.voice);
                bed.voice = {};
            }
            // Equal power: two beds crossfading keep the same loudness.
            audio.SetVoiceGain(bed.voice, std::sin(bed.weight * 1.5707964f));
        }
        std::erase_if(m_Beds, [](const Bed& b) { return b.weight <= 0.0f && b.target <= 0.0f; });
    }

    void Ambience::StopAll(AudioSystem& audio) {
        for (Bed& bed : m_Beds) audio.Stop(bed.voice);
        m_Beds.clear();
    }

    float Ambience::Weight(SoundId sound) const {
        for (const Bed& bed : m_Beds)
            if (bed.sound == sound) return bed.weight;
        return 0.0f;
    }
}
