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
#include <cstring>

namespace rock_wall_climbing
{
    namespace
    {
        using namespace rock::api;

        constexpr std::uint64_t CLIMBING_SCOPE_TOKEN =
            0x434C'494D'425F'5631ull;
        constexpr std::uint64_t RIGHT_TARGET_ID =
            0x434C'494D'425F'5201ull;
        constexpr std::uint64_t LEFT_TARGET_ID =
            0x434C'494D'425F'4C02ull;
        constexpr std::uint32_t TARGET_LEASE_FRAMES = 4;
        constexpr std::uint32_t INVALID_BODY_ID = 0x7FFF'FFFFu;
        constexpr float MAXIMUM_FRAME_DELTA_SECONDS = 0.1f;
        constexpr std::uint32_t MAXIMUM_HEAD_MOTION_PASSES = 3;
        // A flight that never lands (a fall out of the world) is handed back
        // to the game; the handover is logged.
        constexpr float MAXIMUM_FLIGHT_SECONDS = 12.0f;
        // The ground ray reaches this far below the feet.
        constexpr float GROUND_PROBE_BELOW_FEET = 4.0f;
        // Head height above the feet used when tracking reports less.
        constexpr float MINIMUM_HEAD_HEIGHT = 40.0f;
        constexpr std::uint32_t CAPSULE_DIAGNOSTIC_FRAME_INTERVAL = 16;
        constexpr std::uintptr_t CAPSULE_CLEARANCE_QUERY_RVA = 0x1E21030;
        constexpr std::uintptr_t PROXY_GET_WORLD_RVA = 0x1E4DEC0;
        constexpr std::uintptr_t RIGID_GET_WORLD_RVA = 0x1E538A0;
        constexpr std::array<std::uint8_t, 16> CAPSULE_CLEARANCE_QUERY_PREFIX{
            0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20, 0x48,
            0x89, 0x50, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41,
        };
        constexpr std::array<std::uint8_t, 5> CAPSULE_COLLISION_FLAGS{
            1, 1, 1, 1, 0,
        };
        constexpr std::array<std::uint8_t, 16> GET_WORLD_PREFIX{
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
            0x01, 0x48, 0x8D, 0x54, 0x24, 0x30, 0x48, 0x8B,
        };
        constexpr std::array<std::uint8_t, 6> ENTRY_JUMP_PREFIX{
            0xFF, 0x25, 0, 0, 0, 0,
        };

        static_assert(
            offsetof(RE::bhkCharacterController, gravity) == 0x330,
            "CommonLibF4VR bhkCharacterController gravity layout changed");
        static_assert(offsetof(RE::bhkCharacterController, shapes) == 0x360);
        static_assert(controller_policy::VELOCITY_SLOT_OFFSET == 0x1E0);

        constexpr std::size_t REQUIRED_SNAPSHOT_BYTES = sizeof(rock::api::core::Snapshot);

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
            const rock::api::touch::TouchGrabTargetFlag value) noexcept
        {
            return static_cast<std::uint32_t>(value);
        }

        [[nodiscard]] constexpr bool hasHandInteractionFlag(
            const std::uint32_t flags,
            const rock::api::grab::HandInteractionFlag value) noexcept
        {
            return (flags & static_cast<std::uint32_t>(value)) != 0;
        }

        [[nodiscard]] constexpr bool hasTouchGrabStateFlag(
            const std::uint32_t flags,
            const rock::api::touch::TouchGrabStateFlag value) noexcept
        {
            return (flags & static_cast<std::uint32_t>(value)) != 0;
        }

        [[nodiscard]] constexpr bool hasWorldRaycastResultFlag(
            const std::uint32_t flags,
            const rock::api::collision::WorldRaycastResultFlag value) noexcept
        {
            return (flags & static_cast<std::uint32_t>(value)) != 0;
        }

        [[nodiscard]] constexpr bool hasEnrichmentFlag(
            const std::uint32_t flags,
            const rock_wall_climbing::FrameFlag value) noexcept
        {
            return (flags & static_cast<std::uint32_t>(value)) != 0;
        }

        [[nodiscard]] policy::Vec3 point(
            const rock::api::Point3 value) noexcept
        {
            return { value.x, value.y, value.z };
        }

        [[nodiscard]] policy::Vec3 translation(
            const rock::api::Transform& value) noexcept
        {
            return {
                value.translate[0],
                value.translate[1],
                value.translate[2],
            };
        }

