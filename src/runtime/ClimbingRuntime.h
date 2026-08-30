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

        struct LedgeCandidate
        {
            bool valid{ false };
            policy::Vec3 floorPoint{};
            policy::Vec3 floorNormal{};
            policy::Vec3 outwardHorizontal{};
            float riseGameUnits{ 0.0f };
            float jumpHeightGameUnits{ 0.0f };
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

        [[nodiscard]] bool tryClampHeadMotion(
            const rock::provider::RockProviderFrameSnapshot& snapshot,
            policy::Vec3 currentPlayerPosition,
            policy::Vec3 desiredPlayerPosition,
            policy::Vec3& clampedPlayerPosition,
            bool& wasClamped) noexcept;
        [[nodiscard]] bool tryFindLedgeCandidate(
            const rock::provider::RockProviderFrameSnapshot& snapshot,
            const std::array<ObservedHand, 2>& observed,
            const rock::provider::RockProviderPlayerControllerStateV1&
                controllerState,
            policy::Vec3 playerPosition,
            LedgeCandidate& candidate) noexcept;
        [[nodiscard]] bool tryPerformClimbJump(
            const rock::provider::RockProviderFrameSnapshot& snapshot,
            const PlayerAccess& access,
            const LedgeCandidate& candidate) noexcept;
        [[nodiscard]] bool gripsReleasedForRearm() const noexcept;

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
        bool _lastSafePlayerPositionValid{ false };
        policy::Vec3 _lastSafePlayerPosition{};
        std::array<HandState, 2> _hands{};
        VelocityHistory _velocityHistory{};

        bool _targetsRequireGripRelease{ false };
        bool _penetrationRecoveryLogged{ false };
        bool _jumpInputPrimed{ false };
        std::uint64_t _lastJumpPressSequence{ 0 };

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
        std::uint32_t _lastControllerStateResult{ UINT32_MAX };
        ControllerResolveStage _lastControllerFailure{
            ControllerResolveStage::Complete
        };
        std::uint32_t _telemetryFrames{ 0 };
    };
}
