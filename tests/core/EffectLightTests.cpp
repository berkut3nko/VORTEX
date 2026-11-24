// Effect lights (fx-budget.md "Effect lights", [wp-8]): the CPU merge that keeps at most eight
// unshadowed lights, intensity-weighted, with the union of their axis clip boxes.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

import vortex.fx;

using vortex::fx::EffectLightIn;
using vortex::fx::kMaxEffectLights;
using vortex::fx::MergeEffectLights;

namespace {
    const glm::dvec3 kCamera(0.0, 10.0, 0.0);

    /// A light of `intensity` at `position`, radius 4, with a unit box around it as clip box.
    EffectLightIn Light(const glm::dvec3& position, float intensity) {
        EffectLightIn light;
        light.position = position;
        light.color = glm::vec3(1.0f, 0.7f, 0.35f);
        light.intensity = intensity;
        light.radius = 4.0f;
        light.clipMin = position - glm::dvec3(1.0);
        light.clipMax = position + glm::dvec3(1.0);
        return light;
    }
}

TEST(EffectLights, DISABLED_AtMostEightAfterTheMerge) {
    // 20 lights 60 m apart: no two are within half a radius of each other.
    std::vector<EffectLightIn> in;
    float total = 0.0f;
    for (int i = 0; i < 20; ++i) {
        in.push_back(Light(glm::dvec3(60.0 * i, 10.0, 0.0), 10.0f - 0.25f * static_cast<float>(i)));
        total += in.back().intensity;
    }
    std::vector<EffectLightIn> out;
    MergeEffectLights(in, kCamera, out);
    EXPECT_EQ(out.size(), static_cast<size_t>(kMaxEffectLights));
    // A light dropped by the slot limit is merged into the nearest slot: nothing is lost.
    float merged = 0.0f;
    for (const EffectLightIn& l : out) merged += l.intensity;
    EXPECT_NEAR(merged, total, 1e-3f);
}

TEST(EffectLights, DISABLED_NearLightsMergeIntensityWeighted) {
    // 0.5 m apart, both radius 4 (within half of it): one slot, 3/4 of the way to the stronger one.
    const std::vector<EffectLightIn> in = {Light(glm::dvec3(10.0, 10.0, 0.0), 1.0f), Light(glm::dvec3(10.5, 10.0, 0.0), 3.0f)};
    std::vector<EffectLightIn> out;
    MergeEffectLights(in, kCamera, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_NEAR(out[0].position.x, 10.375, 1e-9);
    EXPECT_NEAR(out[0].position.y, 10.0, 1e-9);
    EXPECT_NEAR(out[0].intensity, 4.0f, 1e-6f);
    EXPECT_FLOAT_EQ(out[0].radius, 4.0f);
}

TEST(EffectLights, DISABLED_ClipBoxIsTheUnion) {
    EffectLightIn a = Light(glm::dvec3(10.0, 10.0, 0.0), 2.0f);
    a.clipMin = glm::dvec3(0.0);
    a.clipMax = glm::dvec3(1.0);
    EffectLightIn b = Light(glm::dvec3(10.1, 10.0, 0.0), 5.0f);
    b.clipMin = glm::dvec3(2.0);
    b.clipMax = glm::dvec3(3.0);
    const std::vector<EffectLightIn> in = {a, b};
    std::vector<EffectLightIn> out;
    MergeEffectLights(in, kCamera, out);
    ASSERT_EQ(out.size(), 1u);
    for (int axis = 0; axis < 3; ++axis) {
        EXPECT_DOUBLE_EQ(out[0].clipMin[axis], 0.0) << axis;
        EXPECT_DOUBLE_EQ(out[0].clipMax[axis], 3.0) << axis;
    }
}
