// The audio module's policy (docs/design/audio.md): the manifest, distance, variation, merging, budgets, the voice
// cap and stealing. No audio device: a recording backend stands in for miniaudio.
#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

import vortex.audio;

using namespace vortex;
using namespace vortex::audio;

namespace {

    /// Records what the mixer asks for; a voice plays until the test ends it.
    class RecordingBackend final : public Backend {
    public:
        struct Started {
            uint32_t slot;
            SoundId sound;
            uint32_t variant;
            VoiceParams params;
        };
        bool Ready(SoundId, const SoundDesc&, uint32_t) override { return !notReady; }
        bool Start(uint32_t slot, SoundId sound, const SoundDesc&, uint32_t variant, const VoiceParams& params) override {
            if (failNext) {
                failNext = false;
                return false;
            }
            starts.push_back({slot, sound, variant, params});
            playing[slot] = true;
            last[slot] = params;
            return true;
        }
        void Set(uint32_t slot, const VoiceParams& params) override { last[slot] = params; }
        void Stop(uint32_t slot) override {
            if (playing[slot]) ++stops;
            playing[slot] = false;
        }
        bool Playing(uint32_t slot) const override {
            const auto it = playing.find(slot);
            return it != playing.end() && it->second;
        }
        void SetBusGain(Bus bus, float gain) override { busGain[static_cast<size_t>(bus)] = gain; }

        std::vector<Started> starts;
        std::map<uint32_t, bool> playing;
        std::map<uint32_t, VoiceParams> last;
        std::array<float, kBusCount> busGain{1, 1, 1, 1, 1};
        uint32_t stops = 0;
        bool failNext = false;
        bool notReady = false;
    };

    constexpr const char* kManifest =
        "# test manifest\n"
        "sfx.foot.grass bus=sfx variants=5 loop=0 gain=-12 pitch=1 vol=2 dist=S far=0 priority=0\n"
        "sfx.break.stone.small bus=sfx variants=4 gain=-5 pitch=1.5 vol=2 dist=L priority=1\n"
        "sfx.blast.core.medium bus=sfx variants=2 gain=0 pitch=1 vol=2 dist=L far=1 priority=2\n"
        "sfx.blast.distant bus=sfx variants=3 gain=0 pitch=1 vol=2 dist=XL priority=2\n"
        "ui.menu.move bus=ui variants=2 gain=-8 pitch=0.5 vol=0 dist=2D priority=3\n"
        "amb.forest.bed bus=ambience variants=1 loop=1 gain=-8 pitch=0 vol=1 dist=2D priority=2\n"
        "sfx.obj.pickup bus=sfx variants=1 gain=-7 dist=M priority=1\n";

    Manifest TestManifest() {
        Manifest m;
        std::string error;
        EXPECT_TRUE(m.Parse(kManifest, &error)) << error;
        return m;
    }

    PlayParams At(double x, double y = 0.0, double z = 0.0) {
        PlayParams p;
        p.position = glm::dvec3(x, y, z);
        return p;
    }
}

TEST(AudioManifest, ParsesEveryField) {
    const Manifest m = TestManifest();
    ASSERT_EQ(m.Size(), 7u);
    EXPECT_EQ(m.FileCount(), 5u + 4u + 2u + 3u + 2u + 1u + 1u);
    const SoundDesc& foot = m.Get(m.Find("sfx.foot.grass"));
    EXPECT_EQ(foot.bus, Bus::Sfx);
    EXPECT_EQ(foot.variants, 5u);
    EXPECT_FALSE(foot.loop);
    EXPECT_FLOAT_EQ(foot.gainDb, -12.0f);
    EXPECT_FLOAT_EQ(foot.pitchRange, 1.0f);
    EXPECT_FLOAT_EQ(foot.volumeRange, 2.0f);
    EXPECT_EQ(foot.distance, Distance::Small);
    EXPECT_EQ(foot.priority, 0);
    const SoundDesc& bed = m.Get(m.Find("amb.forest.bed"));
    EXPECT_TRUE(bed.loop);
    EXPECT_EQ(bed.bus, Bus::Ambience);
    EXPECT_EQ(bed.distance, Distance::Flat);
    EXPECT_TRUE(m.Get(m.Find("sfx.blast.core.medium")).far);
    // Keys left out keep their defaults.
    const SoundDesc& pickup = m.Get(m.Find("sfx.obj.pickup"));
    EXPECT_FLOAT_EQ(pickup.pitchRange, 0.0f);
    EXPECT_EQ(m.Find("sfx.nothing.here"), kNoSound);
    // sound-list.md section 15: <kind>/<category>/<id>_v<N>.ogg.
    EXPECT_EQ(Manifest::FileOf(foot, 2).generic_string(), "sfx/foot/sfx.foot.grass_v2.ogg");
    EXPECT_EQ(Manifest::FileOf(m.Get(m.Find("ui.menu.move")), 1).generic_string(), "ui/menu/ui.menu.move_v1.ogg");
}

