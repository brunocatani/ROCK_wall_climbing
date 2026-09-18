#pragma once

#include "api/RockTypes.h"
#include <ROCK/Client.h>

#include <cstdint>
#include <span>

namespace rock_wall_climbing
{
    class RockApiClient
    {
    public:
        [[nodiscard]] bool initialize() noexcept;
        void shutdown() noexcept;

        [[nodiscard]] bool ready() const noexcept;
        [[nodiscard]] std::uint64_t ownerToken() const noexcept;

        [[nodiscard]] bool registerFrameCallback(
            rock_wall_climbing::FrameCallback callback,
            void* userData) noexcept;

        [[nodiscard]] rock::api::Status publishTargets(
            std::uint64_t scopeToken,
            std::span<const rock::api::touch::TouchGrabTargetV1>
                targets) const noexcept;

        [[nodiscard]] rock::api::Status clearTargets(
            std::uint64_t scopeToken) const noexcept;

        [[nodiscard]] rock::api::Status copyStates(
            std::uint64_t scopeToken,
            std::span<rock::api::touch::TouchGrabStateV1> states,
            std::uint32_t& outCount) const noexcept;

        [[nodiscard]] rock::api::Status
            queryHandInteractionState(
                rock::api::Hand hand,
                rock::api::grab::HandInteractionStateV1& state)
                const noexcept;

        [[nodiscard]] rock::api::Status queryWorldRaycast(
            const rock::api::collision::WorldRaycastRequestV1& request,
            rock::api::collision::WorldRaycastResultV1& result)
            const noexcept;

        [[nodiscard]] rock::api::Status
            queryPlayerControllerState(
                rock::api::playercontroller::PlayerControllerStateV1& state)
                const noexcept;

        [[nodiscard]] rock::api::Status
            requestPlayerControllerJump(
                const rock::api::playercontroller::PlayerControllerJumpRequestV1& request)
                const noexcept;

    private:
        rock::api::Client _client;
        const rock::api::core::ApiV1* _core{};
        const rock::api::hands::ApiV1* _hands{};
        const rock::api::grab::ApiV1* _grab{};
        const rock::api::touch::ApiV1* _touch{};
        const rock::api::collision::ApiV1* _collision{};
        const rock::api::playercontroller::ApiV1* _controller{};
        FrameCallback _callback{};
        void* _callbackData{};
        static void ROCK_CALL receiveFrame(const rock::api::core::SnapshotV1*,void*);

        std::uint64_t _ownerToken{ 0 };
        std::uint64_t _frameCallbackToken{ 0 };
    };

    [[nodiscard]] RockApiClient& rockApiClient() noexcept;
}
