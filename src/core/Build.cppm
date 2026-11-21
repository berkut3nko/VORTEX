module;

#include <string>
#include <string_view>

export module vortex.core:build;

export namespace vortex {

    /**
     * @brief Which build this process is (engine/cmake/BuildInfo.cmake): logged first by the game,
     * vortex_server and vortex_bot (`Build: ...`) and printed by their --version. The commits are 12
     * hex digits, with "-dirty" when the checkout had uncommitted tracked changes or untracked files
     * in its source folders (src, tests, tools, cmake, shaders, CMakeLists.txt); "unknown" without
     * git. The game fields are empty when the engine is built on its own.
     */
    struct BuildInfo {
        std::string_view engineCommit;
        std::string_view engineBranch;
        std::string_view gameName;
        std::string_view gameCommit;
        std::string_view gameBranch;
        std::string_view buildType;   ///< CMAKE_BUILD_TYPE (RelWithDebInfo, Debug, Release ...).
        std::string_view compiler;    ///< Compiler id and version ("Clang 22.1.0").
        std::string_view cpuFlags;    ///< The instruction-set flags every engine unit is built with (Jolt's).
    };

    const BuildInfo& GetBuildInfo();
    /// @brief One line: "Shredborne 1c45cd2a8b3f (engine-rewrite), VORTEX a23af7a1c9d0 (engine-rewrite),
    /// RelWithDebInfo, Clang 22.1.0, -mavx2 -mbmi ...".
    std::string DescribeBuild();
    /// @brief "VORTEX-BUILD-STAMP engine=<engine commit> game=<game commit>": kept in every binary as
    /// a NUL-terminated string (even unused), so tools/install-live.sh can tell which commits a binary
    /// was built from without running it. Its format is pinned by BuildInfo.DescribesTheBuild.
    std::string_view BuildStamp();
}
