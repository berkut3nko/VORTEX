// WP1 (project review 2026-11-17): --validation without the Khronos validation layer fails start-up
// (the game exits 2) instead of running without it, so an automated run that exits 0 under
// --validation proves the layer ran. graphics::DecideValidation is the rule Device::Initialize applies.

#include <gtest/gtest.h>

import vortex.graphics;

using namespace vortex;

TEST(Validation, ARequestWithoutTheLayerFailsStartUp) {
    // Requested, the layer and its debug utils there: on.
    graphics::ValidationDecision d = graphics::DecideValidation(true, true, true);
    EXPECT_TRUE(d.enable);
    EXPECT_FALSE(d.fail);
    // Requested without the layer, or without its debug utils extension: start-up fails.
    d = graphics::DecideValidation(true, false, true);
    EXPECT_FALSE(d.enable);
    EXPECT_TRUE(d.fail);
    d = graphics::DecideValidation(true, true, false);
    EXPECT_FALSE(d.enable);
    EXPECT_TRUE(d.fail);
    // Not requested: off, whatever is installed.
    for (const bool layer : {false, true}) {
        d = graphics::DecideValidation(false, layer, true);
        EXPECT_FALSE(d.enable);
        EXPECT_FALSE(d.fail);
    }
}
