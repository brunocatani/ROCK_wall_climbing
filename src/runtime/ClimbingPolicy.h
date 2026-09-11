#pragma once

#include <cstdint>
#include <span>

namespace rock_wall_climbing::policy
{
    struct Vec3
    {
        float x{ 0.0f };
        float y{ 0.0f };
        float z{ 0.0f };
    };

    [[nodiscard]] bool finite(Vec3 value) noexcept;
    [[nodiscard]] float lengthSquared(Vec3 value) noexcept;
    [[nodiscard]] float length(Vec3 value) noexcept;
    [[nodiscard]] float dot(Vec3 left, Vec3 right) noexcept;
    [[nodiscard]] Vec3 add(Vec3 left, Vec3 right) noexcept;
    [[nodiscard]] Vec3 subtract(Vec3 left, Vec3 right) noexcept;
    [[nodiscard]] Vec3 scale(Vec3 value, float factor) noexcept;
    [[nodiscard]] Vec3 divide(Vec3 value, float divisor) noexcept;
    [[nodiscard]] Vec3 clampMagnitude(Vec3 value, float maximum) noexcept;
    [[nodiscard]] bool tryNormalize(Vec3 value, Vec3& normalized) noexcept;

    struct ActiveHands
    {
        bool right{ false };
        bool left{ false };
    };

    [[nodiscard]] constexpr ActiveHands decodeActiveHands(
        const std::uint32_t activeHandMask,
        const std::uint32_t rightHandMask,
        const std::uint32_t leftHandMask) noexcept
    {
        return {
            (activeHandMask & rightHandMask) != 0,
            (activeHandMask & leftHandMask) != 0,
        };
    }

    struct HandMotionInput
    {
        Vec3 previousHandOffset{};
        Vec3 currentHandOffset{};
        bool previousAnchorValid{ false };
        bool currentAnchorValid{ false };
        Vec3 previousAnchor{};
        Vec3 currentAnchor{};
        float movementScale{ 1.0f };
        float maximumDelta{ 24.0f };
        bool followMovingSurface{ true };
    };

    struct HandMotionResult
    {
        bool valid{ false };
        bool discontinuity{ false };
        Vec3 pullDelta{};
        Vec3 surfaceCarryDelta{};
        Vec3 totalDelta{};
    };

    [[nodiscard]] HandMotionResult evaluateHandMotion(
        const HandMotionInput& input) noexcept;

    inline constexpr float HAND_JOIN_BLEND_SECONDS = 0.08f;

    struct HandMotionContribution
    {
        bool valid{ false };
        float weight{ 0.0f };
        Vec3 movementDelta{};
        Vec3 pullDelta{};
    };

    struct HandMotionBlend
    {
        bool valid{ false };
        std::uint32_t contributionCount{ 0 };
        float totalWeight{ 0.0f };
        Vec3 movementDelta{};
        Vec3 pullDelta{};
    };

    [[nodiscard]] float advanceHandBlendWeight(
        float currentWeight,
        float deltaSeconds,
        float rampSeconds) noexcept;

    [[nodiscard]] HandMotionBlend blendHandMotions(
        std::span<const HandMotionContribution> contributions) noexcept;

    inline constexpr float HAND_ACTIVITY_FULL_RESPONSE_DISTANCE = 3.0f;
    inline constexpr float HELD_HAND_MINIMUM_ACTIVITY_WEIGHT = 0.25f;

    [[nodiscard]] float activityAdjustedHandWeight(
        float baseWeight,
        Vec3 pullDelta,
        bool releasing) noexcept;

    inline constexpr float ADAPTIVE_SMOOTHING_QUIET_MULTIPLIER = 0.75f;
    inline constexpr float ADAPTIVE_SMOOTHING_ACTIVE_MULTIPLIER = 1.45f;
    inline constexpr float ADAPTIVE_SMOOTHING_FULL_RESPONSE_DISTANCE = 12.0f;

    [[nodiscard]] float adaptiveSmoothingSpeed(
        float baseSpeed,
        float targetLead) noexcept;

    [[nodiscard]] float exponentialSmoothingFactor(
        float speed,
        float deltaSeconds) noexcept;

    [[nodiscard]] bool slideAlongCollisionPlane(
        Vec3 remainingMotion,
        Vec3 hitNormal,
        Vec3 castDirection,
        Vec3& slidingMotion) noexcept;

    struct VelocitySample
    {
        Vec3 displacement{};
        float deltaSeconds{ 0.0f };
    };

    struct LaunchSettings
    {
        bool enabled{ true };
        float historySeconds{ 0.10f };
        float directionFilterDegrees{ 75.0f };
        float multiplier{ 1.15f };
        float horizontalBoost{ 1.05f };
        float minimumSpeed{ 35.0f };
        float maximumSpeed{ 450.0f };
    };

    [[nodiscard]] Vec3 calculateLaunchVelocity(
        std::span<const VelocitySample> samples,
        const LaunchSettings& settings) noexcept;

    inline constexpr float CHARACTER_GRAVITY_ACCELERATION_SCALE = 700.0f;
    inline constexpr float MAXIMUM_NATIVE_JUMP_HEIGHT_GAME = 256.0f;

    [[nodiscard]] Vec3 addLaunchImpulse(
        Vec3 currentVelocity,
        Vec3 launchImpulse,
        float maximumSpeed) noexcept;

    [[nodiscard]] float nativeJumpHeightForVelocity(
        float upwardVelocity,
        float gravityScalar) noexcept;

    // Exact vanilla world-surface domain used by ROCK's FixedAnchor dynamic
    // hand proxies. Dynamic car proxy layers are intentionally omitted.
    inline constexpr std::uint64_t CLIMBABLE_WORLD_LAYER_MASK =
        (std::uint64_t{ 1 } << 1) |   // STATIC
        (std::uint64_t{ 1 } << 2) |   // ANIMSTATIC
        (std::uint64_t{ 1 } << 3) |   // TRANSPARENT
        (std::uint64_t{ 1 } << 9) |   // TREES
        (std::uint64_t{ 1 } << 13) |  // TERRAIN
        (std::uint64_t{ 1 } << 17) |  // GROUND
        (std::uint64_t{ 1 } << 26) |  // TRANSPARENT_SMALL
        (std::uint64_t{ 1 } << 27) |  // INVISIBLE_WALL
        (std::uint64_t{ 1 } << 28) |  // TRANSPARENT_SMALL_ANIM
        (std::uint64_t{ 1 } << 31) |  // STAIRHELPER
        (std::uint64_t{ 1 } << 34) |  // AVOIDBOX
        (std::uint64_t{ 1 } << 35);   // COLLISIONBOX
}