TEST(AudioManifest, RefusesBadLinesAndKeepsNothing) {
    Manifest m = TestManifest();
    std::string error;
    EXPECT_FALSE(m.Parse("sfx.a.b bus=sfx variants=2 gian=3\n", &error));
    EXPECT_NE(error.find("line 1"), std::string::npos);
    EXPECT_NE(error.find("gian"), std::string::npos);
    EXPECT_EQ(m.Size(), 7u) << "a failed parse keeps the previous manifest";
    EXPECT_FALSE(m.Parse("sfx.a.b bus=loud variants=2\n", &error));
    EXPECT_FALSE(m.Parse("sfx.a.b bus=sfx\n", &error)) << "variants is required";
    EXPECT_FALSE(m.Parse("sfx.a.b bus=sfx variants=0\n", &error));
    EXPECT_FALSE(m.Parse("sfx.a.b bus=sfx variants=1 dist=far\n", &error));
    EXPECT_FALSE(m.Parse("Sfx.A bus=sfx variants=1\n", &error));
    EXPECT_FALSE(m.Parse("# ok\nsfx.a.b bus=sfx variants=1\nsfx.a.b bus=sfx variants=1\n", &error));
    EXPECT_NE(error.find("line 3"), std::string::npos);
    EXPECT_TRUE(m.Parse("\r\n# comment\r\nsfx.a.b bus=sfx variants=1\r\n", &error)) << error;
    EXPECT_EQ(m.Size(), 1u);
}