        [[nodiscard]] rock::api::Point3 providerPoint(
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
            "Connected to ROCK fixed-surface latch and world-raycast services.");
        return true;
    }

    void ClimbingRuntime::beginSession() noexcept
    {
        try {
            if (_sessionActive) {
                finishClimb(nullptr, "session-refresh");
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
        finishClimb(nullptr, "session-ended");
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

    void ROCK_CALL ClimbingRuntime::onRockFrame(
        const rock_wall_climbing::Frame* snapshot,
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
            runtime->finishClimb(nullptr, "callback-exception");
            runtime->clearTargets();
        } catch (...) {
            logger::error(
                "Climbing frame failed closed after an unknown exception.");
            runtime->finishClimb(nullptr, "callback-exception");
            runtime->clearTargets();
        }
    }

    std::uint32_t ClimbingRuntime::snapshotBlockers(
        const rock_wall_climbing::Frame& snapshot) const noexcept
    {
        std::uint32_t blockers = 0;
        if (!_sessionActive) {
            blockers |= SessionInactive;
        }
        if (!_config.enabled) {
            blockers |= AddonDisabled;
        }
        if (snapshot.size < REQUIRED_SNAPSHOT_BYTES ||
            snapshot.version != rock::api::core::Snapshot{}.version) {
            blockers |= InvalidSnapshot;
            return blockers;
        }
        if (snapshot.providerReady == 0 ||
            !hasLifecycleFlag(
                snapshot.lifecycleFlags,
                rock::api::core::LifecycleFlag::ProviderReady)) {
            blockers |= ProviderNotReady;
        }
        if (snapshot.frikSkeletonReady == 0 ||
            !hasLifecycleFlag(
                snapshot.lifecycleFlags,
                rock::api::core::LifecycleFlag::SkeletonReady)) {
            blockers |= SkeletonNotReady;
        }
        if (snapshot.menuBlocking != 0 || snapshot.configBlocking != 0 ||
            hasLifecycleFlag(
                snapshot.lifecycleFlags,
                rock::api::core::LifecycleFlag::MenuBlocking) ||
            hasLifecycleFlag(
                snapshot.lifecycleFlags,
                rock::api::core::LifecycleFlag::ConfigBlocking)) {
            blockers |= MenuOrConfigBlocked;
        }
        if (hasLifecycleFlag(
                snapshot.lifecycleFlags,
                rock::api::core::LifecycleFlag::LoadingOrWorldTransition) ||
            !hasLifecycleFlag(
                snapshot.lifecycleFlags,
                rock::api::core::LifecycleFlag::WorldAvailable) ||
            snapshot.sample.worldGeneration == 0 ||
            snapshot.sample.skeletonGeneration == 0 ||
            snapshot.sample.providerGeneration == 0) {
            blockers |= WorldTransition;
        }
        if (!hasLifecycleFlag(
                snapshot.lifecycleFlags,
                rock::api::core::LifecycleFlag::GeneratedBodiesValid) ||
            !hasLifecycleFlag(
                snapshot.lifecycleFlags,
                rock::api::core::LifecycleFlag::PhysicsWriteAllowed) ||
            !hasLifecycleFlag(
                snapshot.lifecycleFlags,
                rock::api::core::LifecycleFlag::VisualWriteAllowed)) {
            blockers |= PhysicsOrVisualWritesBlocked;
        }
        if (!hasEnrichmentFlag(
                snapshot.enrichmentFlags,
                rock_wall_climbing::FrameFlag::DeltaSecondsValid) ||
            !std::isfinite(snapshot.deltaSeconds) ||
            snapshot.deltaSeconds <= 0.0f ||
            snapshot.deltaSeconds > MAXIMUM_FRAME_DELTA_SECONDS ||
            (_lastFrameIndex != 0 &&
                snapshot.sample.frameIndex <= _lastFrameIndex &&
                snapshot.sample.worldGeneration == _worldGeneration &&
                snapshot.sample.skeletonGeneration == _skeletonGeneration &&
                snapshot.sample.providerGeneration == _providerGeneration)) {
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
                rock_wall_climbing::FrameFlag::HmdTransformValid) ||
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
        const rock_wall_climbing::Frame& snapshot)
    {
        const std::uint32_t blockers = snapshotBlockers(snapshot);
        observeBlockers(blockers);
        if (blockers != 0) {
            finishClimb(nullptr,"operational-gate");
            clearTargets();
            return;
        }

        _lastFrameIndex = snapshot.sample.frameIndex;
        const bool generationChanged =
            snapshot.sample.worldGeneration != _worldGeneration ||
            snapshot.sample.skeletonGeneration != _skeletonGeneration ||
            snapshot.sample.providerGeneration != _providerGeneration;
        if (generationChanged) {
            finishClimb(nullptr,"provider-generation");
            clearTargets();
            _worldGeneration = snapshot.sample.worldGeneration;
            _skeletonGeneration = snapshot.sample.skeletonGeneration;
            _providerGeneration = snapshot.sample.providerGeneration;
            static_cast<void>(nextTargetGeneration());
            logger::info(
                "Climbing generations changed to world={} skeleton={} provider={} targetGeneration={}.",
                _worldGeneration,
                _skeletonGeneration,
                _providerGeneration,
                _targetGeneration);
        }

        if (!publishTargets(snapshot)) {
            finishClimb(nullptr,"target-publication");
            clearTargets();
            return;
        }

        std::array<ObservedHand, 2> observed{};
        bool invalidated = false;
        if (!observeHands(observed, invalidated)) {
            finishClimb(nullptr,"state-readback");
            clearTargets();
            return;
        }
        if (invalidated) {
            finishClimb(nullptr,"target-invalidated");
            clearTargets();
            static_cast<void>(nextTargetGeneration());
            logger::warn(
                "A climbing target was invalidated; rearming with targetGeneration={} next frame.",
                _targetGeneration);
            return;
        }

        const std::uint32_t currentHeldCount =
            static_cast<std::uint32_t>(observed[0].held) +
            static_cast<std::uint32_t>(observed[1].held);
        if (currentHeldCount == 0 && !_climbing && !_gravityOwned) {
            _hands = {};
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
            finishClimb(nullptr,"controller-unavailable");
            clearTargets();
            return;
        }
        if (_lastControllerFailure != ControllerResolveStage::Complete) {
            logger::info("Player controller chain recovered.");
            _lastControllerFailure = ControllerResolveStage::Complete;
        }

        // Read back the preceding submitted step before flushing its hand set.
        // This separates rejected pull/clamping from motion undone by the game.
        if (_motionPositionValid) {
            ++_motionTrace.observedFrames;
            _motionTrace.observedStep = policy::add(
                _motionTrace.observedStep,
                policy::subtract(access.playerPosition, _previousPlayerPosition));
            _motionTrace.externalStep = policy::add(
                _motionTrace.externalStep,
                policy::subtract(access.playerPosition, _previousSubmittedPosition));
        }
        const std::uint32_t currentHandMask =
            (observed[0].held ? 1u : 0u) | (observed[1].held ? 2u : 0u);
        if (_motionTrace.frames != 0 &&
            _motionTrace.handMask != currentHandMask) {
            logMotionTrace(currentHeldCount == 0 ? "release" : "handoff");
        } else if (_config.detailedTelemetry && _motionTrace.seconds >= 0.5f) {
            logMotionTrace("sample");
        }
        _previousPlayerPosition = access.playerPosition;
        _previousSubmittedPosition = access.playerPosition;
        _motionPositionValid = true;

        if (_air.phase != AirPhase::None) {
            if (currentHeldCount == 0) {
                updateAir(snapshot, access);
                return;
            }
            // A hand caught a surface: the flight or lift ends where it is
            // and climbing starts from this position.
            endAir("caught");
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
            finishClimb(&access,"hand-offset-invalid");
            clearTargets();
            return;
        }

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

        std::array<policy::HandMotionContribution, 2> contributions{};
        std::uint32_t discontinuityCount = 0;
        for (std::size_t index = 0; index < _hands.size(); ++index) {
            auto& hand = _hands[index];
            const auto& current = observed[index];
            const HandState previous = hand;
            const bool wasHeld = previous.held;
            const bool sameBody =
                wasHeld && current.held &&
                previous.bodyId == current.bodyId;
            const bool continuingOwnership =
                sameBody && previous.baselineValid;

            if (continuingOwnership) {
                hand = previous;
                hand.blendWeight = policy::advanceHandBlendWeight(
                    previous.blendWeight,
                    snapshot.deltaSeconds,
                    policy::HAND_JOIN_BLEND_SECONDS);
                policy::HandMotionInput input{};
                input.previousHandOffset = previous.previousHandOffset;
                input.currentHandOffset = currentHandOffsets[index];
                input.previousAnchorValid = previous.previousAnchorValid;
                input.currentAnchorValid = current.anchorValid;
                input.previousAnchor = previous.previousAnchor;
                input.currentAnchor = current.anchor;
                input.movementScale = _config.movementScale;
                input.maximumDelta = _config.maximumHandDeltaGameUnits;
                input.followMovingSurface = _config.followMovingSurfaces;

                const auto motion = policy::evaluateHandMotion(input);
                if (motion.discontinuity) {
                    ++discontinuityCount;
                    hand.blendWeight = 0.0f;
                } else if (motion.valid) {
                    const float weight =
                        policy::activityAdjustedHandWeight(
                            hand.blendWeight,
                            motion.pullDelta);
                    contributions[index] = {
                        true,
                        weight,
                        motion.totalDelta,
                        motion.pullDelta,
                    };
                }

                hand.previousHandOffset = currentHandOffsets[index];
                hand.previousAnchorValid = current.anchorValid;
                hand.previousAnchor = current.anchor;
                continue;
            }

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
                        previous.bodyId,
                        current.bodyId);
                }
                hand = {};
                hand.held = true;
                hand.baselineValid = true;
                hand.bodyId = current.bodyId;
                hand.previousHandOffset = currentHandOffsets[index];
                hand.previousAnchorValid = current.anchorValid;
                hand.previousAnchor = current.anchor;
            } else {
                if (wasHeld) {
                    logger::info(
                        "{} hand released climbing surface.",
                        index == 0 ? "Right" : "Left");
                }
                hand = {};
            }
        }

        policy::HandMotionBlend motionBlend =
            policy::blendHandMotions(contributions);
        if (discontinuityCount > 0 && motionBlend.valid) {
            logger::warn(
                "Ignored {} hand tracking discontinuity above {:.1f} game units; valid hand motion continued.",
                discontinuityCount,
                _config.maximumHandDeltaGameUnits);
        } else if (discontinuityCount > 0) {
            logger::warn(
                "All usable hand motion exceeded the {:.1f} game-unit continuity limit; rebasing climb motion.",
                _config.maximumHandDeltaGameUnits);
            _targetPlayerPosition = access.playerPosition;
            _targetPositionValid = true;
            _velocityHistory.clear();
        }

        const policy::Vec3 blendedPull = motionBlend.valid ?
            motionBlend.pullDelta :
            policy::Vec3{};

        if (currentHeldCount == 0) {
            if (_climbing) {
                releaseToAir(snapshot, access);
            } else {
                // Only a deferred gravity restore is left to finish.
                finishClimb(&access, "gravity-restore");
            }
            return;
        }

        _velocityHistory.push(
            policy::VelocitySample{
                blendedPull,
                snapshot.deltaSeconds,
            },
            _config.launch.historySeconds);

        if (!suspendGravity(access)) {
            logger::error(
                "Could not suspend the verified player-controller gravity field; climbing failed closed.");
            finishClimb(&access,"gravity-write");
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

        if (motionBlend.valid) {
            _targetPlayerPosition = policy::add(
                _targetPlayerPosition,
                motionBlend.movementDelta);
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
        std::uint32_t headSlidePasses = 0;
        const bool headQuerySucceeded = tryResolveHeadMotion(
            snapshot,
            access.playerPosition,
            desiredPosition,
            clampedPosition,
            headClamped,
            headSlidePasses);
        if (!headQuerySucceeded) {
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
        CapsuleQueryResult destinationQuery{};
        CapsuleQueryResult pathQuery{};
        const bool sampledCapsule = _config.detailedTelemetry &&
            snapshot.sample.frameIndex % CAPSULE_DIAGNOSTIC_FRAME_INTERVAL == 0 &&
            policy::lengthSquared(appliedStep) > 1.0e-6f;
        if (sampledCapsule) {
            // Shadow-query the actual controller shape; this is diagnostic
            // evidence only and does not change the established climb step.
            destinationQuery = queryNativeCapsuleClearance(
                access, clampedPosition, false);
            if (destinationQuery.clearance == CapsuleClearance::Clear) {
                pathQuery = queryNativeCapsuleClearance(
                    access, clampedPosition, true);
            }
        }
        if (!trySetVelocity(access, {})) {
            logger::error(
                "Player velocity cancellation failed during climbing; failing closed.");
            finishClimb(nullptr,"velocity-write");
            clearTargets();
            return;
        }
        if (policy::lengthSquared(appliedStep) > 1.0e-6f &&
            !trySetPlayerPosition(access.player, clampedPosition)) {
            logger::error(
                "Player SetPosition failed during climbing; failing closed.");
            finishClimb(nullptr,"position-write");
            clearTargets();
            return;
        }

        _previousSubmittedPosition = policy::lengthSquared(appliedStep) > 1.0e-6f ?
            clampedPosition : access.playerPosition;
        auto& trace = _motionTrace;
        ++trace.frames;
        trace.handMask = currentHandMask;
        trace.seconds += snapshot.deltaSeconds;
        for (std::size_t index = 0; index < contributions.size(); ++index) {
            const auto& contribution = contributions[index];
            if (contribution.valid) {
                trace.pull[index] = policy::add(trace.pull[index], contribution.pullDelta);
                trace.upwardPull[index] += std::max(0.0f, contribution.pullDelta.z);
                trace.carry[index] = policy::add(trace.carry[index],
                    policy::subtract(contribution.movementDelta, contribution.pullDelta));
                trace.weightSum[index] += contribution.weight;
            }
        }
        trace.blendedPull = policy::add(trace.blendedPull, blendedPull);
        trace.upwardBlendedPull += std::max(0.0f, blendedPull.z);
        trace.upwardRequested += std::max(0.0f, desiredPosition.z - access.playerPosition.z);
        trace.upwardSubmitted += std::max(0.0f, appliedStep.z);
        trace.requestedStep = policy::add(trace.requestedStep,
            policy::subtract(desiredPosition, access.playerPosition));
        trace.appliedStep = policy::add(trace.appliedStep, appliedStep);
        trace.maximumTargetLead = std::max(trace.maximumTargetLead, targetLead);
        trace.headClampedFrames += headClamped ? 1u : 0u;
        trace.headSlidePasses += headSlidePasses;
        trace.raycastFailures += headQuerySucceeded ? 0u : 1u;
        trace.discontinuities += discontinuityCount;
        if (sampledCapsule) {
            if (destinationQuery.clearance == CapsuleClearance::Blocked) {
                ++trace.capsuleDestinationBlocked;
            } else if (destinationQuery.clearance == CapsuleClearance::Clear &&
                       pathQuery.clearance == CapsuleClearance::Blocked) {
                ++trace.capsulePathBlocked;
            } else if (pathQuery.clearance == CapsuleClearance::Clear) {
                ++trace.capsuleClear;
            } else {
                ++trace.capsuleQueryUnavailable;
                const CapsuleQueryStage failedStage =
                    destinationQuery.clearance == CapsuleClearance::Unavailable ?
                    destinationQuery.stage :
                    pathQuery.stage;
                if (static_cast<std::uint8_t>(failedStage) >
                    static_cast<std::uint8_t>(
                        trace.deepestCapsuleQueryStage)) {
                    trace.deepestCapsuleQueryStage = failedStage;
                }
            }
        }
    }

    bool ClimbingRuntime::publishTargets(
        const rock_wall_climbing::Frame& snapshot) noexcept
    {
        std::array<rock::api::touch::TouchGrabTarget, 2> targets{};
        const auto makeTarget = [&](
                                    const std::uint64_t targetId,
                                    const rock::api::touch::TouchGrabTargetFlag
                                        allowedHand) {
            rock::api::touch::TouchGrabTarget target{};
            target.targetId = targetId;
            target.targetGeneration = _targetGeneration;
            target.kind = rock::api::touch::TouchGrabKind::FixedAnchor;
            target.flags =
                flag(allowedHand) |
                flag(rock::api::touch::TouchGrabTargetFlag::AllowTwoHands) |
                flag(rock::api::touch::TouchGrabTargetFlag::MatchAnyBody) |
                flag(rock::api::touch::TouchGrabTargetFlag::FallbackOnly) |
                flag(rock::api::touch::TouchGrabTargetFlag::ExcludePowerArmor) |
                flag(rock::api::touch::TouchGrabTargetFlag::MatchStaticMotion) |
                flag(rock::api::touch::TouchGrabTargetFlag::MatchKeyframedMotion);
            if (_config.catchWithHeldGrip) {
                // A grip pressed just before the hand lands, or held through
                // a flight, still takes the surface on contact.
                target.flags |= flag(
                    rock::api::touch::TouchGrabTargetFlag::AcquireWhileGripHeld);
            }
            target.bodyId = INVALID_BODY_ID;
            target.allowedLayerMask =
                policy::CLIMBABLE_WORLD_LAYER_MASK;
            target.leaseFrames = TARGET_LEASE_FRAMES;
            target.worldGeneration = snapshot.sample.worldGeneration;
            target.skeletonGeneration = snapshot.sample.skeletonGeneration;
            target.providerGeneration = snapshot.sample.providerGeneration;
            return target;
        };
        targets[0] = makeTarget(
            RIGHT_TARGET_ID,
            rock::api::touch::TouchGrabTargetFlag::AllowRightHand);
        targets[1] = makeTarget(
            LEFT_TARGET_ID,
            rock::api::touch::TouchGrabTargetFlag::AllowLeftHand);

        const auto result = rockApiClient().publishTargets(
            CLIMBING_SCOPE_TOKEN,
            targets);
        const auto resultValue = static_cast<std::uint32_t>(result);
        if (result != rock::api::Status::Ok) {
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
        if (result != rock::api::Status::Ok &&
            result != rock::api::Status::TargetUnavailable &&
            result != rock::api::Status::NotReady) {
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
        std::array<rock::api::touch::TouchGrabState, 2> states{};
        std::uint32_t count = 0;
        const auto result = rockApiClient().copyStates(
            CLIMBING_SCOPE_TOKEN,
            states,
            count);
        const auto resultValue = static_cast<std::uint32_t>(result);
        if (result != rock::api::Status::Ok || count > states.size()) {
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

            if (state.phase == rock::api::touch::TouchGrabPhase::Invalidated ||
                state.phase == rock::api::touch::TouchGrabPhase::Yielded) {
                invalidated = true;
                continue;
            }
            const bool targetHeld =
                state.phase == rock::api::touch::TouchGrabPhase::Held &&
                state.kind == rock::api::touch::TouchGrabKind::FixedAnchor &&
                state.bodyId != INVALID_BODY_ID;
            if (!targetHeld) {
                continue;
            }

            const auto activeHands = policy::decodeActiveHands(
                state.activeHandMask,
                static_cast<std::uint32_t>(
                    rock::api::touch::TouchGrabHandMask::Right),
                static_cast<std::uint32_t>(
                    rock::api::touch::TouchGrabHandMask::Left));
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
                const policy::Vec3 normal = point(
                    state.contactNormalGame);
                const float normalLengthSquared =
                    policy::lengthSquared(normal);
                hand.normalValid = hasTouchGrabStateFlag(
                                       state.flags,
                                       rock::api::touch::TouchGrabStateFlag::
                                           ContactNormalValid) &&
                                   policy::finite(normal) &&
                                   normalLengthSquared >= 0.25f &&
                                   normalLengthSquared <= 2.25f;
                hand.normal = hand.normalValid ?
                    normal :
                    policy::Vec3{};
            };
            mergeHand(0, activeHands.right);
            mergeHand(1, activeHands.left);
        }

        if (invalidated) {
            return true;
        }

        constexpr std::array<rock::api::Hand, 2> PROVIDER_HANDS{
            rock::api::Hand::Right,
            rock::api::Hand::Left,
        };
        for (std::size_t index = 0; index < observed.size(); ++index) {
            auto& hand = observed[index];
            if (!hand.held) {
                continue;
            }

            rock::api::grab::HandInteractionState interaction{};
            const auto interactionResult =
                rockApiClient().queryHandInteractionState(
                    PROVIDER_HANDS[index],
                    interaction);
            const bool stateMatches =
                interactionResult == rock::api::Status::Ok &&
                interaction.hand == PROVIDER_HANDS[index] &&
                interaction.phase ==
                    rock::api::grab::HandInteractionPhase::Holding &&
                hasHandInteractionFlag(
                    interaction.flags,
                    rock::api::grab::HandInteractionFlag::Valid) &&
                hasHandInteractionFlag(
                    interaction.flags,
                    rock::api::grab::HandInteractionFlag::TouchGrab) &&
                hasHandInteractionFlag(
                    interaction.flags,
                    rock::api::grab::HandInteractionFlag::FixedSurfaceLatch) &&
                hasHandInteractionFlag(
                    interaction.flags,
                    rock::api::grab::HandInteractionFlag::SurfaceAnchorValid) &&
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

    bool ClimbingRuntime::tryResolveHeadMotion(
        const rock_wall_climbing::Frame& snapshot,
        const policy::Vec3 currentPlayerPosition,
        const policy::Vec3 desiredPlayerPosition,
        policy::Vec3& resolvedPlayerPosition,
        bool& wasConstrained,
        std::uint32_t& slidePasses) noexcept
    {
        resolvedPlayerPosition = currentPlayerPosition;
        wasConstrained = false;
        slidePasses = 0;
        const policy::Vec3 requestedStep = policy::subtract(
            desiredPlayerPosition,
            currentPlayerPosition);
        if (!policy::finite(requestedStep)) {
            return false;
        }
        if (policy::lengthSquared(requestedStep) <= 1.0e-8f) {
            return true;
        }

        const policy::Vec3 hmdPosition =
            translation(snapshot.hmdTransform);
        policy::Vec3 appliedStep{};
        policy::Vec3 remainingStep = requestedStep;
        for (std::uint32_t pass = 0;
             pass < MAXIMUM_HEAD_MOTION_PASSES;
             ++pass) {
            const float distance = policy::length(remainingStep);
            if (distance <= 1.0e-4f) {
                remainingStep = {};
                break;
            }
            const policy::Vec3 direction =
                policy::divide(remainingStep, distance);

            rock::api::collision::WorldRaycastRequest request{};
            request.startGame = providerPoint(
                policy::add(hmdPosition, appliedStep));
            request.directionGame = providerPoint(direction);
            request.maxDistanceGame =
                distance + _config.headClearanceGameUnits;
            request.worldGeneration = snapshot.sample.worldGeneration;
            request.skeletonGeneration = snapshot.sample.skeletonGeneration;
            request.providerGeneration = snapshot.sample.providerGeneration;

            rock::api::collision::WorldRaycastResult result{};
            if (rockApiClient().queryWorldRaycast(request, result) !=
                rock::api::Status::Ok) {
                return false;
            }

            const bool hit = result.hit != 0;
            if (!hit) {
                appliedStep = policy::add(appliedStep, remainingStep);
                remainingStep = {};
                break;
            }
            if (!std::isfinite(result.hitDistanceGame) ||
                result.hitDistanceGame < 0.0f) {
                return false;
            }

            const float allowedDistance = std::clamp(
                result.hitDistanceGame - _config.headClearanceGameUnits,
                0.0f,
                distance);
            const policy::Vec3 traveled =
                policy::scale(direction, allowedDistance);
            appliedStep = policy::add(appliedStep, traveled);
            remainingStep = policy::subtract(remainingStep, traveled);
            wasConstrained = true;
            if (allowedDistance + 1.0e-4f >= distance) {
                remainingStep = {};
                break;
            }

            const policy::Vec3 hitNormal = point(result.hitNormalGame);
            policy::Vec3 slidingMotion{};
            if (!hasWorldRaycastResultFlag(
                    result.flags,
                    rock::api::collision::WorldRaycastResultFlag::NormalValid) ||
                !policy::slideAlongCollisionPlane(
                    remainingStep,
                    hitNormal,
                    direction,
                    slidingMotion)) {
                remainingStep = {};
                break;
            }
            if (policy::lengthSquared(slidingMotion) + 1.0e-6f >=
                policy::lengthSquared(remainingStep)) {
                remainingStep = {};
                break;
            }
            remainingStep = slidingMotion;
            ++slidePasses;
        }

        resolvedPlayerPosition = policy::add(
            currentPlayerPosition,
            appliedStep);
        wasConstrained = wasConstrained ||
            policy::lengthSquared(policy::subtract(
                appliedStep,
                requestedStep)) > 1.0e-6f;
        return validCoordinate(resolvedPlayerPosition);
    }

    bool ClimbingRuntime::castSurfaceRay(
        const rock_wall_climbing::Frame& snapshot,
        const policy::Vec3 start,
        const policy::Vec3 direction,
        const float distance,
        bool& hit,
        float& hitDistance) noexcept
    {
        hit = false;
        hitDistance = 0.0f;
        if (!validCoordinate(start) || !policy::finite(direction) ||
            !std::isfinite(distance) || distance <= 0.0f) {
            return false;
        }

        rock::api::collision::WorldRaycastRequest request{};
        request.startGame = providerPoint(start);
        request.directionGame = providerPoint(direction);
        request.maxDistanceGame = distance;
        request.worldGeneration = snapshot.sample.worldGeneration;
        request.skeletonGeneration = snapshot.sample.skeletonGeneration;
        request.providerGeneration = snapshot.sample.providerGeneration;
        // Solid world surfaces only: the player's own body, actors and held
        // objects are never ground or a ceiling.
        request.filter = rock::api::collision::WorldRaycastFilter::Surfaces;

        rock::api::collision::WorldRaycastResult result{};
        if (rockApiClient().queryWorldRaycast(request, result) !=
            rock::api::Status::Ok) {
            return false;
        }
        if (result.hit == 0) {
            return true;
        }
        if (!std::isfinite(result.hitDistanceGame) ||
            result.hitDistanceGame < 0.0f) {
            return false;
        }
        hit = true;
        hitDistance = result.hitDistanceGame;
        return true;
    }

    bool ClimbingRuntime::findGroundUnderHead(
        const rock_wall_climbing::Frame& snapshot,
        const policy::Vec3 headPosition,
        const float feetHeight,
        float& groundHeight) noexcept
    {
        groundHeight = 0.0f;
        const float headHeight = std::max(
            headPosition.z - feetHeight,
            MINIMUM_HEAD_HEIGHT);
        bool hit = false;
        float hitDistance = 0.0f;
        if (!castSurfaceRay(
                snapshot,
                headPosition,
                { 0.0f, 0.0f, -1.0f },
                headHeight + GROUND_PROBE_BELOW_FEET,
                hit,
                hitDistance) ||
            !hit) {
            return false;
        }
        groundHeight = headPosition.z - hitDistance;
        return true;
    }

    void ClimbingRuntime::releaseToAir(
        const rock_wall_climbing::Frame& snapshot,
        const PlayerAccess& access)
    {
        std::array<policy::VelocitySample, VelocityHistory::CAPACITY>
            samples{};
        const auto chronological =
            _velocityHistory.copyChronological(samples);
        const policy::Vec3 launch = policy::calculateLaunchVelocity(
            chronological,
            _config.launch);

        logMotionTrace("release");
        _climbing = false;
        _targetPositionValid = false;
        _targetPlayerPosition = {};
        _hands = {};
        _velocityHistory.clear();

        if (!trySetVelocity(access, {})) {
            logger::error(
                "Player velocity cancellation failed on release; failing closed.");
            finishClimb(nullptr, "velocity-write");
            return;
        }

        const policy::Vec3 head = translation(snapshot.hmdTransform);
        if (policy::lengthSquared(launch) > 1.0e-6f) {
            _air = {};
            _air.phase = AirPhase::Flight;
            _air.velocity = launch;
            _air.launchVelocity = launch;
            _air.startPosition = access.playerPosition;
            logger::info(
                "Released into flight velocity=({:.1f},{:.1f},{:.1f}) speed={:.1f} samples={} gravity={:.0f}.",
                launch.x,
                launch.y,
                launch.z,
                policy::length(launch),
                chronological.size(),
                _config.launch.gravity);
            return;
        }

        float groundHeight = 0.0f;
        const bool groundFound = findGroundUnderHead(
            snapshot,
            head,
            access.playerPosition.z,
            groundHeight);
        if (policy::decideLanding(
                groundFound,
                groundHeight,
                access.playerPosition.z,
                0.0f) == policy::LandingDecision::Mantle &&
            tryBeginMantle(
                snapshot,
                access.playerPosition,
                head,
                groundHeight)) {
            return;
        }
        finishClimb(&access, "final-release");
    }

    bool ClimbingRuntime::tryBeginMantle(
        const rock_wall_climbing::Frame& snapshot,
        const policy::Vec3 playerPosition,
        const policy::Vec3 headPosition,
        const float groundHeight)
    {
        float rise = groundHeight - playerPosition.z;
        if (!std::isfinite(rise) || rise < policy::MANTLE_MINIMUM_RISE) {
            return false;
        }

        // The head rises as far as the feet do; lift only through free room.
        bool ceilingHit = false;
        float ceilingDistance = 0.0f;
        if (!castSurfaceRay(
                snapshot,
                headPosition,
                { 0.0f, 0.0f, 1.0f },
                rise + _config.headClearanceGameUnits,
                ceilingHit,
                ceilingDistance)) {
            return false;
        }
        if (ceilingHit) {
            rise = std::min(
                rise,
                ceilingDistance - _config.headClearanceGameUnits);
        }
        if (rise < policy::MANTLE_MINIMUM_RISE) {
            logger::info(
                "Mantle skipped: no head room above (ceiling {:.1f} game units over the head).",
                ceilingDistance);
            return false;
        }

        policy::Vec3 target = playerPosition;
        target.z += rise;
        const policy::Vec3 launchVelocity = _air.launchVelocity;
        const policy::Vec3 startPosition = _air.phase == AirPhase::Flight ?
            _air.startPosition :
            playerPosition;
        const float seconds = _air.seconds;
        _air = {};
        _air.phase = AirPhase::Mantle;
        _air.mantle = policy::makeMantlePath(playerPosition, target);
        _air.launchVelocity = launchVelocity;
        _air.startPosition = startPosition;
        _air.seconds = seconds;
        logger::info(
            "Mantle started rise={:.1f} seconds={:.2f} ceiling={}.",
            rise,
            _air.mantle.durationSeconds,
            ceilingHit ? "limited" : "clear");
        return true;
    }

    void ClimbingRuntime::updateAir(
        const rock_wall_climbing::Frame& snapshot,
        const PlayerAccess& access)
    {
        _air.seconds += snapshot.deltaSeconds;
        ++_air.frames;

        if (!suspendGravity(access)) {
            logger::error(
                "Could not keep the player gravity suspended in the air; failing closed.");
            finishClimb(&access, "gravity-write");
            return;
        }
        if (!trySetVelocity(access, {})) {
            logger::error(
                "Player velocity cancellation failed in the air; failing closed.");
            finishClimb(nullptr, "velocity-write");
            return;
        }

        if (_air.phase == AirPhase::Mantle) {
            _air.mantleProgress +=
                snapshot.deltaSeconds / _air.mantle.durationSeconds;
            const bool complete = _air.mantleProgress >= 1.0f;
            const policy::Vec3 position = complete ?
                _air.mantle.target :
                policy::evaluateMantlePath(_air.mantle, _air.mantleProgress);
            if (!trySetPlayerPosition(access.player, position)) {
                logger::error(
                    "Player SetPosition failed during the mantle; failing closed.");
                finishClimb(nullptr, "position-write");
                return;
            }
            if (complete) {
                finishClimb(&access, "mantle-complete");
            }
            return;
        }

        if (_air.seconds > MAXIMUM_FLIGHT_SECONDS) {
            logger::warn(
                "Flight exceeded {:.0f} s without landing; returning the player to the game's own fall.",
                MAXIMUM_FLIGHT_SECONDS);
            finishClimb(&access, "flight-timeout");
            return;
        }

        const auto step = policy::integrateFlight(
            _air.velocity,
            _config.launch.gravity,
            snapshot.deltaSeconds);
        const policy::Vec3 desiredPosition = policy::add(
            access.playerPosition,
            step.displacement);
        policy::Vec3 position{};
        bool constrained = false;
        std::uint32_t slidePasses = 0;
        if (!tryResolveHeadMotion(
                snapshot,
                access.playerPosition,
                desiredPosition,
                position,
                constrained,
                slidePasses)) {
            logger::warn(
                "ROCK head-motion raycast failed in flight; returning the player to the game's own fall.");
            finishClimb(&access, "flight-raycast");
            return;
        }

        const policy::Vec3 appliedStep = policy::subtract(
            position,
            access.playerPosition);
        // What a wall or ceiling stopped is gone; what slid along it stays.
        policy::Vec3 velocity = step.velocity;
        if (constrained) {
            velocity = policy::divide(appliedStep, snapshot.deltaSeconds);
            ++_air.constrainedFrames;
        }

        const policy::Vec3 head = policy::add(
            translation(snapshot.hmdTransform),
            appliedStep);
        float groundHeight = 0.0f;
        const bool groundFound = findGroundUnderHead(
            snapshot,
            head,
            position.z,
            groundHeight);
        const auto landing = policy::decideLanding(
            groundFound,
            groundHeight,
            position.z,
            velocity.z);
        if (landing == policy::LandingDecision::Land) {
            position.z = std::max(position.z, groundHeight);
        }

        if (policy::lengthSquared(policy::subtract(
                position,
                access.playerPosition)) > 1.0e-6f &&
            !trySetPlayerPosition(access.player, position)) {
            logger::error(
                "Player SetPosition failed in flight; failing closed.");
            finishClimb(nullptr, "position-write");
            return;
        }
        _air.velocity = velocity;

        if (landing == policy::LandingDecision::Land) {
            finishClimb(&access, "landed");
        } else if (landing == policy::LandingDecision::Mantle &&
                   !tryBeginMantle(snapshot, position, head, groundHeight)) {
            finishClimb(&access, "landed-without-room");
        }
    }

    void ClimbingRuntime::endAir(const char* reason)
    {
        if (_air.phase == AirPhase::None) {
            return;
        }
        logger::info(
            "{} ended reason={} seconds={:.2f} frames={} constrainedFrames={} launch=({:.1f},{:.1f},{:.1f}) start=({:.1f},{:.1f},{:.1f}) velocity=({:.1f},{:.1f},{:.1f}).",
            _air.phase == AirPhase::Flight ? "Flight" : "Mantle",
            reason ? reason : "unknown",
            _air.seconds,
            _air.frames,
            _air.constrainedFrames,
            _air.launchVelocity.x,
            _air.launchVelocity.y,
            _air.launchVelocity.z,
            _air.startPosition.x,
            _air.startPosition.y,
            _air.startPosition.z,
            _air.velocity.x,
            _air.velocity.y,
            _air.velocity.z);
        _air = {};
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

    ClimbingRuntime::CapsuleQueryResult
        ClimbingRuntime::queryNativeCapsuleClearance(
            const ControllerAccess& access,
            const policy::Vec3 destinationGame,
            const bool sweepPath) noexcept
    {
        CapsuleQueryResult result{};
        if (!access.controller || !validCoordinate(destinationGame) ||
            !REL::Module::IsVR() ||
            REL::Module::get().version() != F4SE::RUNTIME_VR_1_2_72) {
            return result;
        }

        const std::uintptr_t moduleBase = REL::Module::get().base();
        const std::uintptr_t queryAddress =
            moduleBase + CAPSULE_CLEARANCE_QUERY_RVA;
        const RE::NiPoint3 destination{
            destinationGame.x,
            destinationGame.y,
            destinationGame.z,
        };
        __try {
            result.stage = CapsuleQueryStage::QueryGuard;
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(
                queryAddress);
            for (std::size_t index = 0;
                 index < CAPSULE_CLEARANCE_QUERY_PREFIX.size(); ++index) {
                if (bytes[index] != CAPSULE_CLEARANCE_QUERY_PREFIX[index]) {
                    return result;
                }
            }

            result.stage = CapsuleQueryStage::Shape;
            const auto* controllerBytes =
                reinterpret_cast<const std::uint8_t*>(access.controller);
            const auto shapeIndex =
                *reinterpret_cast<const std::int32_t*>(
                    controllerBytes + 0x354);
            if (shapeIndex < 0 || shapeIndex > 1 ||
                !*reinterpret_cast<void* const*>(
                    controllerBytes + 0x360 + shapeIndex * sizeof(void*)) ||
                !*reinterpret_cast<void* const*>(
                    controllerBytes + 0x470)) {
                return result;
            }
            result.stage = CapsuleQueryStage::Implementation;
            const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(
                access.controller);
            const std::uintptr_t expectedGetWorldRva =
                access.velocityImplementation ==
                        controller_policy::VelocityImplementation::Proxy ?
                    PROXY_GET_WORLD_RVA :
                access.velocityImplementation ==
                        controller_policy::VelocityImplementation::RigidBody ?
                    RIGID_GET_WORLD_RVA :
                    0;
            const std::uintptr_t getWorldAddress =
                vtable[0x210 / sizeof(void*)];
            if (expectedGetWorldRva == 0 ||
                getWorldAddress != moduleBase + expectedGetWorldRva) {
                return result;
            }
            result.stage = CapsuleQueryStage::WorldGuard;
            const auto* getWorldBytes =
                reinterpret_cast<const std::uint8_t*>(getWorldAddress);
            bool worldAccessorValid = true;
            for (std::size_t index = 0; index < GET_WORLD_PREFIX.size(); ++index) {
                if (getWorldBytes[index] != GET_WORLD_PREFIX[index]) {
                    worldAccessorValid = false;
                    break;
                }
            }
            if (!worldAccessorValid &&
                access.velocityImplementation ==
                    controller_policy::VelocityImplementation::Proxy) {
                // ROCK's verified teardown-safety entry hook replaces the
                // original proxy accessor bytes with FF 25 + an absolute
                // target. Accept only that exact patch aimed at executable
                // memory belonging to the loaded ROCK.dll.
                bool patchMatches = true;
                for (std::size_t index = 0; index < ENTRY_JUMP_PREFIX.size();
                     ++index) {
                    if (getWorldBytes[index] != ENTRY_JUMP_PREFIX[index]) {
                        patchMatches = false;
                        break;
                    }
                }
                std::uintptr_t hookTarget = 0;
                if (patchMatches) {
                    std::memcpy(&hookTarget, getWorldBytes + 6,
                        sizeof(hookTarget));
                }
                const HMODULE rockModule = GetModuleHandleW(L"ROCK.dll");
                MEMORY_BASIC_INFORMATION memory{};
                const bool targetOwnedByRock = patchMatches &&
                    rockModule && hookTarget != 0 &&
                    VirtualQuery(reinterpret_cast<const void*>(hookTarget),
                        &memory, sizeof(memory)) == sizeof(memory) &&
                    memory.AllocationBase == rockModule &&
                    memory.State == MEM_COMMIT;
                const DWORD protection = memory.Protect & 0xFFu;
                const bool executable = protection == PAGE_EXECUTE ||
                    protection == PAGE_EXECUTE_READ ||
                    protection == PAGE_EXECUTE_READWRITE ||
                    protection == PAGE_EXECUTE_WRITECOPY;
                worldAccessorValid = targetOwnedByRock && executable;
            }
            if (!worldAccessorValid) {
                return result;
            }
            using GetWorldFunction = void* (*)(RE::bhkCharacterController*);
            const auto getWorld = reinterpret_cast<GetWorldFunction>(
                getWorldAddress);
            void* world = getWorld(access.controller);
            result.stage = CapsuleQueryStage::World;
            if (!world ||
                !*reinterpret_cast<void* const*>(
                    reinterpret_cast<const std::uint8_t*>(world) + 0x60)) {
                return result;
            }

            // FO4VR's own candidate-position query at 0x141E21030 acquires
            // the current controller shape and checks destination overlap;
            // the optional final argument also checks the swept path. These
            // flags match a native four-layer-category caller at 0x140DF3283.
            using QueryFunction = bool (*)(
                RE::bhkCharacterController*,
                const RE::NiPoint3*,
                const std::uint8_t*,
                std::uint8_t);
            const auto query = reinterpret_cast<QueryFunction>(queryAddress);
            result.clearance = query(
                access.controller,
                &destination,
                CAPSULE_COLLISION_FLAGS.data(),
                sweepPath ? 1u : 0u) ?
                CapsuleClearance::Clear : CapsuleClearance::Blocked;
            result.stage = CapsuleQueryStage::Complete;
            return result;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return result;
        }
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
        const char* reason) noexcept
    {
        if (!_climbing && !_gravityOwned) {
            resetLocalState();
            return;
        }

        restoreGravity(currentAccess);
        if (currentAccess) {
            static_cast<void>(trySetVelocity(*currentAccess, {}));
        }

        try {
            if (_climbing) {
                logMotionTrace(reason);
            }
            endAir(reason);
            logger::info(
                "Climb ended reason={}.",
                reason ? reason : "unknown");
        } catch (...) {
        }
        resetLocalState();
    }

    void ClimbingRuntime::logMotionTrace(const char* reason)
    {
        const auto& trace = _motionTrace;
        if (trace.frames == 0) {
            return;
        }
        const float frameCount = static_cast<float>(trace.frames);
        logger::info(
            "Climb motion reason={} frame={} hands=0x{:X} frames={} observedFrames={} seconds={:.3f} "
            "rightPull=({:.2f},{:.2f},{:.2f}) leftPull=({:.2f},{:.2f},{:.2f}) "
            "rightCarry=({:.2f},{:.2f},{:.2f}) leftCarry=({:.2f},{:.2f},{:.2f}) meanWeight=({:.3f},{:.3f}) "
            "blendedPull=({:.2f},{:.2f},{:.2f}) requested=({:.2f},{:.2f},{:.2f}) "
            "submitted=({:.2f},{:.2f},{:.2f}) observed=({:.2f},{:.2f},{:.2f}) external=({:.2f},{:.2f},{:.2f}) "
            "upward(right/left/blended/requested/submitted)=({:.2f},{:.2f},{:.2f},{:.2f},{:.2f}) "
            "maxLead={:.2f} headClamped={} headSlides={} rayFailures={} discontinuities={} "
            "capsule(clear/destination/path/unavailable)=({}/{}/{}/{}) capsuleStage={}.",
            reason ? reason : "unknown", _lastFrameIndex, trace.handMask,
            trace.frames, trace.observedFrames, trace.seconds,
            trace.pull[0].x, trace.pull[0].y, trace.pull[0].z,
            trace.pull[1].x, trace.pull[1].y, trace.pull[1].z,
            trace.carry[0].x, trace.carry[0].y, trace.carry[0].z,
            trace.carry[1].x, trace.carry[1].y, trace.carry[1].z,
            trace.weightSum[0] / frameCount, trace.weightSum[1] / frameCount,
            trace.blendedPull.x, trace.blendedPull.y, trace.blendedPull.z,
            trace.requestedStep.x, trace.requestedStep.y, trace.requestedStep.z,
            trace.appliedStep.x, trace.appliedStep.y, trace.appliedStep.z,
            trace.observedStep.x, trace.observedStep.y, trace.observedStep.z,
            trace.externalStep.x, trace.externalStep.y, trace.externalStep.z,
            trace.upwardPull[0], trace.upwardPull[1], trace.upwardBlendedPull,
            trace.upwardRequested, trace.upwardSubmitted,
            trace.maximumTargetLead, trace.headClampedFrames,
            trace.headSlidePasses, trace.raycastFailures,
            trace.discontinuities, trace.capsuleClear,
            trace.capsuleDestinationBlocked,
            trace.capsulePathBlocked, trace.capsuleQueryUnavailable,
            static_cast<std::uint32_t>(trace.deepestCapsuleQueryStage));
        _motionTrace = {};
    }

    void ClimbingRuntime::resetLocalState() noexcept
    {
        _climbing = false;
        _air = {};
        _targetPositionValid = false;
        _targetPlayerPosition = {};
        _hands = {};
        _velocityHistory.clear();
        _motionTrace = {};
        _motionPositionValid = false;
        _previousPlayerPosition = {};
        _previousSubmittedPosition = {};
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
