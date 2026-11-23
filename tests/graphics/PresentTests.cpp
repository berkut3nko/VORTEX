// The present mode choice of the swapchain (perf.md P1): the preference's order against what the
// surface offers, FIFO as the fallback the spec guarantees. No Vulkan device is created.

#include <gtest/gtest.h>

#include <vulkan/vulkan.h>

#include <initializer_list>
#include <string_view>
#include <vector>

import vortex.graphics;

using namespace vortex::graphics;

namespace {
    VkPresentModeKHR Choose(PresentPreference preference, std::initializer_list<VkPresentModeKHR> available) {
        const std::vector<VkPresentModeKHR> modes(available);
        return ChoosePresentMode(preference, modes);
    }
    constexpr VkPresentModeKHR F = VK_PRESENT_MODE_FIFO_KHR;
    constexpr VkPresentModeKHR R = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    constexpr VkPresentModeKHR I = VK_PRESENT_MODE_IMMEDIATE_KHR;
    constexpr VkPresentModeKHR M = VK_PRESENT_MODE_MAILBOX_KHR;
}

TEST(Present, ChoosesThePreferredModeOrFifo) {
    EXPECT_EQ(Choose(PresentPreference::FifoRelaxed, {F, R}), R);
    EXPECT_EQ(Choose(PresentPreference::FifoRelaxed, {F, M, I}), F);
    EXPECT_EQ(Choose(PresentPreference::Fifo, {F, R, I, M}), F);
    EXPECT_EQ(Choose(PresentPreference::Immediate, {F, M, I}), I);
    EXPECT_EQ(Choose(PresentPreference::Immediate, {F, M}), M);
    EXPECT_EQ(Choose(PresentPreference::Immediate, {F}), F);
    EXPECT_EQ(Choose(PresentPreference::FifoRelaxed, {}), F);
}

TEST(Present, NamesAndParsing) {
    EXPECT_EQ(std::string_view(PresentModeName(F)), "fifo");
    EXPECT_EQ(std::string_view(PresentModeName(R)), "fifo-relaxed");
    EXPECT_EQ(std::string_view(PresentModeName(I)), "immediate");
    EXPECT_EQ(std::string_view(PresentModeName(M)), "mailbox");
    EXPECT_EQ(std::string_view(PresentModeName(VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR)), "other");

    for (PresentPreference p : {PresentPreference::Fifo, PresentPreference::FifoRelaxed, PresentPreference::Immediate}) {
        PresentPreference parsed = PresentPreference::Fifo;
        ASSERT_TRUE(ParsePresentPreference(PresentPreferenceName(p), parsed)) << PresentPreferenceName(p);
        EXPECT_EQ(parsed, p);
    }
    EXPECT_EQ(std::string_view(PresentPreferenceName(PresentPreference::FifoRelaxed)), "relaxed");
    PresentPreference out = PresentPreference::Fifo;
    EXPECT_TRUE(ParsePresentPreference("relaxed", out));
    EXPECT_EQ(out, PresentPreference::FifoRelaxed);
    out = PresentPreference::Immediate;
    EXPECT_FALSE(ParsePresentPreference("vsync", out));
    EXPECT_FALSE(ParsePresentPreference("", out));
    EXPECT_EQ(out, PresentPreference::Immediate);   // unchanged on failure
}
