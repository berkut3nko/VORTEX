// WP1 (project review 2026-11-17): what a program says about itself at start-up. The build identity
// (engine/cmake/BuildInfo.cmake, the `Build:` line and --version) and the CPU check's decoder
// (src/startup/CpuCheck.c; the check itself runs before main: CpuCheck.RefusesAMissingFeature in
// tests/CMakeLists.txt runs a program with a feature pretended missing).

#include <gtest/gtest.h>

#include "CpuCheck.h"

#include <array>
#include <string>
#include <string_view>

import vortex.core;

using namespace vortex;

TEST(BuildInfo, DescribesTheBuild) {
    const BuildInfo& b = GetBuildInfo();
    EXPECT_FALSE(b.engineCommit.empty());
    EXPECT_FALSE(b.engineBranch.empty());
    EXPECT_FALSE(b.compiler.empty());
    // A commit is 12 hex digits ("-dirty" after it) or "unknown" (no git).
    if (b.engineCommit != "unknown") {
        ASSERT_GE(b.engineCommit.size(), 12u);
        for (const char c : b.engineCommit.substr(0, 12)) EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << b.engineCommit;
        const std::string_view rest = b.engineCommit.substr(12);
        EXPECT_TRUE(rest.empty() || rest == "-dirty") << b.engineCommit;
    }
    // The supported compiler (CLAUDE.md: Clang), a known build type, and the engine's CPU flags.
    EXPECT_EQ(b.compiler.substr(0, 5), "Clang") << b.compiler;
    bool known = false;
    for (const std::string_view type : {"Debug", "Release", "RelWithDebInfo", "MinSizeRel"}) known = known || b.buildType == type;
    EXPECT_TRUE(known) << b.buildType;
#if defined(__x86_64__)
    EXPECT_NE(b.cpuFlags.find("-mavx2"), std::string_view::npos) << b.cpuFlags;
#endif
    const std::string line = DescribeBuild();
    EXPECT_NE(line.find("VORTEX " + std::string(b.engineCommit)), std::string::npos) << line;
    if (!b.gameName.empty()) EXPECT_EQ(line.rfind(std::string(b.gameName) + " ", 0), 0u) << line;
    // tools/install-live.sh greps the binaries for exactly this text, NUL-terminated.
    EXPECT_EQ(BuildStamp(), "VORTEX-BUILD-STAMP engine=" + std::string(b.engineCommit) + " game=" + std::string(b.gameCommit));
}

namespace {
    /// A CPU with everything the build needs (a Haswell): the bits CpuCheck.c reads.
    VortexCpuBits Haswell() {
        VortexCpuBits b{};
        b.maxLeaf = 13;
        b.maxExtendedLeaf = 0x80000008u;
        b.leaf1Ecx = (1u << 19) | (1u << 20) | (1u << 23) | (1u << 12) | (1u << 29) | (1u << 27) | (1u << 28);
        b.leaf7Ebx = (1u << 5) | (1u << 3);
        b.extended1Ecx = 1u << 5;
        b.xcr0 = 0x7;
        return b;
    }

    std::string Names(unsigned mask) {
        std::array<char, 128> text{};
        vortex_cpu_names(mask, text.data(), text.size());
        return text.data();
    }
}

TEST(CpuCheck, NamesEveryMissingFeature) {
    VortexCpuBits b = Haswell();
    EXPECT_EQ(vortex_cpu_missing(&b), 0u);

    // An OS that does not save the AVX registers: every AVX-encoded feature is unusable.
    b.xcr0 = 0x3;
    EXPECT_EQ(Names(vortex_cpu_missing(&b)), "AVX, AVX2, FMA, F16C");
    // OSXSAVE clear: XGETBV is never run (the reader leaves xcr0 0), the same four are missing.
    b = Haswell();
    b.leaf1Ecx &= ~(1u << 27);
    b.xcr0 = 0;
    EXPECT_EQ(Names(vortex_cpu_missing(&b)), "AVX, AVX2, FMA, F16C");

    b = Haswell();
    b.extended1Ecx = 0;
    EXPECT_EQ(Names(vortex_cpu_missing(&b)), "LZCNT");
    b.maxExtendedLeaf = 0x80000000u;   // the leaf is not there: whatever it read does not count
    b.extended1Ecx = 1u << 5;
    EXPECT_EQ(Names(vortex_cpu_missing(&b)), "LZCNT");

    // A Sandy Bridge (AVX, no AVX2 / FMA / BMI / LZCNT / F16C).
    b = Haswell();
    b.leaf1Ecx &= ~((1u << 12) | (1u << 29));
    b.leaf7Ebx = 0;
    b.extended1Ecx = 0;
    EXPECT_EQ(Names(vortex_cpu_missing(&b)), "AVX2, FMA, BMI1, LZCNT, F16C");
    // Leaf 7 above the maximum leaf reads as nothing.
    b = Haswell();
    b.maxLeaf = 6;
    EXPECT_EQ(Names(vortex_cpu_missing(&b)), "AVX2, BMI1");

    // The test hook's names, any case.
    EXPECT_EQ(vortex_cpu_parse_names("avx2"), static_cast<unsigned>(VORTEX_CPU_AVX2));
    EXPECT_EQ(vortex_cpu_parse_names("Lzcnt, sse4.2,nothing"), static_cast<unsigned>(VORTEX_CPU_LZCNT | VORTEX_CPU_SSE42));
    EXPECT_EQ(vortex_cpu_parse_names(nullptr), 0u);
    EXPECT_EQ(vortex_cpu_parse_names(""), 0u);
    // A short buffer is cut, never overrun.
    std::array<char, 8> small{};
    vortex_cpu_names(VORTEX_CPU_AVX2 | VORTEX_CPU_FMA | VORTEX_CPU_LZCNT, small.data(), small.size());
    EXPECT_EQ(std::string(small.data()), "AVX2, F");
}