TEST(AudioManifest, LoadsAFileAndNamesAMissingOne) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vortex-audio-manifest-test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "manifest.txt";
    {
        std::ofstream out(file, std::ios::binary);
        out << kManifest;
    }
    Manifest m;
    std::string error;
    EXPECT_TRUE(m.Load(file, &error)) << error;
    EXPECT_EQ(m.Size(), 7u);
    EXPECT_FALSE(m.Load(dir / "missing.txt", &error));
    EXPECT_NE(error.find("missing.txt"), std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST(AudioDistance, FullInsideThenInverseToSilenceAtTheOuterDistance) {
    EXPECT_FLOAT_EQ(DistanceGain(Distance::Small, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(DistanceGain(Distance::Small, 2.0f), 1.0f);
    EXPECT_FLOAT_EQ(DistanceGain(Distance::Small, 25.0f), 0.0f);
    EXPECT_FLOAT_EQ(DistanceGain(Distance::Huge, 400.0f), 0.0f);
    EXPECT_FLOAT_EQ(DistanceGain(Distance::Flat, 1000.0f), 1.0f);
    // Strictly falling between inner and outer.
    for (const Distance d : {Distance::Small, Distance::Medium, Distance::Large, Distance::Huge}) {
        const DistanceRange r = RangeOf(d);
        float previous = 1.0f;
        for (float m = r.inner + 0.5f; m < r.outer; m += 0.5f) {
            const float g = DistanceGain(d, m);
            EXPECT_LT(g, previous) << static_cast<int>(d) << " at " << m;
            EXPECT_GT(g, 0.0f);
            previous = g;
        }
    }
    // Inverse distance near the source: twice the inner distance is about half (a little less: the fade to silence).
    EXPECT_NEAR(DistanceGain(Distance::Large, 10.0f), 0.5f * (140.0f / 145.0f), 1.0e-5f);
}

TEST(AudioMixer, AVariantNeverRepeatsTheLastAndWithFourNotTheOneBefore) {
    const Manifest m = TestManifest();
    RecordingBackend backend;
    Mixer mixer(m, backend);
    for (const char* id : {"sfx.foot.grass", "sfx.break.stone.small", "sfx.blast.core.medium"}) {
        const SoundId s = m.Find(id);
        const uint32_t n = m.Get(s).variants;
        std::vector<uint32_t> seen(n + 1, 0);
        uint32_t last = 0, beforeLast = 0;
        for (int i = 0; i < 2000; ++i) {
            const uint32_t v = mixer.PickVariant(s);
            ASSERT_GE(v, 1u);
            ASSERT_LE(v, n);
            EXPECT_NE(v, last) << id;
            if (n >= 4) EXPECT_NE(v, beforeLast) << id;
            ++seen[v];
            beforeLast = last;
            last = v;
        }
        for (uint32_t v = 1; v <= n; ++v) EXPECT_GT(seen[v], 2000u / n / 2) << id << " variant " << v << " is hardly ever picked";
    }
    // One variant: always 1.
    const SoundId one = m.Find("sfx.obj.pickup");
    for (int i = 0; i < 5; ++i) EXPECT_EQ(mixer.PickVariant(one), 1u);
}

TEST(AudioMixer, PitchAndVolumeJitterStayInTheSoundsRange) {
    const Manifest m = TestManifest();
    RecordingBackend backend;
    Mixer mixer(m, backend);
    const SoundId breakStone = m.Find("sfx.break.stone.small");   // P±1.5 V±2
    glm::vec2 lo(100.0f), hi(-100.0f);
    for (int i = 0; i < 5000; ++i) {
        const glm::vec2 j = mixer.DrawJitter(breakStone);
        lo = glm::min(lo, j);
        hi = glm::max(hi, j);
    }
    EXPECT_GE(lo.x, -1.5f);
    EXPECT_LE(hi.x, 1.5f);
    EXPECT_GE(lo.y, -2.0f);
    EXPECT_LE(hi.y, 2.0f);
    EXPECT_LT(lo.x, -1.4f) << "the range is used";
    EXPECT_GT(hi.x, 1.4f);
    // A sound without jitter has none (UI volume, a bed's pitch).
    const glm::vec2 ui = mixer.DrawJitter(m.Find("ui.menu.move"));
    EXPECT_FLOAT_EQ(ui.y, 0.0f);
    EXPECT_LE(std::abs(ui.x), 0.5f);
    // What a backend gets: pitch ratio 2^(semitones / 12) inside the range, level inside gain +- vol.
    for (int i = 0; i < 200; ++i) {
        mixer.Update(1.0f);   // past the burst time and refills the budget
        ASSERT_TRUE(mixer.Play(breakStone, At(1.0)).Valid());
        const VoiceParams& p = backend.starts.back().params;
        EXPECT_GE(p.pitch, std::pow(2.0f, -1.5f / 12.0f) - 1.0e-5f);
        EXPECT_LE(p.pitch, std::pow(2.0f, 1.5f / 12.0f) + 1.0e-5f);
        EXPECT_GE(GainToDb(p.gain), -5.0f - 2.0f - 1.0e-3f);
        EXPECT_LE(GainToDb(p.gain), -5.0f + 2.0f + 1.0e-3f);
        backend.playing[backend.starts.back().slot] = false;
    }
}

TEST(AudioMixer, WhenFullTheQuietestOfALowerOrEqualPriorityGivesWay) {
    const Manifest m = TestManifest();
    RecordingBackend backend;
    MixerConfig config;
    config.maxVoices = 4;
    config.maxPerSound = 4;
    config.quietestDb = -70.0f;   // this test is about the cap: far breaks stay audible
    Mixer mixer(m, backend, config);
    // Four breaks at 10, 25, 50, 100 m (priority 1): the voices are full. The distances are far enough apart that
    // the +-2 dB jitter cannot change which is quietest.
    const SoundId breakStone = m.Find("sfx.break.stone.small");
    std::vector<Voice> voices;
    for (const double x : {10.0, 25.0, 50.0, 100.0}) voices.push_back(mixer.Play(breakStone, At(x)));
    for (const Voice& v : voices) ASSERT_TRUE(v.Valid());
    EXPECT_EQ(mixer.ActiveVoices(), 4u);

    // A blast (priority 2) takes the quietest: the break 100 m away.
    const Voice blast = mixer.Play(m.Find("sfx.blast.core.medium"), At(5.0));
    ASSERT_TRUE(blast.Valid());
    EXPECT_FALSE(mixer.IsPlaying(voices[3]));
    EXPECT_TRUE(mixer.IsPlaying(voices[0]));
    EXPECT_EQ(mixer.Stats().stolen, 1u);

    // A footstep (priority 0) takes nothing: everything playing outranks it.
    EXPECT_FALSE(mixer.Play(m.Find("sfx.foot.grass"), At(1.0)).Valid());
    EXPECT_EQ(mixer.Stats().refusedFull, 1u);

    // A break of the same priority takes a quieter break, but not when it is quieter itself.
    mixer.Update(1.0f);
    EXPECT_FALSE(mixer.Play(breakStone, At(140.0)).Valid()) << "quieter than every break playing";
    EXPECT_EQ(mixer.Stats().refusedFull, 2u);
    const Voice near = mixer.Play(breakStone, At(2.0));
    ASSERT_TRUE(near.Valid());
    EXPECT_FALSE(mixer.IsPlaying(voices[2])) << "the break at 50 m was the quietest";
    EXPECT_TRUE(mixer.IsPlaying(blast));
}

TEST(AudioMixer, AVariantThatIsNotReadyStealsNothingAndKeepsTheVariantOrder) {
    const Manifest m = TestManifest();
    RecordingBackend backend;
    MixerConfig config;
    config.maxVoices = 1;
    Mixer mixer(m, backend, config);
    const SoundId breakStone = m.Find("sfx.break.stone.small");
    const Voice first = mixer.Play(breakStone, At(20.0));
    ASSERT_TRUE(first.Valid());
    const uint32_t firstVariant = backend.starts.back().variant;
    // A blast would take the only voice, but its file is still decoding: the break keeps playing.
    backend.notReady = true;
    EXPECT_FALSE(mixer.Play(m.Find("sfx.blast.core.medium"), At(2.0)).Valid());
    EXPECT_TRUE(mixer.IsPlaying(first));
    EXPECT_EQ(mixer.Stats().stolen, 0u);
    EXPECT_EQ(mixer.Stats().failed, 1u);
    // A refused play does not count as the last variant played.
    backend.notReady = false;
    backend.playing[first.slot] = false;
    mixer.Update(1.0f);
    for (int i = 0; i < 20; ++i) {
        mixer.Update(1.0f);
        ASSERT_TRUE(mixer.Play(breakStone, At(20.0)).Valid());
        const uint32_t v = backend.starts.back().variant;
        backend.playing[backend.starts.back().slot] = false;
        if (i == 0) EXPECT_NE(v, firstVariant);
    }
}

TEST(AudioMixer, ALoopStartsHoweverQuietItBegins) {
    const Manifest m = TestManifest();
    RecordingBackend backend;
    Mixer mixer(m, backend);
    PlayParams bed;
    bed.flat = true;
    bed.gain = 0.001f;   // a bed fading in from nothing
    EXPECT_TRUE(mixer.Play(m.Find("amb.forest.bed"), bed).Valid());
    EXPECT_EQ(mixer.Stats().inaudible, 0u);
}

TEST(AudioMixer, AnIdHasAtMostFourVoicesAndTheLoudestWin) {
    const Manifest m = TestManifest();
    RecordingBackend backend;
    MixerConfig config;
    config.burstStarts = 100.0f;   // this test is about the cap, not the budget
    config.quietestDb = -70.0f;
    Mixer mixer(m, backend, config);
    const SoundId breakStone = m.Find("sfx.break.stone.small");
    std::vector<Voice> voices;
    for (const double x : {20.0, 40.0, 70.0, 120.0}) voices.push_back(mixer.Play(breakStone, At(x)));
    EXPECT_EQ(mixer.ActiveVoicesOf(breakStone), 4u);
    // A fifth, nearer: the farthest stops. A sixth, farther than all: refused.
    ASSERT_TRUE(mixer.Play(breakStone, At(3.0)).Valid());
    EXPECT_FALSE(mixer.IsPlaying(voices[3]));
    EXPECT_FALSE(mixer.Play(breakStone, At(140.0)).Valid());
    EXPECT_EQ(mixer.ActiveVoicesOf(breakStone), 4u);
    // Other IDs are not limited by it.
    EXPECT_TRUE(mixer.Play(m.Find("sfx.obj.pickup"), At(1.0)).Valid());
}

TEST(AudioMixer, OneIdAtOnePlaceWithin80MsPlaysOnceAndWithin50MsLouder) {
    const Manifest m = TestManifest();
    RecordingBackend backend;
    Mixer mixer(m, backend);
    const SoundId breakStone = m.Find("sfx.break.stone.small");
    const Voice first = mixer.Play(breakStone, At(10.0));
    ASSERT_TRUE(first.Valid());
    const float before = backend.last[first.slot].gain;
    mixer.Update(0.02f);
    const Voice merged = mixer.Play(breakStone, At(10.5));   // 0.5 m and 20 ms away: the same play, 2 dB louder
    EXPECT_EQ(merged, first);
    EXPECT_NEAR(GainToDb(backend.last[first.slot].gain) - GainToDb(before), 2.0f, 0.05f);
    mixer.Play(breakStone, At(10.2));                        // once only
    EXPECT_NEAR(GainToDb(backend.last[first.slot].gain) - GainToDb(before), 2.0f, 0.05f);
    mixer.Update(0.04f);                                     // 60 ms: no new voice, no boost
    EXPECT_EQ(mixer.Play(breakStone, At(10.0)), first);
    EXPECT_EQ(backend.starts.size(), 1u);
    EXPECT_EQ(mixer.Stats().merged, 3u);
    // Elsewhere, or later, it plays again.
    EXPECT_NE(mixer.Play(breakStone, At(20.0)), first);
    mixer.Update(0.1f);
    EXPECT_NE(mixer.Play(breakStone, At(10.0)), first);
    EXPECT_EQ(backend.starts.size(), 3u);
}

TEST(AudioMixer, TheStartBudgetHoldsBackACollapse) {
    const Manifest m = TestManifest();
    RecordingBackend backend;
    MixerConfig config;
    config.maxPerSound = 64;
    config.maxVoices = 64;
    Mixer mixer(m, backend, config);
    const SoundId breakStone = m.Find("sfx.break.stone.small");
    // A collapse: 40 breaks of one ID, 2 m apart, in one frame. The bucket holds 6.
    uint32_t started = 0;
    for (int i = 0; i < 40; ++i) started += mixer.Play(breakStone, At(2.0 * i)).Valid() ? 1u : 0u;
    EXPECT_EQ(started, static_cast<uint32_t>(config.burstStarts));
    EXPECT_EQ(mixer.Stats().refusedBudget, 40u - started);
    // It refills at 12 per second: half a second later 6 more.
    mixer.Update(0.5f);
    uint32_t later = 0;
    for (int i = 0; i < 40; ++i) later += mixer.Play(breakStone, At(2.0 * i, 0.0, 3.0)).Valid() ? 1u : 0u;
    EXPECT_EQ(later, 6u);
    // Other IDs keep their own budget.
    EXPECT_TRUE(mixer.Play(m.Find("sfx.obj.pickup"), At(1.0)).Valid());
}

TEST(AudioMixer, AQuietOrFarSoundDoesNotStartAndAFarBlastPlaysTheFarLayer) {
    const Manifest m = TestManifest();
    RecordingBackend backend;
    Mixer mixer(m, backend);
    EXPECT_FALSE(mixer.Play(m.Find("sfx.foot.grass"), At(30.0)).Valid()) << "past 25 m: silent";
    EXPECT_EQ(mixer.Stats().inaudible, 1u);
    // 100 m: the blast core is a `far` sound; the far layer (XL) plays in its place.
    ASSERT_TRUE(mixer.Play(m.Find("sfx.blast.core.medium"), At(100.0)).Valid());
    EXPECT_EQ(backend.starts.back().sound, m.Find("sfx.blast.distant"));
    EXPECT_EQ(mixer.Stats().farLayer, 1u);
    ASSERT_TRUE(mixer.Play(m.Find("sfx.blast.core.medium"), At(30.0)).Valid());
    EXPECT_EQ(backend.starts.back().sound, m.Find("sfx.blast.core.medium"));
    // A muted bus: everything on it is inaudible.
    mixer.SetBusVolume(Bus::Ui, 0.0f);
    EXPECT_EQ(backend.busGain[static_cast<size_t>(Bus::Ui)], 0.0f);
    PlayParams flat;
    flat.flat = true;
    EXPECT_FALSE(mixer.Play(m.Find("ui.menu.move"), flat).Valid());
    mixer.SetBusVolume(Bus::Ui, 1.0f);
    EXPECT_TRUE(mixer.Play(m.Find("ui.menu.move"), flat).Valid());
    // Unknown IDs are counted, not played.
    EXPECT_FALSE(mixer.Play(kNoSound, flat).Valid());
    EXPECT_EQ(mixer.Stats().unknown, 1u);
}

TEST(AudioMixer, FinishedVoicesFreeTheirSlotAndStaleHandlesAreIgnored) {
    const Manifest m = TestManifest();
    RecordingBackend backend;
    MixerConfig config;
    config.maxVoices = 2;
    Mixer mixer(m, backend, config);
    const Voice a = mixer.Play(m.Find("sfx.obj.pickup"), At(1.0));
    ASSERT_TRUE(a.Valid());
    backend.playing[a.slot] = false;   // reached its end
    mixer.Update(0.016f);
    EXPECT_EQ(mixer.ActiveVoices(), 0u);
    EXPECT_FALSE(mixer.IsPlaying(a));
    mixer.Update(1.0f);
    const Voice b = mixer.Play(m.Find("sfx.obj.pickup"), At(1.0));
    ASSERT_TRUE(b.Valid());
    EXPECT_EQ(b.slot, a.slot) << "the slot is used again";
    mixer.Stop(a);                     // stale: does not stop b
    EXPECT_TRUE(mixer.IsPlaying(b));
    mixer.Stop(b);
    EXPECT_FALSE(mixer.IsPlaying(b));
    // A backend that cannot start a file: counted, nothing kept.
    backend.failNext = true;
    mixer.Update(1.0f);
    EXPECT_FALSE(mixer.Play(m.Find("sfx.obj.pickup"), At(1.0)).Valid());
    EXPECT_EQ(mixer.Stats().failed, 1u);
    EXPECT_EQ(mixer.ActiveVoices(), 0u);
}

TEST(AudioMixer, PlacedVoicesPanToTheirSideAndFollowTheListener) {
    const Manifest m = TestManifest();
    RecordingBackend backend;
    Mixer mixer(m, backend);
    mixer.SetListener(glm::dvec3(0.0), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));   // right is +x
    const Voice v = mixer.Play(m.Find("sfx.obj.pickup"), At(10.0));
    ASSERT_TRUE(v.Valid());
    EXPECT_GT(backend.last[v.slot].pan, 0.8f);
    const float gainAt10 = backend.last[v.slot].gain;
    // The listener walks past it: the sound is now on the left and nearer.
    mixer.SetListener(glm::dvec3(14.0, 0.0, 0.0), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    mixer.Update(0.016f);
    EXPECT_LT(backend.last[v.slot].pan, -0.8f);
    EXPECT_GT(backend.last[v.slot].gain, gainAt10);
    // A flat play is centred whatever the position.
    PlayParams flat = At(-50.0);
    flat.flat = true;
    const Voice f = mixer.Play(m.Find("sfx.foot.grass"), flat);
    ASSERT_TRUE(f.Valid());
    EXPECT_FLOAT_EQ(backend.last[f.slot].pan, 0.0f);
}

TEST(AudioAmbience, BedsFadeInAndOutWithoutADevice) {
    // No Initialize: the system is Off, every Play returns no voice, and the weights still move (the fade is policy).
    AudioSystem audio;
    Ambience ambience(2.0f);
    const std::pair<SoundId, float> forest[] = {{3u, 1.0f}};
    ambience.SetTargets(forest);
    ambience.Update(1.0f, audio);
    EXPECT_NEAR(ambience.Weight(3u), 0.5f, 1.0e-5f);
    ambience.Update(1.5f, audio);
    EXPECT_FLOAT_EQ(ambience.Weight(3u), 1.0f);
    const std::pair<SoundId, float> cave[] = {{5u, 0.5f}};
    ambience.SetTargets(cave);
    ambience.Update(0.5f, audio);
    EXPECT_NEAR(ambience.Weight(3u), 0.75f, 1.0e-5f);
    EXPECT_NEAR(ambience.Weight(5u), 0.25f, 1.0e-5f);
    ambience.Update(5.0f, audio);
    EXPECT_FLOAT_EQ(ambience.Weight(3u), 0.0f);
    EXPECT_FLOAT_EQ(ambience.Weight(5u), 0.5f);
}
