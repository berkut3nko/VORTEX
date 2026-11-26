module;

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

module vortex.audio;

namespace vortex::audio {

    const char* ToString(Bus bus) {
        switch (bus) {
            case Bus::Master: return "master";
            case Bus::Sfx: return "sfx";
            case Bus::Ambience: return "ambience";
            case Bus::Ui: return "ui";
            case Bus::Music: return "music";
        }
        return "?";
    }

    DistanceRange RangeOf(Distance distance) {
        switch (distance) {
            case Distance::Flat: return {0.0f, 0.0f};
            case Distance::Small: return {2.0f, 25.0f};
            case Distance::Medium: return {3.0f, 60.0f};
            case Distance::Large: return {5.0f, 150.0f};
            case Distance::Huge: return {10.0f, 300.0f};
        }
        return {0.0f, 0.0f};
    }

    float DistanceGain(Distance distance, float metres) {
        if (distance == Distance::Flat) return 1.0f;
        const DistanceRange r = RangeOf(distance);
        if (metres <= r.inner) return 1.0f;
        if (metres >= r.outer) return 0.0f;
        return (r.inner / metres) * (r.outer - metres) / (r.outer - r.inner);
    }

    namespace {
        bool ParseFloat(std::string_view text, float& out) {
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
            return ec == std::errc() && end == text.data() + text.size() && std::isfinite(out);
        }
        bool ParseUint(std::string_view text, uint32_t& out) {
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
            return ec == std::errc() && end == text.data() + text.size();
        }
        bool ParseBool(std::string_view text, bool& out) {
            if (text == "0") out = false;
            else if (text == "1") out = true;
            else return false;
            return true;
        }
        bool ValidId(std::string_view id) {
            // kind.category[.more]: lower-case letters, digits, '_' and dots; at least two parts.
            if (id.empty() || id.front() == '.' || id.back() == '.' || id.find('.') == std::string_view::npos) return false;
            if (id.find("..") != std::string_view::npos) return false;
            return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.'; });
        }
    }

    bool Manifest::Parse(std::string_view text, std::string* error) {
        std::vector<SoundDesc> sounds;
        std::unordered_map<std::string, SoundId> index;
        const auto fail = [&](size_t line, const std::string& what) {
            if (error) *error = std::format("line {}: {}", line, what);
            return false;
        };
        size_t lineNo = 0;
        size_t pos = 0;
        while (pos <= text.size()) {
            const size_t end = std::min(text.find('\n', pos), text.size());
            std::string_view line = text.substr(pos, end - pos);
            pos = end + 1;
            ++lineNo;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            const size_t first = line.find_first_not_of(" \t");
            if (first == std::string_view::npos || line[first] == '#') {
                if (end == text.size()) break;
                continue;
            }
            line.remove_prefix(first);
            // Tokens separated by spaces: the id, then key=value pairs.
            std::vector<std::string_view> tokens;
            for (size_t p = 0; p < line.size();) {
                const size_t q = std::min(line.find_first_of(" \t", p), line.size());
                if (q > p) tokens.push_back(line.substr(p, q - p));
                p = q + 1;
            }
            SoundDesc desc;
            desc.id = std::string(tokens[0]);
            if (!ValidId(desc.id)) return fail(lineNo, std::format("'{}' is not a sound id (kind.category...)", desc.id));
            if (index.contains(desc.id)) return fail(lineNo, std::format("'{}' is listed twice", desc.id));
            bool haveBus = false, haveVariants = false;
            for (size_t i = 1; i < tokens.size(); ++i) {
                const size_t eq = tokens[i].find('=');
                if (eq == std::string_view::npos) return fail(lineNo, std::format("'{}' is not key=value", tokens[i]));
                const std::string_view key = tokens[i].substr(0, eq), value = tokens[i].substr(eq + 1);
                bool ok = true;
                if (key == "bus") {
                    haveBus = true;
                    if (value == "sfx") desc.bus = Bus::Sfx;
                    else if (value == "ambience") desc.bus = Bus::Ambience;
                    else if (value == "ui") desc.bus = Bus::Ui;
                    else if (value == "music") desc.bus = Bus::Music;
                    else ok = false;
                } else if (key == "variants") {
                    haveVariants = true;
                    ok = ParseUint(value, desc.variants) && desc.variants >= 1 && desc.variants <= 32;
                } else if (key == "loop") {
                    ok = ParseBool(value, desc.loop);
                } else if (key == "gain") {
                    ok = ParseFloat(value, desc.gainDb) && desc.gainDb >= -60.0f && desc.gainDb <= 12.0f;
                } else if (key == "pitch") {
                    ok = ParseFloat(value, desc.pitchRange) && desc.pitchRange >= 0.0f && desc.pitchRange <= 12.0f;
                } else if (key == "vol") {
                    ok = ParseFloat(value, desc.volumeRange) && desc.volumeRange >= 0.0f && desc.volumeRange <= 24.0f;
                } else if (key == "dist") {
                    if (value == "2D") desc.distance = Distance::Flat;
                    else if (value == "S") desc.distance = Distance::Small;
                    else if (value == "M") desc.distance = Distance::Medium;
                    else if (value == "L") desc.distance = Distance::Large;
                    else if (value == "XL") desc.distance = Distance::Huge;
                    else ok = false;
                } else if (key == "far") {
                    ok = ParseBool(value, desc.far);
                } else if (key == "priority") {
                    uint32_t p = 0;
                    ok = ParseUint(value, p) && p <= 3;
                    desc.priority = static_cast<uint8_t>(p);
                } else {
                    return fail(lineNo, std::format("unknown key '{}'", key));
                }
                if (!ok) return fail(lineNo, std::format("bad value '{}' for {}", value, key));
            }
            if (!haveBus || !haveVariants) return fail(lineNo, std::format("'{}' needs bus= and variants=", desc.id));
            index.emplace(desc.id, static_cast<SoundId>(sounds.size()));
            sounds.push_back(std::move(desc));
            if (end == text.size()) break;
        }
        m_Sounds = std::move(sounds);
        m_Index = std::move(index);
        return true;
    }

    bool Manifest::Load(const std::filesystem::path& path, std::string* error) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            if (error) *error = std::format("cannot read {}", path.string());
            return false;
        }
        std::ostringstream text;
        text << in.rdbuf();
        std::string why;
        if (!Parse(text.str(), &why)) {
            if (error) *error = std::format("{}: {}", path.string(), why);
            return false;
        }
        return true;
    }

    uint32_t Manifest::FileCount() const {
        uint32_t n = 0;
        for (const SoundDesc& s : m_Sounds) n += s.variants;
        return n;
    }

    SoundId Manifest::Find(std::string_view id) const {
        const auto it = m_Index.find(std::string(id));
        return it == m_Index.end() ? kNoSound : it->second;
    }

    std::filesystem::path Manifest::FileOf(const SoundDesc& sound, uint32_t variant) {
        const size_t dot1 = sound.id.find('.');
        const size_t dot2 = sound.id.find('.', dot1 + 1);
        const std::string kind = sound.id.substr(0, dot1);
        const std::string category = sound.id.substr(dot1 + 1, dot2 == std::string::npos ? std::string::npos : dot2 - dot1 - 1);
        return std::filesystem::path(kind) / category / std::format("{}_v{}.ogg", sound.id, variant);
    }
}
