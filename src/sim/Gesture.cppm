module;

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

export module vortex.sim:gesture;

import :spell;

export namespace vortex::sim {

    // --- gestures (docs/design/magic.md "Casting: mana, gestures and projectiles") -------------------
    //  The client samples the aim while RMB is held and classifies the gesture on release; the server
    //  checks the points and decides what they make. Every rule is an integer test on quantised
    //  points (q = 1/16 voxel) and on the input's angle units. The numbers are wire values.

    enum class GestureKind : uint8_t { Tap = 0, Rise = 1, Fall = 2, Line = 3 };
    inline constexpr uint8_t kMaxGestureKind = 3;
    /// SpellGesture::form of an AUTO gesture (0-3 force that sim::Form).
    inline constexpr uint8_t kAutoForm = 4;
    /// The most points a gesture keeps and sends.
    inline constexpr uint32_t kMaxGesturePoints = 16;
    /// Held under this many ticks, a gesture is a tap.
    inline constexpr uint32_t kGestureTapTicks = 12;
    /// 12 degrees in PlayerInput::pitch units ((pi / 2) / 32767 per unit): round(32767 * 12 / 90).
    inline constexpr int32_t kGesturePitchUnits = 4369;

    /// @brief A gesture as the client sends it: its kind, the forced form (or kAutoForm) and its points.
    struct SpellGesture {
        GestureKind kind = GestureKind::Tap;
        uint8_t form = kAutoForm;
        std::vector<glm::ivec3> pointsQ;      ///< at most kMaxGesturePoints, in q; the first is S, the last the release point E
        friend bool operator==(const SpellGesture&, const SpellGesture&) = default;
    };

    /// @brief The camera's angles at the press and at each kept point (PlayerInput units).
    struct GestureAngles {
        int16_t pitch = 0;
        uint16_t yaw = 0;
    };

    /// @brief The form AUTO gives a gesture: Tap Pierce, Rise Plume, Fall Flow, Line Burst.
    Form AutoFormOf(GestureKind kind);
    /// @brief The form a gesture casts: the forced one, else AutoFormOf(kind).
    Form FormOfGesture(const SpellGesture& gesture);
    /// @brief The length of the polyline through `pointsQ` (q): the sum of each segment's floored length.
    uint32_t GesturePathQ(std::span<const glm::ivec3> pointsQ);

    /**
     * @brief Classifies a gesture (first match): Tap when held under kGestureTapTicks, or the path is
     * under 1 m (`metreQ`) and the pitch changed under 12 degrees; Rise when the pitch rose 12 degrees or
     * more and its sweep is larger than the yaw's; Fall the same downward; Line any other with a path of
     * 1 m or more; else Tap.
     * @param angles The angles at the press, then at each kept point (empty: no pitch change, no sweep).
     * @details The change is the last angle less the first; a sweep is the largest less the smallest,
     * the yaw unwrapped step by step; sweeps compare in degrees (pitch x 65536 against yaw x 4 x 32767).
     */
    GestureKind ClassifyGesture(std::span<const glm::ivec3> pointsQ, std::span<const GestureAngles> angles, uint32_t heldTicks,
                                uint32_t metreQ);
}
