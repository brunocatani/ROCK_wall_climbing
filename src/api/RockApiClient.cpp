#include "api/RockApiClient.h"

#include "support/Logger.h"

#include <cstring>

namespace rock_wall_climbing
{
    namespace
    {
        constexpr std::uint32_t REQUIRED_CAPABILITIES =
            static_cast<std::uint32_t>(
                rock::provider::RockProviderConsumerCapabilityV1::
                    FrameSnapshots) |
            static_cast<std::uint32_t>(
                rock::provider::RockProviderConsumerCapabilityV1::
                    TouchGrabTargets) |
            static_cast<std::uint32_t>(
                rock::provider::RockProviderConsumerCapabilityV1::
                    WorldRaycasts);
    }

    RockApiClient& rockApiClient() noexcept
    {
        static RockApiClient client;
        return client;
    }

    bool RockApiClient::initialize() noexcept
    {
        if (ready()) {
            return true;
        }

        const int initializeResult =
            rock::provider::RockProviderApi::initialize(
                rock::provider::ROCK_PROVIDER_API_VERSION,
                rock::provider::
                    ROCK_PROVIDER_API_V1_WORLD_RAYCASTS_TABLE_BYTES);
        if (initializeResult != 0) {
            logger::error(
                "ROCK V1 provider initialization failed: result={}.",
                initializeResult);
            return false;
        }

        _api = rock::provider::RockProviderApi::inst;
        const bool snapshotEnrichment =
            rock::provider::hasFeatureBit2V1(
                rock::provider::RockProviderApi::negotiatedFeatureBits2,
                rock::provider::RockProviderFeatureBit2V1::
                    SnapshotEnrichment);
        if (!_api ||
            !snapshotEnrichment ||
            !rock::provider::supportsOwnerFrameCallbacksV1() ||
            !rock::provider::supportsTouchGrabTargetsV1() ||
            !rock::provider::supportsWorldRaycastsV1() ||
            !_api->registerConsumerV1 ||
            !_api->unregisterConsumerV1 ||
            !_api->registerFrameCallbackForOwnerV1 ||
            !_api->unregisterFrameCallbackForOwnerV1 ||
            !_api->setTouchGrabTargetsForScopeV1 ||
            !_api->clearTouchGrabTargetsForScopeV1 ||
            !_api->copyTouchGrabStatesForScopeV1 ||
            !_api->queryWorldRaycastV1) {
            logger::error(
                "Loaded ROCK V1 provider lacks the complete climbing contract (snapshot enrichment, owner callbacks, touch grabs, and world raycasts).");
            _api = nullptr;
            return false;
        }

        rock::provider::RockProviderConsumerRegistrationV1 registration{};
        constexpr char MOD_NAME[] = "ROCK_Wall_Climbing";
        std::memcpy(
            registration.modName,
            MOD_NAME,
            sizeof(MOD_NAME));
        registration.requestedCapabilities = REQUIRED_CAPABILITIES;

        rock::provider::RockProviderConsumerHandleV1 handle{};
        const auto result = _api->registerConsumerV1(
            &registration,
            &handle);
        if (result != rock::provider::RockProviderResultV1::Ok ||
            handle.ownerToken == 0 ||
            (handle.grantedCapabilities & REQUIRED_CAPABILITIES) !=
                REQUIRED_CAPABILITIES) {
            logger::error(
                "ROCK V1 climbing-consumer registration failed: result={} granted=0x{:08X}.",
                static_cast<std::uint32_t>(result),
                handle.grantedCapabilities);
            if (handle.ownerToken != 0) {
                static_cast<void>(
                    _api->unregisterConsumerV1(handle.ownerToken));
            }
            _api = nullptr;
            return false;
        }

        _ownerToken = handle.ownerToken;
        logger::info(
            "Registered ROCK V1 climbing consumer owner={:016X} capabilities=0x{:08X} providerGeneration={}.",
            _ownerToken,
            handle.grantedCapabilities,
            handle.providerGeneration);
        return true;
    }

    void RockApiClient::shutdown() noexcept
    {
        if (!ready()) {
            return;
        }
        if (_frameCallbackToken != 0) {
            static_cast<void>(
                _api->unregisterFrameCallbackForOwnerV1(
                    _ownerToken,
                    _frameCallbackToken));
            _frameCallbackToken = 0;
        }
        static_cast<void>(_api->unregisterConsumerV1(_ownerToken));
        _ownerToken = 0;
        _api = nullptr;
    }

    bool RockApiClient::ready() const noexcept
    {
        return _api && _ownerToken != 0;
    }

    std::uint64_t RockApiClient::ownerToken() const noexcept
    {
        return _ownerToken;
    }

    bool RockApiClient::registerFrameCallback(
        const rock::provider::RockProviderFrameCallback callback,
        void* userData) noexcept
    {
        if (!ready() || !callback) {
            return false;
        }
        if (_frameCallbackToken != 0) {
            return true;
        }
        return _api->registerFrameCallbackForOwnerV1(
                   _ownerToken,
                   callback,
                   userData,
                   &_frameCallbackToken) ==
               rock::provider::RockProviderResultV1::Ok;
    }

    rock::provider::RockProviderResultV1 RockApiClient::publishTargets(
        const std::uint64_t scopeToken,
        const std::span<const
            rock::provider::RockProviderTouchGrabTargetV1> targets)
        const noexcept
    {
        if (!ready()) {
            return rock::provider::RockProviderResultV1::NotReady;
        }
        return _api->setTouchGrabTargetsForScopeV1(
            _ownerToken,
            scopeToken,
            targets.data(),
            static_cast<std::uint32_t>(targets.size()));
    }

    rock::provider::RockProviderResultV1 RockApiClient::clearTargets(
        const std::uint64_t scopeToken) const noexcept
    {
        if (!ready()) {
            return rock::provider::RockProviderResultV1::NotReady;
        }
        return _api->clearTouchGrabTargetsForScopeV1(
            _ownerToken,
            scopeToken);
    }

    rock::provider::RockProviderResultV1 RockApiClient::copyStates(
        const std::uint64_t scopeToken,
        const std::span<rock::provider::RockProviderTouchGrabStateV1> states,
        std::uint32_t& outCount) const noexcept
    {
        outCount = 0;
        if (!ready()) {
            return rock::provider::RockProviderResultV1::NotReady;
        }
        return _api->copyTouchGrabStatesForScopeV1(
            _ownerToken,
            scopeToken,
            states.data(),
            static_cast<std::uint32_t>(states.size()),
            &outCount);
    }

    rock::provider::RockProviderResultV1 RockApiClient::queryWorldRaycast(
        const rock::provider::RockProviderWorldRaycastRequestV1& request,
        rock::provider::RockProviderWorldRaycastResultV1& result)
        const noexcept
    {
        result = {};
        if (!ready()) {
            return rock::provider::RockProviderResultV1::NotReady;
        }
        return _api->queryWorldRaycastV1(
            _ownerToken,
            &request,
            &result);
    }
}
