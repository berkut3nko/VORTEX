module;

#include <glm/glm.hpp>

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

module vortex.core;

namespace vortex {

    namespace {
        std::string_view Trim(std::string_view text) {
            const size_t begin = text.find_first_not_of(" \t");
            if (begin == std::string_view::npos) return {};
            const size_t end = text.find_last_not_of(" \t");
            return text.substr(begin, end - begin + 1);
        }

        /// The whole of `text` as a number; nothing may be left over.
        template <typename T>
        bool ParseWhole(std::string_view text, T& out) {
            if (text.empty()) return false;
            T value{};
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (ec != std::errc() || end != text.data() + text.size()) return false;
            out = value;
            return true;
        }

        /// The first preset greater than `current` (+ 0.001: no float equality), else the first.
        float NextPreset(std::initializer_list<float> presets, float current) {
            for (float p : presets)
                if (p > current + 0.001f) return p;
            return *presets.begin();
        }
    }

    bool ParseGameSettings(std::string_view text, GameSettings& out, std::string* error) {
        GameSettings s = out;
        size_t lineNumber = 0;
        for (size_t start = 0; start <= text.size();) {
            ++lineNumber;
            const size_t newline = text.find('\n', start);
            std::string_view line = text.substr(start, newline == std::string_view::npos ? std::string_view::npos : newline - start);
            start = newline == std::string_view::npos ? text.size() + 1 : newline + 1;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            line = Trim(line);
            if (line.empty() || line.front() == '#') continue;
            const size_t equals = line.find('=');
            if (equals == std::string_view::npos) {
                if (error != nullptr) *error = std::format("line {}: expected key = value", lineNumber);
                return false;
            }
            const std::string_view key = Trim(line.substr(0, equals));
            const std::string_view value = Trim(line.substr(equals + 1));
            bool ok = true;
            if (key == "render_scale") {
                float v = 0.0f;
                ok = ParseWhole(value, v) && std::isfinite(v) && v >= 0.25f && v <= 1.0f;
                if (ok) s.renderScale = v;
            } else if (key == "vsync") {
                ok = value == "on" || value == "off";
                if (ok) s.vsync = value == "on";
            } else if (key == "sensitivity") {
                float v = 0.0f;
                ok = ParseWhole(value, v) && std::isfinite(v) && v >= 0.25f && v <= 4.0f;
                if (ok) s.sensitivity = v;
            } else if (key == "fov") {
                int v = 0;
                ok = ParseWhole(value, v) && v >= 50 && v <= 110;
                if (ok) s.fovDegrees = v;
            }
            // Any other key is skipped: a newer game's file stays readable.
            if (!ok) {
                if (error != nullptr) *error = std::format("line {}: {}: bad value '{}'", lineNumber, key, value);
                return false;
            }
        }
        out = s;
        return true;
    }

    std::string FormatGameSettings(const GameSettings& settings) {
        return std::format("# Shredborne settings (key = value)\nrender_scale = {:.2f}\nvsync = {}\nsensitivity = {:.2f}\nfov = {}\n",
                           settings.renderScale, settings.vsync ? "on" : "off", settings.sensitivity, settings.fovDegrees);
    }

    std::filesystem::path DefaultSettingsPath() {
#ifdef _WIN32
        if (const char* appData = std::getenv("APPDATA"); appData != nullptr && *appData != '\0')
            return std::filesystem::path(appData) / "Shredborne" / "settings.txt";
        return {};
#else
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0')
            return std::filesystem::path(xdg) / "shredborne" / "settings.txt";
        if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
            return std::filesystem::path(home) / ".config" / "shredborne" / "settings.txt";
        return {};
#endif
    }

    std::optional<GameSettings> LoadGameSettings(const std::filesystem::path& path) {
        if (path.empty()) return std::nullopt;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) return std::nullopt;   // no file yet: the defaults, quietly
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            Log::Warn("Settings: {}: cannot be read", path.string());
            return std::nullopt;
        }
        std::ostringstream text;
        text << in.rdbuf();
        GameSettings settings;
        std::string error;
        if (!ParseGameSettings(text.str(), settings, &error)) {
            Log::Warn("Settings: {}: {}", path.string(), error);
            return std::nullopt;
        }
        return settings;
    }

    bool SaveGameSettings(const std::filesystem::path& path, const GameSettings& settings) {
        if (path.empty()) return false;
        std::error_code ec;
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            Log::Warn("Settings: {}: {}", path.string(), ec.message());
            return false;
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << FormatGameSettings(settings);
        out.close();
        if (!out) {
            Log::Warn("Settings: {}: cannot be written", path.string());
            return false;
        }
        return true;
    }

    void SettingsPage::Open(const GameSettings& current) {
        m_Settings = current;
        m_Open = true;
        m_Menu.SetTitle("SETTINGS");
        m_Menu.SetFooter("Enter / click changes   Esc back");
        Rebuild();
        m_Menu.Open();
    }

    void SettingsPage::Rebuild() {
        const int focus = m_Menu.Focus();   // ClearItems resets it
        m_Menu.ClearItems();
        m_Menu.AddItem(std::format("Render scale   {:.2f}", m_Settings.renderScale), kRenderScale);
        m_Menu.AddItem(std::format("VSync          {}", m_Settings.vsync ? "on" : "off"), kVsync);
        m_Menu.AddItem(std::format("Mouse speed    {:.2f}", m_Settings.sensitivity), kSensitivity);
        m_Menu.AddItem(std::format("Field of view  {}", m_Settings.fovDegrees), kFov);
        m_Menu.AddItem("Back", kBack);
        m_Menu.SetFocus(focus);
    }

    SettingsPage::Result SettingsPage::HandleInput(const MenuInput& input, glm::vec2 screen, int scale) {
        Result result;
        if (!m_Open) return result;
        m_Menu.Layout(screen, scale);
        const MenuEvent event = m_Menu.HandleInput(input);
        if (event.kind == MenuEvent::Kind::Back || (event.kind == MenuEvent::Kind::Activated && event.id == kBack)) {
            m_Open = false;
            result.closed = true;
            return result;
        }
        if (event.kind != MenuEvent::Kind::Activated) return result;
        switch (event.id) {
            case kRenderScale: m_Settings.renderScale = NextPreset({0.5f, 0.6f, 0.75f}, m_Settings.renderScale); break;
            case kVsync: m_Settings.vsync = !m_Settings.vsync; break;
            case kSensitivity: m_Settings.sensitivity = NextPreset({0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f}, m_Settings.sensitivity); break;
            case kFov: {
                int next = 60;
                for (int p : {60, 70, 80, 90, 100}) {
                    if (p > m_Settings.fovDegrees) {
                        next = p;
                        break;
                    }
                }
                m_Settings.fovDegrees = next;
                break;
            }
            default: return result;
        }
        Rebuild();
        result.changed = true;
        return result;
    }

    void SettingsPage::Draw(HudCanvas& canvas, glm::vec2 screen, int scale) {
        if (!m_Open) return;
        m_Menu.Layout(screen, scale);
        m_Menu.Draw(canvas);
    }
}
