module;

#include <glm/glm.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

export module vortex.core:settings;

import :hud;

export namespace vortex {

    /// @brief The player's settings (docs/design/game-ui.md "Settings"). Saved as `key = value` lines.
    struct GameSettings {
        float renderScale = 0.6f;   ///< RenderSettings::renderScale; file 0.25..1.0
        bool vsync = true;          ///< true: PresentPreference::FifoRelaxed, false: Immediate
        float sensitivity = 1.0f;   ///< Multiplier of the game's look sensitivity; file 0.25..4.0
        int fovDegrees = 60;        ///< Vertical field of view; file 50..110
        bool playBounds = true;     ///< RenderSettings::playBounds: draw a session's play bounds ([wp-5])
        /// Sound volumes in percent, file 0..100 (docs/design/audio.md "Settings"): the master bus and the four under it.
        int masterVolume = 100;
        int sfxVolume = 100;
        int ambienceVolume = 80;
        int uiVolume = 80;
        int musicVolume = 70;
        friend bool operator==(const GameSettings&, const GameSettings&) = default;
    };

    /// @brief Parses `text`. On success writes `out` and returns true; on the first bad line returns false,
    /// leaves `out` unchanged and, if `error` is given, writes "line N: ...".
    bool ParseGameSettings(std::string_view text, GameSettings& out, std::string* error);
    /// @brief The file text: a comment line, then render_scale, vsync, sensitivity, fov, play_bounds, master_volume,
    /// sfx_volume, ambience_volume, ui_volume, music_volume.
    std::string FormatGameSettings(const GameSettings& settings);
    /// @brief The default file path: `%APPDATA%/Shredborne/settings.txt` on Windows, else
    /// `$XDG_CONFIG_HOME/shredborne/settings.txt` or `$HOME/.config/shredborne/settings.txt`; empty when no home
    /// directory is known.
    std::filesystem::path DefaultSettingsPath();
    /// @brief Reads and parses `path`; std::nullopt when it is missing or bad (a bad file is logged as a warning).
    std::optional<GameSettings> LoadGameSettings(const std::filesystem::path& path);
    /// @brief Writes `settings` to `path` (parent directories created). false on failure (logged).
    bool SaveGameSettings(const std::filesystem::path& path, const GameSettings& settings);

    /// @brief The HUD settings page shown from the pause menu: one item per setting, Enter / click cycles it.
    class SettingsPage {
    public:
        enum Item : int {
            kRenderScale = 1, kVsync = 2, kSensitivity = 3, kFov = 4, kBack = 5, kPlayBounds = 6,
            kMasterVolume = 7, kSfxVolume = 8, kAmbienceVolume = 9, kUiVolume = 10, kMusicVolume = 11
        };
        struct Result {
            bool changed = false;   ///< A setting changed this frame (apply and save it).
            bool closed = false;    ///< Back or Escape: show the pause menu again.
        };

        void Open(const GameSettings& current);
        bool IsOpen() const { return m_Open; }
        void Close() { m_Open = false; }
        const GameSettings& Settings() const { return m_Settings; }
        /// @brief The focused item's index (a change is a move: the game's menu sound).
        int Focus() const { return m_Menu.Focus(); }
        /// @brief One frame of input (lays the page out for `screen` / `scale` first).
        Result HandleInput(const MenuInput& input, glm::vec2 screen, int scale);
        void Draw(HudCanvas& canvas, glm::vec2 screen, int scale);

    private:
        void Rebuild();             ///< Item labels from m_Settings, focus kept.
        Menu m_Menu;
        GameSettings m_Settings;
        bool m_Open = false;
    };
}
