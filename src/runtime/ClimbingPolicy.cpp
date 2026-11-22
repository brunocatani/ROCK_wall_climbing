#include "runtime/ClimbingPolicy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <numbers>

namespace rock_wall_climbing::policy
{
    bool finite(const Vec3 value) noexcept
    {
        return std::isfinite(value.x) &&
               std::isfinite(value.y) &&
               std::isfinite(value.z);
    }

    float lengthSquared(const Vec3 value) noexcept
    {
        return value.x * value.x + value.y * value.y + value.z * value.z;
    }

    float length(const Vec3 value) noexcept
    {
        const float squared = lengthSquared(value);
        return std::isfinite(squared) && squared > 0.0f ?
            std::sqrt(squared) :
            0.0f;
    }

    float dot(const Vec3 left, const Vec3 right) noexcept
    {
        if (!finite(left) || !finite(right)) {
            return 0.0f;
        }
        return left.x * right.x + left.y * right.y + left.z * right.z;
    }

    Vec3 add(const Vec3 left, const Vec3 right) noexcept
    {
        return Vec3{
            left.x + right.x,
            left.y + right.y,
            left.z + right.z,
        };
    }

    Vec3 subtract(const Vec3 left, const Vec3 right) noexcept
    {
        return Vec3{
            left.x - right.x,
            left.y - right.y,
            left.z - right.z,
        };
    }

    Vec3 scale(const Vec3 value, const float factor) noexcept
    {
        if (!finite(value) || !std::isfinite(factor)) {
            return {};
        }
        return Vec3{
            value.x * factor,
            value.y * factor,
            value.z * factor,
        };
    }

    Vec3 divide(const Vec3 value, const float divisor) noexcept
    {
        if (!std::isfinite(divisor) || std::abs(divisor) <= 1.0e-6f) {
            return {};
        }
        return scale(value, 1.0f / divisor);
    }

    Vec3 clampMagnitude(const Vec3 value, const float maximum) noexcept
    {
        if (!finite(value) || !std::isfinite(maximum) || maximum <= 0.0f) {
            return {};
        }
        const float magnitude = length(value);
        return magnitude > maximum ? scale(value, maximum / magnitude) : value;
    }

    bool tryNormalize(const Vec3 value, Vec3& normalized) noexcept
    {
        normalized = {};
        const float magnitude = length(value);
        if (!finite(value) || !std::isfinite(magnitude) ||
            magnitude <= 1.0e-6f) {
            return false;
        }
        normalized = divide(value, magnitude);
        return finite(normalized);
    }

    HandMotionResult evaluateHandMotion(
        const HandMotionInput& input) noexcept
    {
        HandMotionResult result{};
        if (!finite(input.previousHandOffset) ||
            !finite(input.currentHandOffset) ||
            !std::isfinite(input.movementScale) ||
            input.movementScale <= 0.0f ||
            !std::isfinite(input.maximumDelta) ||
            input.maximumDelta <= 0.0f) {
            result.discontinuity = true;
            return result;
        }

        const Vec3 handDelta = subtract(
            input.currentHandOffset,
            input.previousHandOffset);
        if (!finite(handDelta) || length(handDelta) > input.maximumDelta) {
            result.discontinuity = true;
            return result;
        }

        result.pullDelta = scale(handDelta, -input.movementScale);
        if (input.followMovingSurface &&
            input.previousAnchorValid &&
            input.currentAnchorValid) {
            if (!finite(input.previousAnchor) || !finite(input.currentAnchor)) {
                result.discontinuity = true;
                return result;
            }
            result.surfaceCarryDelta = subtract(
                input.currentAnchor,
                input.previousAnchor);
            if (length(result.surfaceCarryDelta) > input.maximumDelta) {
                result.discontinuity = true;
                return result;
            }
        }

        result.totalDelta = add(result.pullDelta, result.surfaceCarryDelta);
        result.valid = finite(result.totalDelta);
        result.discontinuity = !result.valid;
        return result;
    }

