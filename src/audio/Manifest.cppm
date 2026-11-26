module;

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module vortex.audio:manifest;

export namespace vortex::audio {

    /// @brief The mixer's buses (docs/design/audio.md "Mixer"). Master scales the other four.
    enum class Bus : uint8_t { Master = 0, Sfx = 1, Ambience = 2, Ui = 3, Music = 4 };
    inline constexpr uint32_t kBusCount = 5;
    /// @brief "master", "sfx", "ambience", "ui", "music".
    const char* ToString(Bus bus);

    /// @brief How a sound fades with distance (sound-list.md section 18): Flat is not placed (2D).
    enum class Distance : uint8_t { Flat = 0, Small = 1, Medium = 2, Large = 3, Huge = 4 };

    /// @brief Full volume up to `inner` metres, then inverse distance to silence at `outer`.
    struct DistanceRange {
        float inner = 0.0f;
        float outer = 0.0f;
    };
    /// @brief S 2 / 25 m, M 3 / 60 m, L 5 / 150 m, XL 10 / 300 m; Flat 0 / 0 (no fade).
    DistanceRange RangeOf(Distance distance);
    /// @brief The gain (0..1) at `metres` from the source: 1 up to inner, (inner / d) x (outer - d) / (outer - inner)
    /// after it, 0 from outer on. Flat is always 1.
    float DistanceGain(Distance distance, float metres);

    /// @brief One sound ID of the manifest: what the mixer needs to play it.
    struct SoundDesc {
        std::string id;                ///< e.g. "sfx.foot.grass"
        Bus bus = Bus::Sfx;
        uint32_t variants = 1;         ///< files <kind>/<category>/<id>_v1.ogg .. _v<variants>.ogg
        bool loop = false;
        float gainDb = 0.0f;           ///< the sound's level in the mix
        float pitchRange = 0.0f;       ///< per-play pitch jitter, uniform in -range..+range semitones
        float volumeRange = 0.0f;      ///< per-play volume jitter, uniform in -range..+range dB
        Distance distance = Distance::Medium;
        bool far = false;              ///< past 80 m the far layer plays in its place
        uint8_t priority = 1;          ///< 0 (taken first) .. 3 (never taken by a lower one)
    };

    /// @brief An index into Manifest::Sounds(); kNoSound = not in the manifest.
    using SoundId = uint32_t;
    inline constexpr SoundId kNoSound = 0xffffffffu;

    /**
     * @brief The list of sounds the game ships (`assets/audio/manifest.txt`, written by tools/audio/build_sounds.py).
     * @details One line per sound: `<id> bus=sfx variants=5 loop=0 gain=-12 pitch=1 vol=2 dist=S far=0 priority=0`;
     * `#` starts a comment line. Every key is optional except bus and variants; unknown keys are refused (a typo
     * would silently change the mix).
     */
    class Manifest {
    public:
        /// @brief Parses `text`. On the first bad line returns false, keeps nothing and writes "line N: ..." to `error`.
        bool Parse(std::string_view text, std::string* error = nullptr);
        /// @brief Reads and parses `path`; false when it is missing or bad (`error` says why).
        bool Load(const std::filesystem::path& path, std::string* error = nullptr);

        const std::vector<SoundDesc>& Sounds() const { return m_Sounds; }
        size_t Size() const { return m_Sounds.size(); }
        /// @brief Total number of files (the sum of the variants).
        uint32_t FileCount() const;
        SoundId Find(std::string_view id) const;
        const SoundDesc& Get(SoundId sound) const { return m_Sounds[sound]; }

        /// @brief The file of variant `variant` (1-based), relative to the manifest's folder:
        /// "sfx/foot/sfx.foot.grass_v2.ogg".
        static std::filesystem::path FileOf(const SoundDesc& sound, uint32_t variant);

    private:
        std::vector<SoundDesc> m_Sounds;
        std::unordered_map<std::string, SoundId> m_Index;
    };
}
