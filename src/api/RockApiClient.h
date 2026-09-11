#pragma once

#include "api/ROCKProviderApi.h"

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
            rock::provider::RockProviderFrameCallback callback,
            void* userData) noexcept;

        [[nodiscard]] rock::provider::RockProviderResultV1 publishTargets(
            std::uint64_t scopeToken,
            std::span<const rock::provider::RockProviderTouchGrabTargetV1>
                targets) const noexcept;

        [[nodiscard]] rock::provider::RockProviderResultV1 clearTargets(
            std::uint64_t scopeToken) const noexcept;

        [[nodiscard]] rock::provider::RockProviderResultV1 copyStates(
            std::uint64_t scopeToken,
            std::span<rock::provider::RockProviderTouchGrabStateV1> states,
            std::uint32_t& outCount) const noexcept;

        [[nodiscard]] rock::provider::RockProviderResultV1
            queryHandInteractionState(
                rock::provider::RockProviderHand hand,
                rock::provider::RockProviderHandInteractionStateV1& state)
                const noexcept;

        [[nodiscard]] rock::provider::RockProviderResultV1 queryWorldRaycast(
            const rock::provider::RockProviderWorldRaycastRequestV1& request,
            rock::provider::RockProviderWorldRaycastResultV1& result)
            const noexcept;

        [[nodiscard]] rock::provider::RockProviderResultV1
            queryPlayerControllerState(
                std::uint32_t queryFlags,
                rock::provider::RockProviderPlayerControllerStateV1& state)
                const noexcept;

        [[nodiscard]] rock::provider::RockProviderResultV1
            requestPlayerControllerJump(
                const rock::provider::
                    RockProviderPlayerControllerJumpRequestV1& request)
                const noexcept;

    private:
        const rock::provider::RockProviderApi* _api{ nullptr };
        std::uint64_t _ownerToken{ 0 };
        std::uint64_t _frameCallbackToken{ 0 };
    };

    [[nodiscard]] RockApiClient& rockApiClient() noexcept;
}
