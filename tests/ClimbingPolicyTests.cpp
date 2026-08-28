#include "runtime/ClimbingPolicy.h"
#include "runtime/ControllerPolicy.h"
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
    namespace controller_policy = rock_wall_climbing::controller_policy;
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

    void dualHandStateDecodesBothIndependentBits()
    {
        constexpr std::uint32_t rightMask = 1u << 0;
        constexpr std::uint32_t leftMask = 1u << 1;

        const auto both = decodeActiveHands(
            rightMask | leftMask,
            rightMask,
            leftMask);
        assert(both.right);
        assert(both.left);

        const auto rightOnly = decodeActiveHands(
            rightMask,
            rightMask,
            leftMask);
        assert(rightOnly.right);
        assert(!rightOnly.left);

        const auto leftOnly = decodeActiveHands(
            leftMask,
            rightMask,
            leftMask);
        assert(!leftOnly.right);
        assert(leftOnly.left);
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

    void joinedHandConfidenceRampsByElapsedTime()
    {
        float fourStepWeight = 0.0f;
        for (int index = 0; index < 4; ++index) {
            fourStepWeight = advanceHandBlendWeight(
                fourStepWeight,
                0.02f,
                HAND_JOIN_BLEND_SECONDS);
        }

        float eightStepWeight = 0.0f;
        for (int index = 0; index < 8; ++index) {
            eightStepWeight = advanceHandBlendWeight(
                eightStepWeight,
                0.01f,
                HAND_JOIN_BLEND_SECONDS);
        }

        assert(close(fourStepWeight, 1.0f));
        assert(close(eightStepWeight, 1.0f));
        assert(close(
            advanceHandBlendWeight(0.0f, 0.02f, HAND_JOIN_BLEND_SECONDS),
            0.25f));
    }

    void handMotionBlendPreservesSingleHandAndFadesInTheSecond()
    {
        const std::array<HandMotionContribution, 2> joined{
            HandMotionContribution{
                true, 1.0f, { 8.0f, 0.0f, 0.0f },
                { 4.0f, 0.0f, 0.0f } },
            HandMotionContribution{
                true, 0.25f, { -8.0f, 0.0f, 0.0f },
                { -4.0f, 0.0f, 0.0f } },
        };
        const auto partialBlend = blendHandMotions(joined);
        assert(partialBlend.valid);
        assert(partialBlend.contributionCount == 2);
        assert(close(partialBlend.movementDelta.x, 4.8f));
        assert(close(partialBlend.pullDelta.x, 2.4f));

        const std::array<HandMotionContribution, 2> isolated{
            HandMotionContribution{
                true, 0.2f, { 7.0f, 2.0f, -1.0f },
                { 3.0f, 1.0f, 0.0f } },
            HandMotionContribution{
                false, 1.0f, { -50.0f, 0.0f, 0.0f },
                { -50.0f, 0.0f, 0.0f } },
        };
        const auto isolatedBlend = blendHandMotions(isolated);
        assert(isolatedBlend.valid);
        assert(isolatedBlend.contributionCount == 1);
        assert(close(isolatedBlend.movementDelta.x, 7.0f));
        assert(close(isolatedBlend.movementDelta.y, 2.0f));
        assert(close(isolatedBlend.pullDelta.x, 3.0f));
    }

    void adaptiveSmoothingIsBoundedAndMonotonic()
    {
        const float quiet = adaptiveSmoothingSpeed(13.0f, 0.0f);
        const float middle = adaptiveSmoothingSpeed(13.0f, 6.0f);
        const float active = adaptiveSmoothingSpeed(13.0f, 12.0f);
        const float beyond = adaptiveSmoothingSpeed(13.0f, 40.0f);

        assert(close(quiet, 9.75f));
        assert(quiet < middle);
        assert(middle < active);
        assert(close(active, 18.85f));
        assert(close(beyond, active));
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

    void zeroHandoffSamplesAgeOutOldLaunchMomentum()
    {
        VelocityHistory history;
        history.push(
            VelocitySample{ { 12.0f, 0.0f, 0.0f }, 0.04f },
            0.18f);
        for (int index = 0; index < 5; ++index) {
            history.push(
                VelocitySample{ {}, 0.04f },
                0.18f);
        }

        std::array<VelocitySample, VelocityHistory::CAPACITY> copied{};
        const auto samples = history.copyChronological(copied);
        LaunchSettings settings{};
        settings.minimumSpeed = 1.0f;
        assert(close(length(calculateLaunchVelocity(samples, settings)), 0.0f));
    }

    void controllerVelocityDispatchRejectsTheTransformSlot()
    {
        constexpr std::uintptr_t moduleBase = 0x140000000;
        constexpr auto& proxy =
            controller_policy::VELOCITY_DISPATCH_SPECS[0];
        constexpr auto& rigid =
            controller_policy::VELOCITY_DISPATCH_SPECS[1];

        static_assert(controller_policy::VELOCITY_SLOT_OFFSET == 0x1E0);
        assert(controller_policy::selectVelocityDispatch(
                   moduleBase,
                   moduleBase + proxy.vtableRva,
                   moduleBase + proxy.functionRva) == &proxy);
        assert(controller_policy::selectVelocityDispatch(
                   moduleBase,
                   moduleBase + rigid.vtableRva,
                   moduleBase + rigid.functionRva) == &rigid);
        assert(controller_policy::selectVelocityDispatch(
                   moduleBase,
                   moduleBase + proxy.vtableRva,
                   moduleBase + proxy.functionRva - 0x110) == nullptr);
        assert(controller_policy::selectVelocityDispatch(
                   moduleBase,
                   moduleBase + proxy.vtableRva + 0x10,
                   moduleBase + proxy.functionRva) == nullptr);
    }

    void gravityRestoreDefersWithoutDiscardingOwnership()
    {
        using controller_policy::GravityRestoreDecision;
        using controller_policy::decideGravityRestore;

        constexpr std::uintptr_t savedIdentity = 0x12340000;
        assert(decideGravityRestore(false, savedIdentity, 0) ==
               GravityRestoreDecision::Defer);
        assert(decideGravityRestore(true, savedIdentity, savedIdentity) ==
               GravityRestoreDecision::Restore);
        assert(decideGravityRestore(true, savedIdentity, 0x56780000) ==
               GravityRestoreDecision::Retire);
    }
}

int main()
{
    inverseHandMotionAndSurfaceCarry();
    dualHandStateDecodesBothIndependentBits();
    discontinuitiesFailClosed();
    smoothingIsFrameRateCoherent();
    joinedHandConfidenceRampsByElapsedTime();
    handMotionBlendPreservesSingleHandAndFadesInTheSecond();
    adaptiveSmoothingIsBoundedAndMonotonic();
    launchRejectsOpposedNoiseAndCapsSpeed();
    slowReleaseDoesNotLaunch();
    velocityHistoryIsBoundedAndChronological();
    zeroHandoffSamplesAgeOutOldLaunchMomentum();
    controllerVelocityDispatchRejectsTheTransformSlot();
    gravityRestoreDefersWithoutDiscardingOwnership();
    return 0;
}
