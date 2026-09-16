#pragma once

#include "api/ROCKProviderApi.h"
#include "runtime/ClimbingPolicy.h"
#include "runtime/ControllerPolicy.h"
#include "runtime/VelocityHistory.h"
#include "support/Config.h"

#include <array>
#include <cstdint>

namespace RE
{
    class PlayerCharacter;
    class bhkCharacterController;
}

namespace rock_wall_climbing
{
    class ClimbingRuntime
    {
    public:
        static ClimbingRuntime& get() noexcept;

        [[nodiscard]] bool connect() noexcept;
        void beginSession() noexcept;
        void endSession() noexcept;
        void shutdown() noexcept;

    private:
        enum class ControllerResolveStage : std::uint8_t
        {
            None,
            Player,
            CurrentProcess,
            MiddleHigh,
            Controller,
            Vtable,
            VelocityDispatch,
            Gravity,
            Position,
            Complete,
        };

        struct ControllerAccess
        {
            RE::PlayerCharacter* player{ nullptr };
            RE::bhkCharacterController* controller{ nullptr };
            std::uintptr_t controllerIdentity{ 0 };
            std::uintptr_t controllerVtable{ 0 };
            std::uintptr_t velocityFunction{ 0 };
            controller_policy::VelocityImplementation
                velocityImplementation{
                    controller_policy::VelocityImplementation::Unknown
                };
            float gravity{ 0.0f };
        };

        struct PlayerAccess : ControllerAccess
        {
            policy::Vec3 playerPosition{};
        };

        struct HandState
        {
            bool held{ false };
            bool baselineValid{ false };
            std::uint32_t bodyId{ 0x7FFF'FFFFu };
            float blendWeight{ 0.0f };
            policy::Vec3 previousHandOffset{};
            bool previousAnchorValid{ false };
            policy::Vec3 previousAnchor{};
        };

        struct ObservedHand
        {
            bool held{ false };
            std::uint32_t bodyId{ 0x7FFF'FFFFu };
            bool anchorValid{ false };
            policy::Vec3 anchor{};
            bool normalValid{ false };
            policy::Vec3 normal{};
        };

        enum class CapsuleClearance : std::uint8_t
        {
            Clear,
            Blocked,
            Unavailable,
        };

        enum class CapsuleQueryStage : std::uint8_t
        {
            Preflight,
            QueryGuard,
            Shape,
            Implementation,
            WorldGuard,
            World,
            Complete,
        };

        struct CapsuleQueryResult
        {
            CapsuleClearance clearance{ CapsuleClearance::Unavailable };
            CapsuleQueryStage stage{ CapsuleQueryStage::Preflight };
        };

        // Fixed-size totals for one held-hand set. Normal logging emits only
        // at handoffs and teardown; detailed telemetry also emits every 0.5 s.
        struct MotionTrace
        {
            std::uint32_t frames{ 0 };
            std::uint32_t observedFrames{ 0 };
            std::uint32_t handMask{ 0 };
            float seconds{ 0.0f };
            std::array<policy::Vec3, 2> pull{};
            std::array<float, 2> upwardPull{};
            std::array<policy::Vec3, 2> carry{};
            std::array<float, 2> weightSum{};
            policy::Vec3 blendedPull{};
            float upwardBlendedPull{ 0.0f };
            float upwardRequested{ 0.0f };
            float upwardSubmitted{ 0.0f };
            policy::Vec3 requestedStep{};
            policy::Vec3 appliedStep{};
            policy::Vec3 observedStep{};
            policy::Vec3 externalStep{};
            float maximumTargetLead{ 0.0f };
            std::uint32_t headClampedFrames{ 0 };
            std::uint32_t headSlidePasses{ 0 };
            std::uint32_t raycastFailures{ 0 };
            std::uint32_t discontinuities{ 0 };
            std::uint32_t capsuleClear{ 0 };
            std::uint32_t capsuleDestinationBlocked{ 0 };
            std::uint32_t capsulePathBlocked{ 0 };
            std::uint32_t capsuleQueryUnavailable{ 0 };
            CapsuleQueryStage deepestCapsuleQueryStage{
                CapsuleQueryStage::Preflight
            };
        };

        struct LaunchObservation
        {
            bool active{ false };
            bool firstVelocityValid{ false };
            bool nativeJumpRequested{ false };
            std::uint64_t startFrameIndex{ 0 };
            std::uint32_t frames{ 0 };
            std::uint32_t validFrames{ 0 };
            std::uint32_t supportedFrames{ 0 };
            std::uint32_t lastResult{ 0 };
            policy::Vec3 requestedVelocity{};
            policy::Vec3 firstVelocity{};
            policy::Vec3 lastVelocity{};
        };

