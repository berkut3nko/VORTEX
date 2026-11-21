module;

#include <format>
#include <string>
#include <string_view>

// The only file that sees the generated header: a commit changes this object only.
#include "VortexBuildInfo.h"

module vortex.core;

namespace vortex {

    namespace {
        // Emitted even though only BuildStamp reads it (tools/install-live.sh greps the binaries for it).
        [[gnu::used]] constexpr char kBuildStamp[] = "VORTEX-BUILD-STAMP engine=" VORTEX_BUILD_ENGINE_COMMIT " game=" VORTEX_BUILD_GAME_COMMIT;
    }

    std::string_view BuildStamp() { return kBuildStamp; }

    const BuildInfo& GetBuildInfo() {
        static const BuildInfo info{VORTEX_BUILD_ENGINE_COMMIT, VORTEX_BUILD_ENGINE_BRANCH, VORTEX_BUILD_GAME_NAME, VORTEX_BUILD_GAME_COMMIT,
                                    VORTEX_BUILD_GAME_BRANCH,   VORTEX_BUILD_TYPE,          VORTEX_BUILD_COMPILER,  VORTEX_BUILD_CPU_FLAGS};
        return info;
    }

    std::string DescribeBuild() {
        const BuildInfo& b = GetBuildInfo();
        std::string text;
        if (!b.gameName.empty()) text = std::format("{} {} ({}), ", b.gameName, b.gameCommit, b.gameBranch);
        text += std::format("VORTEX {} ({}), {}, {}", b.engineCommit, b.engineBranch, b.buildType.empty() ? "no build type" : b.buildType,
                            b.compiler);
        if (!b.cpuFlags.empty()) text += std::format(", {}", b.cpuFlags);
        return text;
    }
}