    float advanceHandBlendWeight(
        const float currentWeight,
        const float deltaSeconds,
        const float rampSeconds) noexcept
    {
        if (!std::isfinite(currentWeight)) {
            return 0.0f;
        }
        const float boundedWeight = std::clamp(currentWeight, 0.0f, 1.0f);
        if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f ||
            !std::isfinite(rampSeconds) || rampSeconds <= 0.0f) {
            return boundedWeight;
        }
        return std::min(1.0f, boundedWeight + deltaSeconds / rampSeconds);
    }

    HandMotionBlend blendHandMotions(
        const std::span<const HandMotionContribution> contributions) noexcept
    {
        HandMotionBlend blend{};
        Vec3 weightedMovement{};
        Vec3 weightedPull{};
        for (const auto& contribution : contributions) {
            if (!contribution.valid ||
                !finite(contribution.movementDelta) ||
                !finite(contribution.pullDelta) ||
                !std::isfinite(contribution.weight) ||
                contribution.weight <= 0.0f) {
                continue;
            }
            weightedMovement = add(
                weightedMovement,
                scale(contribution.movementDelta, contribution.weight));
            weightedPull = add(
                weightedPull,
                scale(contribution.pullDelta, contribution.weight));
            blend.totalWeight += contribution.weight;
            ++blend.contributionCount;
        }

        if (!std::isfinite(blend.totalWeight) ||
            blend.totalWeight <= 1.0e-6f ||
            blend.contributionCount == 0) {
            return {};
        }

        blend.movementDelta = divide(weightedMovement, blend.totalWeight);
        blend.pullDelta = divide(weightedPull, blend.totalWeight);
        blend.valid = finite(blend.movementDelta) && finite(blend.pullDelta);
        return blend.valid ? blend : HandMotionBlend{};
    }

    float activityAdjustedHandWeight(
        const float baseWeight,
        const Vec3 pullDelta) noexcept
    {
        if (!std::isfinite(baseWeight) || baseWeight <= 0.0f ||
            !finite(pullDelta)) {
            return 0.0f;
        }
        const float normalized = std::clamp(
            length(pullDelta) / HAND_ACTIVITY_FULL_RESPONSE_DISTANCE,
            0.0f,
            1.0f);
        const float response =
            normalized * normalized * (3.0f - 2.0f * normalized);
        return std::clamp(baseWeight, 0.0f, 1.0f) *
               (HELD_HAND_MINIMUM_ACTIVITY_WEIGHT +
                   (1.0f - HELD_HAND_MINIMUM_ACTIVITY_WEIGHT) * response);
    }

    float adaptiveSmoothingSpeed(
        const float baseSpeed,
        const float targetLead) noexcept
    {
        if (!std::isfinite(baseSpeed) || baseSpeed <= 0.0f ||
            !std::isfinite(targetLead)) {
            return 0.0f;
        }
        const float normalized = std::clamp(
            targetLead / ADAPTIVE_SMOOTHING_FULL_RESPONSE_DISTANCE,
            0.0f,
            1.0f);
        const float response =
            normalized * normalized * (3.0f - 2.0f * normalized);
        const float multiplier =
            ADAPTIVE_SMOOTHING_QUIET_MULTIPLIER +
            (ADAPTIVE_SMOOTHING_ACTIVE_MULTIPLIER -
                ADAPTIVE_SMOOTHING_QUIET_MULTIPLIER) *
                response;
        return baseSpeed * multiplier;
    }

    float exponentialSmoothingFactor(
        const float speed,
        const float deltaSeconds) noexcept
    {
        if (!std::isfinite(speed) || speed <= 0.0f ||
            !std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f) {
            return 0.0f;
        }
        return std::clamp(
            1.0f - std::exp(-speed * deltaSeconds),
            0.0f,
            1.0f);
    }

    bool slideAlongCollisionPlane(
        const Vec3 remainingMotion,
        const Vec3 hitNormal,
        const Vec3 castDirection,
        Vec3& slidingMotion) noexcept
    {
        slidingMotion = {};
        if (!finite(remainingMotion)) {
            return false;
        }
        Vec3 normalized{};
        Vec3 direction{};
        if (!tryNormalize(hitNormal, normalized) ||
            !tryNormalize(castDirection, direction)) {
            return false;
        }
        if (dot(normalized, direction) > 0.0f) {
            normalized = scale(normalized, -1.0f);
        }
        const float intoSurface = dot(remainingMotion, normalized);
        slidingMotion = intoSurface < 0.0f ?
            subtract(remainingMotion, scale(normalized, intoSurface)) :
            remainingMotion;
        if (!finite(slidingMotion)) {
            slidingMotion = {};
            return false;
        }
        return true;
    }

    Vec3 calculateLaunchVelocity(
        const std::span<const VelocitySample> samples,
        const LaunchSettings& settings) noexcept
    {
        if (!settings.enabled || samples.empty() ||
            !std::isfinite(settings.historySeconds) ||
            settings.historySeconds <= 0.0f ||
            !std::isfinite(settings.multiplier) ||
            settings.multiplier < 0.0f ||
            !std::isfinite(settings.horizontalBoost) ||
            settings.horizontalBoost < 0.0f ||
            !std::isfinite(settings.minimumSpeed) ||
            settings.minimumSpeed < 0.0f ||
            !std::isfinite(settings.maximumSpeed) ||
            settings.maximumSpeed <= 0.0f) {
            return {};
        }

        float totalDuration = 0.0f;
        for (const auto& sample : samples) {
            if (finite(sample.displacement) &&
                std::isfinite(sample.deltaSeconds) &&
                sample.deltaSeconds > 0.0f) {
                totalDuration += sample.deltaSeconds;
            }
        }
        if (!std::isfinite(totalDuration) || totalDuration <= 0.0f) {
            return {};
        }

        Vec3 dominant{};
        float elapsed = 0.0f;
        for (const auto& sample : samples) {
            if (!finite(sample.displacement) ||
                !std::isfinite(sample.deltaSeconds) ||
                sample.deltaSeconds <= 0.0f) {
                continue;
            }
            const Vec3 velocity = divide(
                sample.displacement,
                sample.deltaSeconds);
            const float speed = length(velocity);
            elapsed += sample.deltaSeconds;
            if (speed <= 1.0e-3f) {
                continue;
            }
            const float age = std::max(0.0f, totalDuration - elapsed);
            const float recency = std::clamp(
                1.0f - 0.75f * age / settings.historySeconds,
                0.25f,
                1.0f);
            dominant = add(
                dominant,
                scale(divide(velocity, speed), speed * recency));
        }

        const float dominantMagnitude = length(dominant);
        if (dominantMagnitude <= 1.0e-3f) {
            return {};
        }
        const Vec3 dominantDirection = divide(dominant, dominantMagnitude);

        const bool filterEnabled =
            std::isfinite(settings.directionFilterDegrees) &&
            settings.directionFilterDegrees > 0.0f &&
            settings.directionFilterDegrees < 180.0f;
        const float cosineThreshold = filterEnabled ?
            std::cos(
                settings.directionFilterDegrees *
                std::numbers::pi_v<float> /
                180.0f) :
            -1.0f;

        Vec3 weightedVelocity{};
        float totalWeight = 0.0f;
        elapsed = 0.0f;
        for (const auto& sample : samples) {
            if (!finite(sample.displacement) ||
                !std::isfinite(sample.deltaSeconds) ||
                sample.deltaSeconds <= 0.0f) {
                continue;
            }
            const Vec3 velocity = divide(
                sample.displacement,
                sample.deltaSeconds);
            const float speed = length(velocity);
            elapsed += sample.deltaSeconds;
            if (speed <= 1.0e-3f) {
                continue;
            }
            const Vec3 direction = divide(velocity, speed);
            const float directionDot =
                direction.x * dominantDirection.x +
                direction.y * dominantDirection.y +
                direction.z * dominantDirection.z;
            if (filterEnabled && directionDot < cosineThreshold) {
                continue;
            }
            const float age = std::max(0.0f, totalDuration - elapsed);
            const float recency = std::clamp(
                1.0f - 0.75f * age / settings.historySeconds,
                0.25f,
                1.0f);
            const float weight = speed * recency;
            weightedVelocity = add(
                weightedVelocity,
                scale(velocity, weight));
            totalWeight += weight;
        }

        if (!std::isfinite(totalWeight) || totalWeight <= 1.0e-6f) {
            return {};
        }

        Vec3 velocity = divide(weightedVelocity, totalWeight);
        velocity.x *= settings.multiplier * settings.horizontalBoost;
        velocity.y *= settings.multiplier * settings.horizontalBoost;
        velocity.z *= settings.multiplier;
        velocity = clampMagnitude(velocity, settings.maximumSpeed);
        return length(velocity) >= settings.minimumSpeed ? velocity : Vec3{};
    }

    FlightStep integrateFlight(
        const Vec3 velocity,
        const float gravity,
        const float deltaSeconds) noexcept
    {
        if (!finite(velocity) || !std::isfinite(gravity) || gravity < 0.0f ||
            !std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f) {
            return {};
        }
        FlightStep step{};
        step.velocity = velocity;
        step.velocity.z -= gravity * deltaSeconds;
        step.displacement = scale(step.velocity, deltaSeconds);
        return step;
    }

    LandingDecision decideLanding(
        const bool groundFound,
        const float groundHeight,
        const float feetHeight,
        const float verticalVelocity,
        const float descentThisFrame) noexcept
    {
        if (!groundFound || !std::isfinite(groundHeight) ||
            !std::isfinite(feetHeight) || !std::isfinite(verticalVelocity) ||
            !std::isfinite(descentThisFrame)) {
            return LandingDecision::Continue;
        }
        const float penetration = groundHeight - feetHeight;
        if (penetration < 0.0f) {
            return LandingDecision::Continue;
        }
        if (verticalVelocity > 0.0f &&
            penetration <= FLIGHT_MAXIMUM_RISING_PENETRATION) {
            return LandingDecision::Continue;
        }
        return penetration >=
                MANTLE_MINIMUM_RISE + std::max(0.0f, descentThisFrame) ?
            LandingDecision::Mantle :
            LandingDecision::Land;
    }

    MantlePath makeMantlePath(
        const Vec3 start,
        const Vec3 target) noexcept
    {
        MantlePath path{};
        if (!finite(start) || !finite(target)) {
            return path;
        }
        path.start = start;
        path.target = target;
        path.durationSeconds = std::max(
            MANTLE_MINIMUM_SECONDS,
            length(subtract(target, start)) * MANTLE_SECONDS_PER_GAME_UNIT);
        return path;
    }

    Vec3 evaluateMantlePath(
        const MantlePath& path,
        const float progress) noexcept
    {
        const float bounded = std::isfinite(progress) ?
            std::clamp(progress, 0.0f, 1.0f) :
            1.0f;
        const float eased = 1.0f - (1.0f - bounded) * (1.0f - bounded);
        return add(
            path.start,
            scale(subtract(path.target, path.start), eased));
    }

    namespace
    {
        [[nodiscard]] float distanceToSegment(
            const Vec3 point,
            const Vec3 start,
            const Vec3 end) noexcept
        {
            const Vec3 segment = subtract(end, start);
            const float segmentLengthSquared = lengthSquared(segment);
            const float along = segmentLengthSquared > 1.0e-8f ?
                std::clamp(
                    dot(subtract(point, start), segment) / segmentLengthSquared,
                    0.0f,
                    1.0f) :
                0.0f;
            return length(subtract(point, add(start, scale(segment, along))));
        }

        // Every vertex of other on or behind plane's surface, at least one
        // clearly behind it.
        [[nodiscard]] bool liesBehind(
            const SurfaceTriangle& plane,
            const Vec3 planeNormal,
            const SurfaceTriangle& other) noexcept
        {
            bool clearlyBehind = false;
            for (const Vec3 vertex : { other.a, other.b, other.c }) {
                const float height = dot(planeNormal, subtract(vertex, plane.a));
                if (height > EDGE_PLANE_TOLERANCE) {
                    return false;
                }
                clearlyBehind = clearlyBehind || height < -EDGE_PLANE_TOLERANCE;
            }
            return clearlyBehind;
        }
    }

    bool hasGrabbableEdge(
        const std::span<const SurfaceTriangle> triangles,
        const Vec3 point,
        const float reach) noexcept
    {
        if (!finite(point) || !std::isfinite(reach) || reach <= 0.0f) {
            return false;
        }
        const float maximumNormalDot = std::cos(
            EDGE_MINIMUM_FOLD_DEGREES * std::numbers::pi_v<float> / 180.0f);
        constexpr float sharedToleranceSquared =
            EDGE_SHARED_VERTEX_TOLERANCE * EDGE_SHARED_VERTEX_TOLERANCE;

        for (std::size_t first = 0; first < triangles.size(); ++first) {
            const auto& left = triangles[first];
            Vec3 leftNormal{};
            if (!tryNormalize(left.normal, leftNormal)) {
                continue;
            }
            for (std::size_t second = first + 1; second < triangles.size(); ++second) {
                const auto& right = triangles[second];
                Vec3 rightNormal{};
                if (!tryNormalize(right.normal, rightNormal) ||
                    dot(leftNormal, rightNormal) > maximumNormalDot) {
                    continue;
                }

                std::array<Vec3, 3> shared{};
                std::size_t sharedCount = 0;
                for (const Vec3 leftVertex : { left.a, left.b, left.c }) {
                    for (const Vec3 rightVertex : { right.a, right.b, right.c }) {
                        if (lengthSquared(subtract(leftVertex, rightVertex)) <=
                            sharedToleranceSquared) {
                            shared[sharedCount++] = leftVertex;
                            break;
                        }
                    }
                }
                if (sharedCount == 0 ||
                    !liesBehind(left, leftNormal, right) ||
                    !liesBehind(right, rightNormal, left)) {
                    continue;
                }

                const float distance = sharedCount == 1 ?
                    length(subtract(point, shared[0])) :
                    distanceToSegment(point, shared[0], shared[1]);
                if (distance <= reach) {
                    return true;
                }
            }
        }
        return false;
    }
}
