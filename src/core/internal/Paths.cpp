module;

#include <filesystem>
#include <mutex>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
#elif defined(__APPLE__)
#   include <mach-o/dyld.h>
#   include <vector>
#endif

module vortex.core;

namespace vortex {

    namespace {
        std::filesystem::path ComputeExecutableDir() {
            std::error_code ec;
#if defined(_WIN32)
            wchar_t buffer[MAX_PATH];
            const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
            if (n > 0 && n < MAX_PATH) return std::filesystem::path(buffer).parent_path();
#elif defined(__APPLE__)
            uint32_t size = 0;
            _NSGetExecutablePath(nullptr, &size);
            std::vector<char> buffer(size);
            if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
                auto p = std::filesystem::canonical(buffer.data(), ec);
                if (!ec) return p.parent_path();
            }
#else
            auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
            if (!ec) return p.parent_path();
#endif
            return std::filesystem::current_path(ec);
        }

        std::filesystem::path g_AssetRoot;
        std::once_flag g_AssetRootOnce;
    }

    const std::filesystem::path& Paths::ExecutableDir() {
        static const std::filesystem::path dir = ComputeExecutableDir();
        return dir;
    }

    const std::filesystem::path& Paths::AssetRoot() {
        std::call_once(g_AssetRootOnce, [] {
            if (g_AssetRoot.empty()) g_AssetRoot = ExecutableDir() / "assets";
        });
        return g_AssetRoot;
    }

    void Paths::SetAssetRoot(const std::filesystem::path& root) {
        // Force the lazy default first so it cannot overwrite an explicit root later.
        (void)AssetRoot();
        g_AssetRoot = root;
    }

    std::filesystem::path Paths::Asset(std::string_view relative) { return AssetRoot() / relative; }
}
