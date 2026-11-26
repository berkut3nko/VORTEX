// The player's settings file and the HUD settings page (docs/design/game-ui.md "Settings").

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <system_error>

import vortex.core;

using namespace vortex;

namespace {
    constexpr glm::vec2 kScreen{1280.0f, 720.0f};
    constexpr int kScale = 2;

    SettingsPage::Result Press(SettingsPage& page, const MenuInput& input) { return page.HandleInput(input, kScreen, kScale); }
}

TEST(GameSettings, RoundTripsAndRefusesBadValues) {
    const GameSettings custom{0.75f, false, 1.5f, 90, false, 60, 40, 0, 100, 20};
    GameSettings parsed;
    std::string error;
    ASSERT_TRUE(ParseGameSettings(FormatGameSettings(custom), parsed, &error)) << error;
    EXPECT_EQ(parsed, custom);
    EXPECT_EQ(FormatGameSettings(GameSettings{}),
              "# Shredborne settings (key = value)\nrender_scale = 0.60\nvsync = on\nsensitivity = 1.00\nfov = 60\nplay_bounds = on\n"
              "master_volume = 100\nsfx_volume = 100\nambience_volume = 80\nui_volume = 80\nmusic_volume = 70\n");

    for (const char* bad : {"render_scale = 2", "render_scale = abc", "vsync = yes", "sensitivity = 0.1", "fov = 49", "fov = 70.5", "fov 70", "play_bounds = 1",
                            "master_volume = 101", "sfx_volume = -1", "music_volume = 50.5"}) {
        GameSettings s = custom;
        error.clear();
        EXPECT_FALSE(ParseGameSettings(bad, s, &error)) << bad;
        EXPECT_EQ(s, custom) << bad;
        EXPECT_EQ(error.rfind("line 1: ", 0), 0u) << bad << ": " << error;
    }
    GameSettings s;
    EXPECT_FALSE(ParseGameSettings("fov 70", s, &error));
    EXPECT_EQ(error, "line 1: expected key = value");
    EXPECT_FALSE(ParseGameSettings("fov = 70\nvsync = maybe\n", s, nullptr));   // no error string asked for
    EXPECT_EQ(s, GameSettings{});

    // The file: saved into a folder that does not exist yet, read back; a missing file is no settings.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / std::format("vortex-settings-test-{}", ::testing::UnitTest::GetInstance()->random_seed());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    const std::filesystem::path file = dir / "shredborne" / "settings.txt";
    EXPECT_FALSE(LoadGameSettings(file).has_value());
    ASSERT_TRUE(SaveGameSettings(file, custom));
    const std::optional<GameSettings> loaded = LoadGameSettings(file);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(*loaded, custom);
    std::filesystem::remove_all(dir, ec);
}

TEST(GameSettings, UnknownKeysAndCommentsAreSkipped) {
    GameSettings s;
    std::string error;
    ASSERT_TRUE(ParseGameSettings("# x\n\n  colour = red\nfov = 80\r\n", s, &error)) << error;
    EXPECT_EQ(s.fovDegrees, 80);
    GameSettings expected;
    expected.fovDegrees = 80;
    EXPECT_EQ(s, expected);
}

TEST(SettingsPage, ActivatingCyclesThePresets) {
    SettingsPage page;
    page.Open(GameSettings{});
    ASSERT_TRUE(page.IsOpen());
    for (float want : {0.75f, 0.5f, 0.6f}) {
        const SettingsPage::Result r = Press(page, MenuInput{.activate = true});
        EXPECT_TRUE(r.changed);
        EXPECT_FALSE(r.closed);
        EXPECT_FLOAT_EQ(page.Settings().renderScale, want);
    }
    EXPECT_FALSE(Press(page, MenuInput{.down = true}).changed);
    EXPECT_TRUE(Press(page, MenuInput{.activate = true}).changed);
    EXPECT_FALSE(page.Settings().vsync);
    EXPECT_TRUE(page.IsOpen());

    // Mouse speed and field of view walk their presets and wrap.
    Press(page, MenuInput{.down = true});
    for (float want : {1.25f, 1.5f, 2.0f, 0.5f}) {
        EXPECT_TRUE(Press(page, MenuInput{.activate = true}).changed);
        EXPECT_FLOAT_EQ(page.Settings().sensitivity, want);
    }
    Press(page, MenuInput{.down = true});
    for (int want : {70, 80, 90, 100, 60}) {
        EXPECT_TRUE(Press(page, MenuInput{.activate = true}).changed);
        EXPECT_EQ(page.Settings().fovDegrees, want);
    }
    // Play bounds ([wp-5]) toggles.
    Press(page, MenuInput{.down = true});
    EXPECT_TRUE(Press(page, MenuInput{.activate = true}).changed);
    EXPECT_FALSE(page.Settings().playBounds);
    EXPECT_TRUE(Press(page, MenuInput{.activate = true}).changed);
    EXPECT_TRUE(page.Settings().playBounds);
    // The volumes (audio.md "Settings") step by 20 and wrap to 0; a value that is no step goes to the next one.
    Press(page, MenuInput{.down = true});
    for (int want : {0, 20, 40}) {
        EXPECT_TRUE(Press(page, MenuInput{.activate = true}).changed);
        EXPECT_EQ(page.Settings().masterVolume, want);
    }
    for (int i = 0; i < 4; ++i) Press(page, MenuInput{.down = true});
    EXPECT_TRUE(Press(page, MenuInput{.activate = true}).changed);
    EXPECT_EQ(page.Settings().musicVolume, 80) << "70 from the defaults goes to the next step";

    GameSettings odd;
    odd.renderScale = 0.9f;   // a value from the file that is no preset
    SettingsPage other;
    other.Open(odd);
    EXPECT_TRUE(Press(other, MenuInput{.activate = true}).changed);
    EXPECT_FLOAT_EQ(other.Settings().renderScale, 0.5f);
}

TEST(SettingsPage, BackClosesThePage) {
    GameSettings start;
    start.fovDegrees = 90;
    SettingsPage page;
    page.Open(start);
    SettingsPage::Result r = Press(page, MenuInput{.back = true});
    EXPECT_TRUE(r.closed);
    EXPECT_FALSE(r.changed);
    EXPECT_FALSE(page.IsOpen());
    EXPECT_EQ(page.Settings(), start);

    page.Open(start);
    for (int i = 0; i < 10; ++i) Press(page, MenuInput{.down = true});
    r = Press(page, MenuInput{.activate = true});
    EXPECT_TRUE(r.closed);
    EXPECT_FALSE(r.changed);
    EXPECT_FALSE(page.IsOpen());
    EXPECT_EQ(page.Settings(), start);
    // A closed page takes no input.
    r = Press(page, MenuInput{.activate = true});
    EXPECT_FALSE(r.closed);
    EXPECT_FALSE(r.changed);
}
