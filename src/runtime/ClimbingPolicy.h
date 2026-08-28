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
    [[nodiscard]] Vec3 add(Vec3 left, Vec3 right) noexcept;
    [[nodiscard]] Vec3 subtract(Vec3 left, Vec3 right) noexcept;
    [[nodiscard]] Vec3 scale(Vec3 value, float factor) noexcept;
    [[nodiscard]] Vec3 divide(Vec3 value, float divisor) noexcept;
    [[nodiscard]] Vec3 clampMagnitude(Vec3 value, float maximum) noexcept;

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

    [[nodiscard]] float exponentialSmoothingFactor(
        float speed,
        float deltaSeconds) noexcept;

    struct VelocitySample
    {
        Vec3 displacement{};
        float deltaSeconds{ 0.0f };
    };

    struct LaunchSettings
    {
        bool enabled{ true };
        float historySeconds{ 0.18f };
        float directionFilterDegrees{ 75.0f };
        float multiplier{ 1.15f };
        float horizontalBoost{ 1.05f };
        float minimumSpeed{ 35.0f };
        float maximumSpeed{ 450.0f };
    };

    [[nodiscard]] Vec3 calculateLaunchVelocity(
        std::span<const VelocitySample> samples,
        const LaunchSettings& settings) noexcept;

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
