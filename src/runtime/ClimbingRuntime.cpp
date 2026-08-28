#include "runtime/ClimbingRuntime.h"

#include "api/RockApiClient.h"
#include "support/Logger.h"

#include "RE/Bethesda/PlayerCharacter.h"
#include "RE/Bethesda/bhkCharacterController.h"
#include "RE/Havok/hkVector4.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace rock_wall_climbing
{
    namespace
    {
        using namespace rock::provider;

        constexpr std::uint64_t CLIMBING_SCOPE_TOKEN =
            0x434C'494D'425F'5631ull;
        constexpr std::uint64_t RIGHT_TARGET_ID =
            0x434C'494D'425F'5201ull;
        constexpr std::uint64_t LEFT_TARGET_ID =
            0x434C'494D'425F'4C02ull;
        constexpr std::uint32_t TARGET_LEASE_FRAMES = 4;
        constexpr std::uint32_t INVALID_BODY_ID = 0x7FFF'FFFFu;
        constexpr float MAXIMUM_FRAME_DELTA_SECONDS = 0.1f;

        static_assert(
            offsetof(RE::bhkCharacterController, gravity) == 0x330,
            "CommonLibF4VR bhkCharacterController gravity layout changed");
        static_assert(controller_policy::VELOCITY_SLOT_OFFSET == 0x1E0);

        constexpr std::size_t REQUIRED_SNAPSHOT_BYTES =
            offsetof(RockProviderFrameSnapshot,
                equippedWeaponTransitionSequence) +
            sizeof(std::uint64_t);

        enum SnapshotBlocker : std::uint32_t
        {
            SessionInactive = 1u << 0,
            AddonDisabled = 1u << 1,
            InvalidSnapshot = 1u << 2,
            ProviderNotReady = 1u << 3,
            SkeletonNotReady = 1u << 4,
            MenuOrConfigBlocked = 1u << 5,
            WorldTransition = 1u << 6,
            PhysicsOrVisualWritesBlocked = 1u << 7,
            InvalidTiming = 1u << 8,
            InvalidScale = 1u << 9,
            InvalidTransforms = 1u << 10,
        };

        [[nodiscard]] constexpr std::uint32_t flag(
            const RockProviderTouchGrabTargetFlagV1 value) noexcept
        {
            return static_cast<std::uint32_t>(value);
        }

        [[nodiscard]] constexpr bool hasHandInteractionFlag(
            const std::uint32_t flags,
            const RockProviderHandInteractionFlagV1 value) noexcept
        {
            return (flags & static_cast<std::uint32_t>(value)) != 0;
        }

        [[nodiscard]] constexpr bool hasEnrichmentFlag(
            const std::uint32_t flags,
            const RockProviderFrameEnrichmentFlagV1 value) noexcept
        {
            return (flags & static_cast<std::uint32_t>(value)) != 0;
        }

        [[nodiscard]] policy::Vec3 point(
            const RockProviderPoint3 value) noexcept
        {
            return { value.x, value.y, value.z };
        }

        [[nodiscard]] policy::Vec3 translation(
            const RockProviderTransform& value) noexcept
        {
            return {
                value.translate[0],
                value.translate[1],
                value.translate[2],
            };
        }

        [[nodiscard]] RockProviderPoint3 providerPoint(
            const policy::Vec3 value) noexcept
        {
            return { value.x, value.y, value.z };
        }

        [[nodiscard]] bool validCoordinate(const policy::Vec3 value) noexcept
        {
            constexpr float MAXIMUM_ABSOLUTE_GAME_COORDINATE = 1.0e7f;
            return policy::finite(value) &&
                   std::abs(value.x) < MAXIMUM_ABSOLUTE_GAME_COORDINATE &&
                   std::abs(value.y) < MAXIMUM_ABSOLUTE_GAME_COORDINATE &&
                   std::abs(value.z) < MAXIMUM_ABSOLUTE_GAME_COORDINATE;
        }

        struct VelocityDispatchResolution
        {
            bool matched{ false };
            std::uintptr_t vtableAddress{ 0 };
            std::uintptr_t functionAddress{ 0 };
            const controller_policy::VelocityDispatchSpec* spec{ nullptr };

            [[nodiscard]] bool valid() const noexcept
            {
                return matched && spec &&
                       vtableAddress != 0 && functionAddress != 0;
            }
        };

        [[nodiscard]] VelocityDispatchResolution resolveVelocityDispatch(
            RE::bhkCharacterController* controller) noexcept
        {
            VelocityDispatchResolution resolution{};
            if (!controller) {
                return resolution;
            }
            if (!REL::Module::IsVR() ||
                REL::Module::get().version() !=
                    F4SE::RUNTIME_VR_1_2_72) {
                return resolution;
            }

            const std::uintptr_t moduleBase = REL::Module::get().base();
            __try {
                resolution.vtableAddress =
                    reinterpret_cast<std::uintptr_t>(
                        *reinterpret_cast<void* const*>(controller));
                if (resolution.vtableAddress == 0) {
                    return resolution;
                }

                const auto* vtable =
                    reinterpret_cast<const std::uintptr_t*>(
                        resolution.vtableAddress);
                resolution.functionAddress =
                    vtable[controller_policy::VELOCITY_SLOT_INDEX];
                resolution.spec =
                    controller_policy::selectVelocityDispatch(
                        moduleBase,
                        resolution.vtableAddress,
                        resolution.functionAddress);
                if (!resolution.spec) {
                    return resolution;
                }

                const auto* functionBytes =
                    reinterpret_cast<const std::uint8_t*>(
                        resolution.functionAddress);
                for (std::size_t index = 0;
                     index < resolution.spec->functionPrefix.size();
                     ++index) {
                    if (functionBytes[index] !=
                        resolution.spec->functionPrefix[index]) {
                        return resolution;
                    }
                }
                resolution.matched = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                resolution.matched = false;
            }
            return resolution;
        }
    }

    ClimbingRuntime& ClimbingRuntime::get() noexcept
    {
        static ClimbingRuntime runtime;
        return runtime;
    }

    bool ClimbingRuntime::connect() noexcept
    {
        if (_connected && rockApiClient().ready()) {
            return true;
        }
        if (!rockApiClient().initialize()) {
            _connected = false;
            return false;
        }
        if (!rockApiClient().registerFrameCallback(
                &ClimbingRuntime::onRockFrame,
                this)) {
            logger::error(
                "Could not register the ROCK owner frame callback.");
            rockApiClient().shutdown();
            _connected = false;
            return false;
        }
        _connected = true;
        logger::info(
            "Connected to ROCK V1 fixed-surface latch and world-raycast services.");
        return true;
    }

    void ClimbingRuntime::beginSession() noexcept
    {
        try {
            if (_sessionActive) {
                finishClimb(nullptr, 0.0f, false, "session-refresh");
                clearTargets();
            }
            static_cast<void>(_config.reload());
            _sessionActive = true;
            _lastBlockers = UINT32_MAX;
            static_cast<void>(connect());
        } catch (const std::exception& error) {
            logger::error(
                "Session initialization failed: {}.",
                error.what());
            _sessionActive = false;
        } catch (...) {
            logger::error("Session initialization failed.");
            _sessionActive = false;
        }
    }

    void ClimbingRuntime::endSession() noexcept
    {
        _sessionActive = false;
        finishClimb(nullptr, 0.0f, false, "session-ended");
        clearTargets();
        _worldGeneration = 0;
        _skeletonGeneration = 0;
        _providerGeneration = 0;
        _lastFrameIndex = 0;
        _lastBlockers = UINT32_MAX;
    }

    void ClimbingRuntime::shutdown() noexcept
    {
        endSession();
        rockApiClient().shutdown();
        _connected = false;
    }

    void ROCK_PROVIDER_CALL ClimbingRuntime::onRockFrame(
        const RockProviderFrameSnapshot* snapshot,
        void* userData) noexcept
    {
        auto* runtime = static_cast<ClimbingRuntime*>(userData);
        if (!runtime || !snapshot) {
            return;
        }
        try {
            runtime->update(*snapshot);
        } catch (const std::exception& error) {
            logger::error(
                "Climbing frame failed closed after exception: {}.",
                error.what());
            runtime->finishClimb(
                nullptr,
                0.0f,
                false,
                "callback-exception");
            runtime->clearTargets();
        } catch (...) {
            logger::error(
                "Climbing frame failed closed after an unknown exception.");
            runtime->finishClimb(
                nullptr,
                0.0f,
                false,
                "callback-exception");
            runtime->clearTargets();
        }
    }

    std::uint32_t ClimbingRuntime::snapshotBlockers(
        const RockProviderFrameSnapshot& snapshot) const noexcept
    {
        std::uint32_t blockers = 0;
        if (!_sessionActive) {
            blockers |= SessionInactive;
        }
        if (!_config.enabled) {
            blockers |= AddonDisabled;
        }
        if (snapshot.size < REQUIRED_SNAPSHOT_BYTES ||
            snapshot.version < ROCK_PROVIDER_API_VERSION) {
            blockers |= InvalidSnapshot;
            return blockers;
        }
        if (snapshot.providerReady == 0 ||
            !hasLifecycleFlag(
                snapshot.lifecycleFlags,
                RockProviderLifecycleFlag::ProviderReady)) {
            blockers |= ProviderNotReady;
        }
        if (snapshot.frikSkeletonReady == 0 ||
            !hasLifecycleFlag(
                snapshot.lifecycleFlags,
                RockProviderLifecycleFlag::SkeletonReady)) {
            blockers |= SkeletonNotReady;
        }
        if (snapshot.menuBlocking != 0 || snapshot.configBlocking != 0 ||
            hasLifecycleFlag(
                snapshot.lifecycleFlags,
                RockProviderLifecycleFlag::MenuBlocking) ||
            hasLifecycleFlag(
                snapshot.lifecycleFlags,
                RockProviderLifecycleFlag::ConfigBlocking)) {
            blockers |= MenuOrConfigBlocked;
        }
        if (hasLifecycleFlag(
                snapshot.lifecycleFlags,
                RockProviderLifecycleFlag::LoadingOrWorldTransition) ||
            !hasLifecycleFlag(
                snapshot.lifecycleFlags,
                RockProviderLifecycleFlag::WorldAvailable) ||
            snapshot.worldGeneration == 0 ||
            snapshot.skeletonGeneration == 0 ||
            snapshot.providerGeneration == 0) {
            blockers |= WorldTransition;
        }
        if (!hasLifecycleFlag(
                snapshot.lifecycleFlags,
                RockProviderLifecycleFlag::GeneratedBodiesValid) ||
            !hasLifecycleFlag(
                snapshot.lifecycleFlags,
                RockProviderLifecycleFlag::PhysicsWriteAllowed) ||
            !hasLifecycleFlag(
                snapshot.lifecycleFlags,
                RockProviderLifecycleFlag::VisualWriteAllowed)) {
            blockers |= PhysicsOrVisualWritesBlocked;
        }
        if (!hasEnrichmentFlag(
                snapshot.enrichmentFlags,
                RockProviderFrameEnrichmentFlagV1::DeltaSecondsValid) ||
            !std::isfinite(snapshot.deltaSeconds) ||
            snapshot.deltaSeconds <= 0.0f ||
            snapshot.deltaSeconds > MAXIMUM_FRAME_DELTA_SECONDS ||
            (_lastFrameIndex != 0 &&
                snapshot.frameIndex <= _lastFrameIndex &&
                snapshot.worldGeneration == _worldGeneration &&
                snapshot.skeletonGeneration == _skeletonGeneration &&
                snapshot.providerGeneration == _providerGeneration)) {
            blockers |= InvalidTiming;
        }
        if (!std::isfinite(snapshot.gameToHavokScale) ||
            snapshot.gameToHavokScale <= 0.0f ||
            snapshot.gameToHavokScale >= 1.0f ||
            !std::isfinite(snapshot.havokToGameScale) ||
            snapshot.havokToGameScale <= 1.0f) {
            blockers |= InvalidScale;
        }
        if (!hasEnrichmentFlag(
                snapshot.enrichmentFlags,
                RockProviderFrameEnrichmentFlagV1::HmdTransformValid) ||
            !validCoordinate(translation(snapshot.hmdTransform)) ||
            !validCoordinate(translation(snapshot.rightHandTransform)) ||
            !validCoordinate(translation(snapshot.leftHandTransform))) {
            blockers |= InvalidTransforms;
        }
        return blockers;
    }

    void ClimbingRuntime::observeBlockers(
        const std::uint32_t blockers) noexcept
    {
        if (blockers == _lastBlockers) {
            return;
        }
        logger::info(
            "Climbing gate {} blockers=0x{:03X} (session={} addon={} provider={} skeleton={} menu={} transition={} writes={} timing={} scale={} transforms={}).",
            blockers == 0 ? "open" : "closed",
            blockers,
            (blockers & SessionInactive) == 0 ? "active" : "inactive",
            (blockers & AddonDisabled) == 0 ? "enabled" : "disabled",
            (blockers & ProviderNotReady) == 0 ? "ready" : "blocked",
            (blockers & SkeletonNotReady) == 0 ? "ready" : "blocked",
            (blockers & MenuOrConfigBlocked) == 0 ? "clear" : "blocked",
            (blockers & WorldTransition) == 0 ? "stable" : "changing",
            (blockers & PhysicsOrVisualWritesBlocked) == 0 ? "allowed" : "blocked",
            (blockers & InvalidTiming) == 0 ? "valid" : "invalid",
            (blockers & InvalidScale) == 0 ? "valid" : "invalid",
            (blockers & InvalidTransforms) == 0 ? "valid" : "invalid");
        _lastBlockers = blockers;
    }

    void ClimbingRuntime::update(
        const RockProviderFrameSnapshot& snapshot)
    {
        const std::uint32_t blockers = snapshotBlockers(snapshot);
        observeBlockers(blockers);
        if (blockers != 0) {
            finishClimb(nullptr, 0.0f, false, "operational-gate");
            clearTargets();
            return;
        }

        _lastFrameIndex = snapshot.frameIndex;
        const bool generationChanged =
            snapshot.worldGeneration != _worldGeneration ||
            snapshot.skeletonGeneration != _skeletonGeneration ||
            snapshot.providerGeneration != _providerGeneration;
        if (generationChanged) {
            finishClimb(nullptr, 0.0f, false, "provider-generation");
            clearTargets();
            _worldGeneration = snapshot.worldGeneration;
            _skeletonGeneration = snapshot.skeletonGeneration;
            _providerGeneration = snapshot.providerGeneration;
            static_cast<void>(nextTargetGeneration());
            logger::info(
                "Climbing generations changed to world={} skeleton={} provider={} targetGeneration={}.",
                _worldGeneration,
                _skeletonGeneration,
                _providerGeneration,
                _targetGeneration);
        }

        if (!publishTargets(snapshot)) {
            finishClimb(nullptr, 0.0f, false, "target-publication");
            clearTargets();
            return;
        }

        std::array<ObservedHand, 2> observed{};
        bool invalidated = false;
        if (!observeHands(observed, invalidated)) {
            finishClimb(nullptr, 0.0f, false, "state-readback");
            clearTargets();
            return;
        }
        if (invalidated) {
            finishClimb(nullptr, 0.0f, false, "target-invalidated");
            clearTargets();
            static_cast<void>(nextTargetGeneration());
            logger::warn(
                "A climbing target was invalidated; rearming with targetGeneration={} next frame.",
                _targetGeneration);
            return;
        }

        PlayerAccess access{};
        ControllerResolveStage deepestStage = ControllerResolveStage::None;
        if (!tryResolvePlayerAccess(access, deepestStage)) {
            if (deepestStage != _lastControllerFailure) {
                logger::warn(
                    "Player controller unavailable; deepest verified stage={}. Climbing failed closed.",
                    controllerStageName(deepestStage));
                _lastControllerFailure = deepestStage;
            }
            finishClimb(nullptr, 0.0f, false, "controller-unavailable");
            clearTargets();
            return;
        }
        if (_lastControllerFailure != ControllerResolveStage::Complete) {
            logger::info("Player controller chain recovered.");
            _lastControllerFailure = ControllerResolveStage::Complete;
        }

        const std::array<policy::Vec3, 2> currentHandOffsets{
            policy::subtract(
                translation(snapshot.rightHandTransform),
                access.playerPosition),
            policy::subtract(
                translation(snapshot.leftHandTransform),
                access.playerPosition),
        };
        if (!validCoordinate(currentHandOffsets[0]) ||
            !validCoordinate(currentHandOffsets[1])) {
            finishClimb(&access, 0.0f, false, "hand-offset-invalid");
            clearTargets();
            return;
        }

        const std::uint32_t currentHeldCount =
            static_cast<std::uint32_t>(observed[0].held) +
            static_cast<std::uint32_t>(observed[1].held);
        if (!_climbing && currentHeldCount > 0) {
            _climbing = true;
            _targetPlayerPosition = access.playerPosition;
            _targetPositionValid = true;
            _velocityHistory.clear();
            logger::info(
                "Climb started hands={} player=({:.2f},{:.2f},{:.2f}).",
                currentHeldCount,
                access.playerPosition.x,
                access.playerPosition.y,
                access.playerPosition.z);
        }

        bool ownershipTransition = false;
        for (std::size_t index = 0; index < _hands.size(); ++index) {
            auto& hand = _hands[index];
            const auto& current = observed[index];
            const bool wasHeld = hand.held;
            const bool sameBody =
                wasHeld && current.held && hand.bodyId == current.bodyId;
            const bool continuingOwnership =
                sameBody && hand.baselineValid;
            ownershipTransition = ownershipTransition ||
                wasHeld != current.held ||
                (wasHeld && current.held && !sameBody) ||
                (current.held && !hand.baselineValid);

            if (current.held) {
                if (!wasHeld) {
                    logger::info(
                        "{} hand latched targetGeneration={} anchorMode={}.",
                        index == 0 ? "Right" : "Left",
                        _targetGeneration,
                        current.anchorValid ? "contact" : "unavailable");
                } else if (!sameBody) {
                    logger::info(
                        "{} hand changed climbing surface body from {} to {}.",
                        index == 0 ? "Right" : "Left",
                        hand.bodyId,
                        current.bodyId);
                }
                hand.held = true;
                hand.bodyId = current.bodyId;
                hand.blendWeight = continuingOwnership ?
                    hand.blendWeight :
                    0.0f;
            } else {
                if (wasHeld) {
                    logger::info(
                        "{} hand released climbing surface.",
                        index == 0 ? "Right" : "Left");
                }
                hand = {};
            }
        }

        if (currentHeldCount == 0) {
            finishClimb(
                &access,
                snapshot.gameToHavokScale,
                true,
                "final-release");
            return;
        }

        policy::HandMotionBlend motionBlend{};
        if (ownershipTransition) {
            for (std::size_t index = 0; index < _hands.size(); ++index) {
                auto& hand = _hands[index];
                const auto& current = observed[index];
                if (!current.held) {
                    continue;
                }
                hand.baselineValid = true;
                hand.previousHandOffset = currentHandOffsets[index];
                hand.previousAnchorValid = current.anchorValid;
                hand.previousAnchor = current.anchor;
            }
            _velocityHistory.push(
                policy::VelocitySample{ {}, snapshot.deltaSeconds },
                _config.launch.historySeconds);
        } else {
            std::array<policy::HandMotionContribution, 2> contributions{};
            std::uint32_t discontinuityCount = 0;
            for (std::size_t index = 0; index < _hands.size(); ++index) {
                auto& hand = _hands[index];
                const auto& current = observed[index];
                if (!current.held || !hand.held || !hand.baselineValid ||
                    hand.bodyId != current.bodyId) {
                    continue;
                }

                hand.blendWeight = policy::advanceHandBlendWeight(
                    hand.blendWeight,
                    snapshot.deltaSeconds,
                    policy::HAND_JOIN_BLEND_SECONDS);
                policy::HandMotionInput input{};
                input.previousHandOffset = hand.previousHandOffset;
                input.currentHandOffset = currentHandOffsets[index];
                input.previousAnchorValid = hand.previousAnchorValid;
                input.currentAnchorValid = current.anchorValid;
                input.previousAnchor = hand.previousAnchor;
                input.currentAnchor = current.anchor;
                input.movementScale = _config.movementScale;
                input.maximumDelta = _config.maximumHandDeltaGameUnits;
                input.followMovingSurface = _config.followMovingSurfaces;

                const auto motion = policy::evaluateHandMotion(input);
                if (motion.discontinuity) {
                    ++discontinuityCount;
                    hand.blendWeight = 0.0f;
                } else if (motion.valid) {
                    contributions[index] = {
                        true,
                        hand.blendWeight,
                        motion.totalDelta,
                        motion.pullDelta,
                    };
                }

                hand.previousHandOffset = currentHandOffsets[index];
                hand.previousAnchorValid = current.anchorValid;
                hand.previousAnchor = current.anchor;
            }

            motionBlend = policy::blendHandMotions(contributions);
            if (discontinuityCount > 0 && motionBlend.valid) {
                logger::warn(
                    "Ignored {} held-hand tracking discontinuity above {:.1f} game units; valid hand motion continued.",
                    discontinuityCount,
                    _config.maximumHandDeltaGameUnits);
            } else if (discontinuityCount > 0) {
                logger::warn(
                    "All held-hand motion exceeded the {:.1f} game-unit continuity limit; rebasing climb motion.",
                    _config.maximumHandDeltaGameUnits);
                _targetPlayerPosition = access.playerPosition;
                _targetPositionValid = true;
                _velocityHistory.clear();
            }
        }

        if (!suspendGravity(access)) {
            logger::error(
                "Could not suspend the verified player-controller gravity field; climbing failed closed.");
            finishClimb(&access, 0.0f, false, "gravity-write");
            clearTargets();
            return;
        }

        if (!_targetPositionValid) {
            _targetPlayerPosition = access.playerPosition;
            _targetPositionValid = true;
        }
        const float targetSeparation = policy::length(policy::subtract(
            _targetPlayerPosition,
            access.playerPosition));
        if (targetSeparation >
            _config.maximumTargetSeparationGameUnits) {
            logger::warn(
                "Player/target separation {:.1f} exceeded {:.1f}; rebasing after external displacement.",
                targetSeparation,
                _config.maximumTargetSeparationGameUnits);
            _targetPlayerPosition = access.playerPosition;
            _velocityHistory.clear();
            motionBlend = {};
        }

        policy::Vec3 blendedPull{};
        if (motionBlend.valid) {
            blendedPull = motionBlend.pullDelta;
            _targetPlayerPosition = policy::add(
                _targetPlayerPosition,
                motionBlend.movementDelta);
        }
        if (!ownershipTransition) {
            _velocityHistory.push(
                policy::VelocitySample{
                    blendedPull,
                    snapshot.deltaSeconds,
                },
                _config.launch.historySeconds);
        }

        const float targetLead = policy::length(policy::subtract(
            _targetPlayerPosition,
            access.playerPosition));
        const float smoothingSpeed = policy::adaptiveSmoothingSpeed(
            _config.smoothingSpeed,
            targetLead);
        const float smoothingFactor = policy::exponentialSmoothingFactor(
            smoothingSpeed,
            snapshot.deltaSeconds);
        const policy::Vec3 desiredPosition = policy::add(
            access.playerPosition,
            policy::scale(
                policy::subtract(
                    _targetPlayerPosition,
                    access.playerPosition),
                smoothingFactor));

        policy::Vec3 clampedPosition{};
        bool headClamped = false;
        if (!tryClampHeadMotion(
                snapshot,
                access.playerPosition,
                desiredPosition,
                clampedPosition,
                headClamped)) {
            logger::warn(
                "ROCK head-motion raycast failed; rebasing without moving this frame.");
            _targetPlayerPosition = access.playerPosition;
            _velocityHistory.clear();
            clampedPosition = access.playerPosition;
        } else if (headClamped) {
            _targetPlayerPosition = clampedPosition;
        }

        const policy::Vec3 appliedStep = policy::subtract(
            clampedPosition,
            access.playerPosition);
        if (!trySetVelocity(access, {})) {
            logger::error(
                "Player velocity cancellation failed during climbing; failing closed.");
            finishClimb(nullptr, 0.0f, false, "velocity-write");
            clearTargets();
            return;
        }
        if (policy::lengthSquared(appliedStep) > 1.0e-6f &&
            !trySetPlayerPosition(access.player, clampedPosition)) {
            logger::error(
                "Player SetPosition failed during climbing; failing closed.");
            finishClimb(nullptr, 0.0f, false, "position-write");
            clearTargets();
            return;
        }

        if (_config.detailedTelemetry && ++_telemetryFrames >= 90) {
            _telemetryFrames = 0;
            logger::debug(
                "Climb frame={} hands={} pull=({:.2f},{:.2f},{:.2f}) targetLead={:.2f} step=({:.2f},{:.2f},{:.2f}) headClamped={} history={}.",
                snapshot.frameIndex,
                currentHeldCount,
                blendedPull.x,
                blendedPull.y,
                blendedPull.z,
                policy::length(policy::subtract(
                    _targetPlayerPosition,
                    access.playerPosition)),
                appliedStep.x,
                appliedStep.y,
                appliedStep.z,
                headClamped ? "yes" : "no",
                _velocityHistory.size());
        }
    }

    bool ClimbingRuntime::publishTargets(
        const RockProviderFrameSnapshot& snapshot) noexcept
    {
        std::array<RockProviderTouchGrabTargetV1, 2> targets{};
        const auto makeTarget = [&](
                                    const std::uint64_t targetId,
                                    const RockProviderTouchGrabTargetFlagV1
                                        allowedHand) {
            RockProviderTouchGrabTargetV1 target{};
            target.targetId = targetId;
            target.targetGeneration = _targetGeneration;
            target.kind = RockProviderTouchGrabKindV1::FixedAnchor;
            target.flags =
                flag(allowedHand) |
                flag(RockProviderTouchGrabTargetFlagV1::AllowTwoHands) |
                flag(RockProviderTouchGrabTargetFlagV1::MatchAnyBody) |
                flag(RockProviderTouchGrabTargetFlagV1::MatchStaticMotion) |
                flag(RockProviderTouchGrabTargetFlagV1::MatchKeyframedMotion);
            target.bodyId = INVALID_BODY_ID;
            target.allowedLayerMask =
                policy::CLIMBABLE_WORLD_LAYER_MASK;
            target.leaseFrames = TARGET_LEASE_FRAMES;
            target.worldGeneration = snapshot.worldGeneration;
            target.skeletonGeneration = snapshot.skeletonGeneration;
            target.providerGeneration = snapshot.providerGeneration;
            return target;
        };
        targets[0] = makeTarget(
            RIGHT_TARGET_ID,
            RockProviderTouchGrabTargetFlagV1::AllowRightHand);
        targets[1] = makeTarget(
            LEFT_TARGET_ID,
            RockProviderTouchGrabTargetFlagV1::AllowLeftHand);

        const auto result = rockApiClient().publishTargets(
            CLIMBING_SCOPE_TOKEN,
            targets);
        const auto resultValue = static_cast<std::uint32_t>(result);
        if (result != RockProviderResultV1::Ok) {
            if (resultValue != _lastPublishResult) {
                logger::error(
                    "ROCK climbing-target publication failed: result={}.",
                    resultValue);
                _lastPublishResult = resultValue;
            }
            _targetsPublished = false;
            return false;
        }
        if (!_targetsPublished) {
            logger::info(
                "Armed joinable right/left ROCK fixed-surface targets generation={} layers=0x{:016X} lease={}.",
                _targetGeneration,
                policy::CLIMBABLE_WORLD_LAYER_MASK,
                TARGET_LEASE_FRAMES);
        }
        _targetsPublished = true;
        _lastPublishResult = UINT32_MAX;
        return true;
    }

    void ClimbingRuntime::clearTargets() noexcept
    {
        if (!_targetsPublished || !rockApiClient().ready()) {
            _targetsPublished = false;
            return;
        }
        const auto result = rockApiClient().clearTargets(
            CLIMBING_SCOPE_TOKEN);
        if (result != RockProviderResultV1::Ok &&
            result != RockProviderResultV1::TargetUnavailable &&
            result != RockProviderResultV1::NotReady) {
            logger::warn(
                "ROCK climbing-target clear returned result={}.",
                static_cast<std::uint32_t>(result));
        }
        _targetsPublished = false;
    }

    bool ClimbingRuntime::observeHands(
        std::array<ObservedHand, 2>& observed,
        bool& invalidated) noexcept
    {
        observed = {};
        invalidated = false;
        std::array<RockProviderTouchGrabStateV1, 2> states{};
        std::uint32_t count = 0;
        const auto result = rockApiClient().copyStates(
            CLIMBING_SCOPE_TOKEN,
            states,
            count);
        const auto resultValue = static_cast<std::uint32_t>(result);
        if (result != RockProviderResultV1::Ok || count > states.size()) {
            if (resultValue != _lastStateResult) {
                logger::error(
                    "ROCK climbing-state readback failed: result={} count={}.",
                    resultValue,
                    count);
                _lastStateResult = resultValue;
            }
            return false;
        }
        _lastStateResult = UINT32_MAX;

        for (std::uint32_t index = 0; index < count; ++index) {
            const auto& state = states[index];
            if (state.targetGeneration != _targetGeneration) {
                continue;
            }
            if (state.targetId != RIGHT_TARGET_ID &&
                state.targetId != LEFT_TARGET_ID) {
                continue;
            }

            if (state.phase == RockProviderTouchGrabPhaseV1::Invalidated ||
                state.phase == RockProviderTouchGrabPhaseV1::Yielded) {
                invalidated = true;
                continue;
            }
            const bool targetHeld =
                state.phase == RockProviderTouchGrabPhaseV1::Held &&
                state.kind == RockProviderTouchGrabKindV1::FixedAnchor &&
                state.bodyId != INVALID_BODY_ID;
            if (!targetHeld) {
                continue;
            }

            const auto activeHands = policy::decodeActiveHands(
                state.activeHandMask,
                static_cast<std::uint32_t>(
                    RockProviderTouchGrabHandMaskV1::Right),
                static_cast<std::uint32_t>(
                    RockProviderTouchGrabHandMaskV1::Left));
            const auto mergeHand = [&](
                                       const std::size_t handIndex,
                                       const bool active) {
                if (!active) {
                    return;
                }
                auto& hand = observed[handIndex];
                if (hand.held && hand.bodyId != state.bodyId) {
                    invalidated = true;
                    return;
                }
                hand.held = true;
                hand.bodyId = state.bodyId;
            };
            mergeHand(0, activeHands.right);
            mergeHand(1, activeHands.left);
        }

        if (invalidated) {
            return true;
        }

        constexpr std::array<RockProviderHand, 2> PROVIDER_HANDS{
            RockProviderHand::Right,
            RockProviderHand::Left,
        };
        for (std::size_t index = 0; index < observed.size(); ++index) {
            auto& hand = observed[index];
            if (!hand.held) {
                continue;
            }

            RockProviderHandInteractionStateV1 interaction{};
            const auto interactionResult =
                rockApiClient().queryHandInteractionState(
                    PROVIDER_HANDS[index],
                    interaction);
            const bool stateMatches =
                interactionResult == RockProviderResultV1::Ok &&
                interaction.hand == PROVIDER_HANDS[index] &&
                interaction.phase ==
                    RockProviderHandInteractionPhaseV1::Holding &&
                hasHandInteractionFlag(
                    interaction.flags,
                    RockProviderHandInteractionFlagV1::Valid) &&
                hasHandInteractionFlag(
                    interaction.flags,
                    RockProviderHandInteractionFlagV1::TouchGrab) &&
                hasHandInteractionFlag(
                    interaction.flags,
                    RockProviderHandInteractionFlagV1::FixedSurfaceLatch) &&
                hasHandInteractionFlag(
                    interaction.flags,
                    RockProviderHandInteractionFlagV1::SurfaceAnchorValid) &&
                interaction.primaryBodyId == hand.bodyId &&
                interaction.worldGeneration == _worldGeneration &&
                interaction.skeletonGeneration == _skeletonGeneration &&
                interaction.providerGeneration == _providerGeneration;
            const policy::Vec3 anchor = point(
                interaction.surfaceAnchorGame);
            hand.anchorValid = stateMatches && validCoordinate(anchor);
            hand.anchor = hand.anchorValid ? anchor : policy::Vec3{};
        }
        return true;
    }

    bool ClimbingRuntime::tryClampHeadMotion(
        const RockProviderFrameSnapshot& snapshot,
        const policy::Vec3 currentPlayerPosition,
        const policy::Vec3 desiredPlayerPosition,
        policy::Vec3& clampedPlayerPosition,
        bool& wasClamped) noexcept
    {
        clampedPlayerPosition = currentPlayerPosition;
        wasClamped = false;
        const policy::Vec3 step = policy::subtract(
            desiredPlayerPosition,
            currentPlayerPosition);
        const float distance = policy::length(step);
        if (distance <= 1.0e-4f) {
            return true;
        }

        RockProviderWorldRaycastRequestV1 request{};
        request.startGame = providerPoint(
            translation(snapshot.hmdTransform));
        request.directionGame = providerPoint(
            policy::divide(step, distance));
        request.maxDistanceGame =
            distance + _config.headClearanceGameUnits;
        request.worldGeneration = snapshot.worldGeneration;
        request.skeletonGeneration = snapshot.skeletonGeneration;
        request.providerGeneration = snapshot.providerGeneration;

        RockProviderWorldRaycastResultV1 result{};
        const auto queryResult = rockApiClient().queryWorldRaycast(
            request,
            result);
        if (queryResult != RockProviderResultV1::Ok) {
            return false;
        }

        float allowedDistance = distance;
        if (result.hit != 0 &&
            std::isfinite(result.hitDistanceGame) &&
            result.hitDistanceGame >= 0.0f) {
            allowedDistance = std::clamp(
                result.hitDistanceGame -
                    _config.headClearanceGameUnits,
                0.0f,
                distance);
            wasClamped = allowedDistance + 1.0e-4f < distance;
        }
        clampedPlayerPosition = policy::add(
            currentPlayerPosition,
            policy::scale(
                policy::divide(step, distance),
                allowedDistance));
        return validCoordinate(clampedPlayerPosition);
    }

    bool ClimbingRuntime::tryResolveControllerAccess(
        ControllerAccess& access,
        ControllerResolveStage& deepestStage) noexcept
    {
        access = {};
        deepestStage = ControllerResolveStage::None;
        bool valid = false;

        __try {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }
            deepestStage = ControllerResolveStage::Player;

            // FO4VR 1.2.72 authority: Actor+0x300 -> AIProcess+0x08 ->
            // MiddleHighProcessData+0x3E8. ROCK's established controller
            // runtime uses the same guarded walk; +0x330 is the separately
            // verified gravity scalar consumed by the character state machine.
            const auto* playerBytes =
                reinterpret_cast<const std::uint8_t*>(player);
            const void* currentProcess =
                *reinterpret_cast<void* const*>(playerBytes + 0x300);
            if (!currentProcess) {
                return false;
            }
            deepestStage = ControllerResolveStage::CurrentProcess;

            const auto* processBytes =
                reinterpret_cast<const std::uint8_t*>(currentProcess);
            const void* middleHigh =
                *reinterpret_cast<void* const*>(processBytes + 0x08);
            if (!middleHigh) {
                return false;
            }
            deepestStage = ControllerResolveStage::MiddleHigh;

            const auto* middleHighBytes =
                reinterpret_cast<const std::uint8_t*>(middleHigh);
            auto* controller = reinterpret_cast<RE::bhkCharacterController*>(
                *reinterpret_cast<void* const*>(middleHighBytes + 0x3E8));
            if (!controller) {
                return false;
            }
            deepestStage = ControllerResolveStage::Controller;

            const auto velocityDispatch =
                resolveVelocityDispatch(controller);
            if (velocityDispatch.vtableAddress == 0) {
                return false;
            }
            deepestStage = ControllerResolveStage::Vtable;
            if (!velocityDispatch.valid()) {
                return false;
            }
            deepestStage = ControllerResolveStage::VelocityDispatch;

            const auto* controllerBytes =
                reinterpret_cast<const std::uint8_t*>(controller);
            const float gravity =
                *reinterpret_cast<const float*>(controllerBytes + 0x330);
            if (!std::isfinite(gravity) || std::abs(gravity) > 1000.0f) {
                return false;
            }
            deepestStage = ControllerResolveStage::Gravity;

            access.player = player;
            access.controller = controller;
            access.controllerIdentity =
                reinterpret_cast<std::uintptr_t>(controller);
            access.controllerVtable = velocityDispatch.vtableAddress;
            access.velocityFunction = velocityDispatch.functionAddress;
            access.velocityImplementation =
                velocityDispatch.spec->implementation;
            access.gravity = gravity;
            valid = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            valid = false;
        }
        return valid;
    }

    bool ClimbingRuntime::tryResolvePlayerAccess(
        PlayerAccess& access,
        ControllerResolveStage& deepestStage) noexcept
    {
        access = {};
        ControllerAccess controllerAccess{};
        if (!tryResolveControllerAccess(controllerAccess, deepestStage)) {
            return false;
        }
        static_cast<ControllerAccess&>(access) = controllerAccess;

        bool valid = false;
        __try {
            const RE::NiPoint3 playerPosition =
                controllerAccess.player->GetPosition();
            const policy::Vec3 convertedPosition{
                playerPosition.x,
                playerPosition.y,
                playerPosition.z,
            };
            if (!validCoordinate(convertedPosition)) {
                return false;
            }
            deepestStage = ControllerResolveStage::Position;
            access.playerPosition = convertedPosition;
            deepestStage = ControllerResolveStage::Complete;
            valid = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            valid = false;
        }
        return valid;
    }

    bool ClimbingRuntime::tryWriteGravity(
        RE::bhkCharacterController* controller,
        const float value) noexcept
    {
        if (!controller || !std::isfinite(value)) {
            return false;
        }
        bool written = false;
        __try {
            auto* controllerBytes =
                reinterpret_cast<std::uint8_t*>(controller);
            *reinterpret_cast<float*>(controllerBytes + 0x330) = value;
            written = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            written = false;
        }
        return written;
    }

    bool ClimbingRuntime::trySetVelocity(
        const ControllerAccess& access,
        const policy::Vec3 velocityHavok) noexcept
    {
        if (!access.controller || access.velocityFunction == 0 ||
            access.velocityImplementation ==
                controller_policy::VelocityImplementation::Unknown ||
            !policy::finite(velocityHavok)) {
            return false;
        }
        bool written = false;
        __try {
            const RE::hkVector4f velocity(
                velocityHavok.x,
                velocityHavok.y,
                velocityHavok.z,
                0.0f);
            using SetLinearVelocityFunction = void (*)(
                RE::bhkCharacterController*,
                const RE::hkVector4f*);
            const auto function =
                reinterpret_cast<SetLinearVelocityFunction>(
                    access.velocityFunction);
            function(access.controller, std::addressof(velocity));
            written = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            written = false;
        }
        if (written && !_velocityDispatchLogged) {
            const char* implementation =
                access.velocityImplementation ==
                        controller_policy::VelocityImplementation::Proxy ?
                    "proxy" :
                    "rigid-body";
            logger::info(
                "Validated FO4VR {} controller velocity dispatch vtable={:016X} function={:016X} slot=0x{:X}.",
                implementation,
                access.controllerVtable,
                access.velocityFunction,
                controller_policy::VELOCITY_SLOT_OFFSET);
            _velocityDispatchLogged = true;
        }
        return written;
    }

    bool ClimbingRuntime::trySetPlayerPosition(
        RE::PlayerCharacter* player,
        const policy::Vec3 positionGame) noexcept
    {
        if (!player || !validCoordinate(positionGame)) {
            return false;
        }
        bool written = false;
        __try {
            player->SetPosition(
                RE::NiPoint3{
                    positionGame.x,
                    positionGame.y,
                    positionGame.z,
                },
                true);
            written = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            written = false;
        }
        return written;
    }

    bool ClimbingRuntime::suspendGravity(
        const ControllerAccess& access) noexcept
    {
        const bool controllerChanged =
            _gravityOwned &&
            _gravityControllerIdentity != access.controllerIdentity;
        if (controllerChanged) {
            logger::warn(
                "Player controller identity changed during climbing; transferring gravity ownership without dereferencing the retired controller.");
            clearGravityOwnership();
        }

        if (!_gravityOwned) {
            if (!tryWriteGravity(access.controller, 0.0f)) {
                return false;
            }
            _savedGravity = access.gravity;
            _gravityControllerIdentity = access.controllerIdentity;
            _gravityOwned = true;
            _gravityRestoreDeferredLogged = false;
            logger::info(
                "Suspended player gravity (saved {:.4f}) for controller {:016X}.",
                _savedGravity,
                _gravityControllerIdentity);
            return true;
        }
        return tryWriteGravity(access.controller, 0.0f);
    }

    void ClimbingRuntime::restoreGravity(
        const ControllerAccess* currentAccess) noexcept
    {
        if (!_gravityOwned) {
            return;
        }

        ControllerAccess resolved{};
        ControllerResolveStage deepest = ControllerResolveStage::None;
        const ControllerAccess* access = currentAccess;
        if (!access && tryResolveControllerAccess(resolved, deepest)) {
            access = &resolved;
        }

        const auto decision = controller_policy::decideGravityRestore(
            access != nullptr,
            _gravityControllerIdentity,
            access ? access->controllerIdentity : 0);
        if (decision ==
            controller_policy::GravityRestoreDecision::Restore) {
            if (tryWriteGravity(access->controller, _savedGravity)) {
                logger::info(
                    "Restored player gravity to {:.4f} for controller {:016X}.",
                    _savedGravity,
                    _gravityControllerIdentity);
                clearGravityOwnership();
            } else {
                if (!_gravityRestoreDeferredLogged) {
                    logger::warn(
                        "Player gravity restoration write failed; saved value {:.4f} retained for retry.",
                        _savedGravity);
                    _gravityRestoreDeferredLogged = true;
                }
            }
            return;
        }

        if (decision ==
            controller_policy::GravityRestoreDecision::Retire) {
            logger::warn(
                "Retired saved gravity ownership after the controller changed (saved {:016X}, current {:016X}); the replacement controller was not modified.",
                _gravityControllerIdentity,
                access->controllerIdentity);
            clearGravityOwnership();
            return;
        }

        if (!_gravityRestoreDeferredLogged) {
            logger::warn(
                "Player gravity restoration deferred; controller-only resolution stopped at stage={} and saved value {:.4f} was retained.",
                controllerStageName(deepest),
                _savedGravity);
            _gravityRestoreDeferredLogged = true;
        }
    }

    void ClimbingRuntime::clearGravityOwnership() noexcept
    {
        _gravityOwned = false;
        _gravityRestoreDeferredLogged = false;
        _savedGravity = 0.0f;
        _gravityControllerIdentity = 0;
    }

    void ClimbingRuntime::finishClimb(
        const PlayerAccess* currentAccess,
        const float gameToHavokScale,
        const bool allowLaunch,
        const char* reason) noexcept
    {
        if (!_climbing && !_gravityOwned) {
            resetLocalState();
            return;
        }

        std::array<policy::VelocitySample, VelocityHistory::CAPACITY>
            samples{};
        const auto chronological =
            _velocityHistory.copyChronological(samples);
        const policy::Vec3 launchGame =
            allowLaunch && currentAccess &&
                    std::isfinite(gameToHavokScale) &&
                    gameToHavokScale > 0.0f ?
                policy::calculateLaunchVelocity(
                    chronological,
                    _config.launch) :
                policy::Vec3{};

        restoreGravity(currentAccess);

        bool launchApplied = false;
        if (currentAccess) {
            if (policy::lengthSquared(launchGame) > 1.0e-6f) {
                const policy::Vec3 launchHavok = policy::scale(
                    launchGame,
                    gameToHavokScale);
                launchApplied = trySetVelocity(
                    *currentAccess,
                    launchHavok);
                if (!launchApplied) {
                    logger::warn(
                        "Could not apply final launch velocity; normal gravity resumed.");
                }
            } else {
                static_cast<void>(
                    trySetVelocity(*currentAccess, {}));
            }
        }

        if (_climbing) {
            logger::info(
                "Climb ended reason={} launch={} velocityGame=({:.1f},{:.1f},{:.1f}) speed={:.1f} samples={}.",
                reason ? reason : "unknown",
                launchApplied ? "applied" : "none",
                launchGame.x,
                launchGame.y,
                launchGame.z,
                policy::length(launchGame),
                chronological.size());
        }
        resetLocalState();
    }

    void ClimbingRuntime::resetLocalState() noexcept
    {
        _climbing = false;
        _targetPositionValid = false;
        _targetPlayerPosition = {};
        _hands = {};
        _velocityHistory.clear();
        _telemetryFrames = 0;
    }

    const char* ClimbingRuntime::controllerStageName(
        const ControllerResolveStage stage) noexcept
    {
        switch (stage) {
        case ControllerResolveStage::None:
            return "none";
        case ControllerResolveStage::Player:
            return "player";
        case ControllerResolveStage::CurrentProcess:
            return "currentProcess";
        case ControllerResolveStage::MiddleHigh:
            return "middleHigh";
        case ControllerResolveStage::Controller:
            return "controller";
        case ControllerResolveStage::Vtable:
            return "controllerVtable";
        case ControllerResolveStage::VelocityDispatch:
            return "velocityDispatch";
        case ControllerResolveStage::Gravity:
            return "gravity";
        case ControllerResolveStage::Position:
            return "playerPosition";
        case ControllerResolveStage::Complete:
            return "complete";
        }
        return "unknown";
    }

    std::uint32_t ClimbingRuntime::nextTargetGeneration() noexcept
    {
        ++_targetGeneration;
        if (_targetGeneration == 0) {
            _targetGeneration = 1;
        }
        return _targetGeneration;
    }
}