        ClimbingRuntime() = default;

        static void ROCK_PROVIDER_CALL onRockFrame(
            const rock::provider::RockProviderFrameSnapshot* snapshot,
            void* userData) noexcept;

        void update(
            const rock::provider::RockProviderFrameSnapshot& snapshot);
        [[nodiscard]] std::uint32_t snapshotBlockers(
            const rock::provider::RockProviderFrameSnapshot& snapshot)
            const noexcept;
        void observeBlockers(std::uint32_t blockers) noexcept;

        [[nodiscard]] bool publishTargets(
            const rock::provider::RockProviderFrameSnapshot& snapshot)
            noexcept;
        void clearTargets() noexcept;
        [[nodiscard]] bool observeHands(
            std::array<ObservedHand, 2>& observed,
            bool& invalidated) noexcept;

        [[nodiscard]] bool tryResolveHeadMotion(
            const rock::provider::RockProviderFrameSnapshot& snapshot,
            policy::Vec3 currentPlayerPosition,
            policy::Vec3 desiredPlayerPosition,
            policy::Vec3& resolvedPlayerPosition,
            bool& wasConstrained,
            std::uint32_t& slidePasses) noexcept;
        void observeLaunchReadback(
            std::uint64_t frameIndex,
            rock::provider::RockProviderResultV1 result,
            const rock::provider::RockProviderPlayerControllerStateV1& state,
            bool stateValid) noexcept;
        void finishLaunchObservation(const char* reason) noexcept;

        [[nodiscard]] static bool tryResolveControllerAccess(
            ControllerAccess& access,
            ControllerResolveStage& deepestStage) noexcept;
        [[nodiscard]] static bool tryResolvePlayerAccess(
            PlayerAccess& access,
            ControllerResolveStage& deepestStage) noexcept;
        [[nodiscard]] static bool tryWriteGravity(
            RE::bhkCharacterController* controller,
            float value) noexcept;
        [[nodiscard]] bool trySetVelocity(
            const ControllerAccess& access,
            policy::Vec3 velocityHavok) noexcept;
        [[nodiscard]] static bool trySetPlayerPosition(
            RE::PlayerCharacter* player,
            policy::Vec3 positionGame) noexcept;
        [[nodiscard]] static CapsuleQueryResult queryNativeCapsuleClearance(
            const ControllerAccess& access,
            policy::Vec3 destinationGame,
            bool sweepPath) noexcept;

        [[nodiscard]] bool suspendGravity(
            const ControllerAccess& access) noexcept;
        void restoreGravity(const ControllerAccess* currentAccess) noexcept;
        void clearGravityOwnership() noexcept;
        void finishClimb(
            const PlayerAccess* currentAccess,
            float gameToHavokScale,
            bool allowLaunch,
            const char* reason) noexcept;
        void resetLocalState() noexcept;
        void logMotionTrace(const char* reason);

        [[nodiscard]] static const char* controllerStageName(
            ControllerResolveStage stage) noexcept;
        [[nodiscard]] std::uint32_t nextTargetGeneration() noexcept;

        Config _config{};
        bool _sessionActive{ false };
        bool _connected{ false };
        bool _targetsPublished{ false };
        bool _climbing{ false };
        bool _targetPositionValid{ false };
        policy::Vec3 _targetPlayerPosition{};
        std::array<HandState, 2> _hands{};
        VelocityHistory _velocityHistory{};

        LaunchObservation _launchObservation{};

        bool _gravityOwned{ false };
        bool _gravityRestoreDeferredLogged{ false };
        float _savedGravity{ 0.0f };
        std::uintptr_t _gravityControllerIdentity{ 0 };
        bool _velocityDispatchLogged{ false };

        std::uint32_t _worldGeneration{ 0 };
        std::uint32_t _skeletonGeneration{ 0 };
        std::uint32_t _providerGeneration{ 0 };
        std::uint32_t _targetGeneration{ 1 };
        std::uint64_t _lastFrameIndex{ 0 };

        std::uint32_t _lastBlockers{ UINT32_MAX };
        std::uint32_t _lastPublishResult{ UINT32_MAX };
        std::uint32_t _lastStateResult{ UINT32_MAX };
        ControllerResolveStage _lastControllerFailure{
            ControllerResolveStage::Complete
        };
        MotionTrace _motionTrace{};
        bool _motionPositionValid{ false };
        policy::Vec3 _previousPlayerPosition{};
        policy::Vec3 _previousSubmittedPosition{};
    };
}
