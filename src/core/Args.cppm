module;

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module vortex.core:args;

export namespace vortex {

    /**
     * @brief Minimal command line parser.
     * @details Accepts `--flag`, `--key=value` and `--key value`.
     */
    class CommandLine {
    public:
        CommandLine() = default;
        CommandLine(int argc, const char* const* argv) {
            for (int i = 1; i < argc; ++i) m_Args.emplace_back(argv[i]);
        }
        explicit CommandLine(std::vector<std::string> args) : m_Args(std::move(args)) {}

        bool Has(std::string_view name) const {
            for (const auto& a : m_Args) {
                if (a == name) return true;
                if (a.size() > name.size() && a.compare(0, name.size(), name) == 0 && a[name.size()] == '=') return true;
            }
            return false;
        }

        std::optional<std::string> Get(std::string_view name) const {
            for (size_t i = 0; i < m_Args.size(); ++i) {
                const std::string& a = m_Args[i];
                if (a.size() > name.size() && a.compare(0, name.size(), name) == 0 && a[name.size()] == '=') {
                    return a.substr(name.size() + 1);
                }
                if (a == name && i + 1 < m_Args.size() && m_Args[i + 1].rfind("--", 0) != 0) return m_Args[i + 1];
            }
            return std::nullopt;
        }

        std::string GetString(std::string_view name, std::string_view fallback) const {
            auto v = Get(name);
            return v ? *v : std::string(fallback);
        }

        int GetInt(std::string_view name, int fallback) const {
            auto v = Get(name);
            if (!v) return fallback;
            int out = fallback;
            auto [ptr, ec] = std::from_chars(v->data(), v->data() + v->size(), out);
            return ec == std::errc() ? out : fallback;
        }

        float GetFloat(std::string_view name, float fallback) const {
            auto v = Get(name);
            if (!v) return fallback;
            try {
                return std::stof(*v);
            } catch (...) {
                return fallback;
            }
        }

    private:
        std::vector<std::string> m_Args;
    };
}
