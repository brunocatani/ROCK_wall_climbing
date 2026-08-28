#include "runtime/ClimbingPolicy.h"
#include "runtime/VelocityHistory.h"

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <array>
#include <cassert>
#include <cmath>

namespace
{
    using rock_wall_climbing::VelocityHistory;
    using namespace rock_wall_climbing::policy;

    [[nodiscard]] bool close(
        const float left,
        const float right,
        const float epsilon = 1.0e-3f)
    {
        return std::abs(left - right) <= epsilon;
    }

    void inverseHandMotionAndSurfaceCarry()
    {
        HandMotionInput input{};
        input.previousHandOffset = { 10.0f, 0.0f, 20.0f };
        input.currentHandOffset = { 8.0f, 1.0f, 17.0f };
        input.previousAnchorValid = true;
        input.currentAnchorValid = true;
        input.previousAnchor = { 100.0f, 50.0f, 30.0f };
        input.currentAnchor = { 101.0f, 52.0f, 30.0f };
        input.maximumDelta = 10.0f;

        const auto result = evaluateHandMotion(input);
        assert(result.valid);
        assert(!result.discontinuity);
        assert(close(result.pullDelta.x, 2.0f));
        assert(close(result.pullDelta.y, -1.0f));
        assert(close(result.pullDelta.z, 3.0f));
        assert(close(result.surfaceCarryDelta.x, 1.0f));
        assert(close(result.surfaceCarryDelta.y, 2.0f));
        assert(close(result.totalDelta.x, 3.0f));
        assert(close(result.totalDelta.y, 1.0f));
        assert(close(result.totalDelta.z, 3.0f));
    }

    void discontinuitiesFailClosed()
    {
        HandMotionInput input{};
        input.previousHandOffset = {};
        input.currentHandOffset = { 100.0f, 0.0f, 0.0f };
        input.maximumDelta = 20.0f;
        const auto result = evaluateHandMotion(input);
        assert(!result.valid);
        assert(result.discontinuity);
        assert(close(length(result.totalDelta), 0.0f));
    }

    void smoothingIsFrameRateCoherent()
    {
        const float oneStep = exponentialSmoothingFactor(18.0f, 1.0f / 45.0f);
        const float halfStep = exponentialSmoothingFactor(18.0f, 1.0f / 90.0f);
        const float composed = 1.0f - (1.0f - halfStep) * (1.0f - halfStep);
        assert(close(oneStep, composed, 1.0e-5f));
    }

    void launchRejectsOpposedNoiseAndCapsSpeed()
    {
        std::array<VelocitySample, 4> samples{
            VelocitySample{ { 0.0f, 3.0f, 1.0f }, 0.01f },
            VelocitySample{ { 0.0f, 3.5f, 1.5f }, 0.01f },
            VelocitySample{ { 0.0f, -1.0f, 0.0f }, 0.01f },
            VelocitySample{ { 0.0f, 4.0f, 2.0f }, 0.01f },
        };
        LaunchSettings settings{};
        settings.directionFilterDegrees = 60.0f;
        settings.multiplier = 2.0f;
        settings.horizontalBoost = 2.0f;
        settings.minimumSpeed = 1.0f;
        settings.maximumSpeed = 300.0f;

        const Vec3 launch = calculateLaunchVelocity(samples, settings);
        assert(launch.y > 0.0f);
        assert(launch.z > 0.0f);
        assert(length(launch) <= 300.001f);
    }

    void slowReleaseDoesNotLaunch()
    {
        const std::array<VelocitySample, 2> samples{
            VelocitySample{ { 0.0f, 0.1f, 0.0f }, 0.1f },
            VelocitySample{ { 0.0f, 0.1f, 0.0f }, 0.1f },
        };
        LaunchSettings settings{};
        settings.minimumSpeed = 10.0f;
        assert(close(length(calculateLaunchVelocity(samples, settings)), 0.0f));
    }

    void velocityHistoryIsBoundedAndChronological()
    {
        VelocityHistory history;
        for (int index = 0; index < 40; ++index) {
            history.push(
                VelocitySample{
                    { static_cast<float>(index), 0.0f, 0.0f },
                    0.01f,
                },
                0.12f);
        }
        assert(history.size() <= VelocityHistory::CAPACITY);
        assert(history.size() <= 12);

        std::array<VelocitySample, VelocityHistory::CAPACITY> copied{};
        const auto samples = history.copyChronological(copied);
        assert(!samples.empty());
        for (std::size_t index = 1; index < samples.size(); ++index) {
            assert(samples[index - 1].displacement.x <
                   samples[index].displacement.x);
        }
    }
}

int main()
{
    inverseHandMotionAndSurfaceCarry();
    discontinuitiesFailClosed();
    smoothingIsFrameRateCoherent();
    launchRejectsOpposedNoiseAndCapsSpeed();
    slowReleaseDoesNotLaunch();
    velocityHistoryIsBoundedAndChronological();
    return 0;
}
