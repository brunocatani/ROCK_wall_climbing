#include "api/RockApiClient.h"

#include "support/Logger.h"

#include <RPS/Runtime/RequiredUpdate.h>

#include <cstring>
#include <cmath>
#include <string>
#include <Windows.h>

namespace rock_wall_climbing
{
    RockApiClient& rockApiClient() noexcept
    {
        static RockApiClient client;
        return client;
    }

    namespace
    {
        // A loaded ROCK that does not serve the API this addon was built for
        // stops the game at the main menu (RPS/Runtime/RequiredUpdate.h); a
        // missing ROCK does not. The addon requests the exact versions its ROCK
        // headers declare, so every version refusal is a real mismatch.
        bool isVersionRefusal(const rock::api::Status status) noexcept
        {
            using rock::api::Status;
            return status==Status::UnknownInterface || status==Status::UnsupportedMajor ||
                status==Status::UnsupportedMinor || status==Status::TableTooSmall || status==Status::InvalidSize;
        }

        void reportRockMismatch(const char* reason) noexcept
        {
            try {
                logger::critical("ROCK Wall Climbing does not work with the loaded ROCK ({}): the game stops at the main menu until they match",reason);
                RPS::Runtime::RequiredUpdate::report(L"ROCK Wall Climbing " +
                    std::wstring(Version::NAME.begin(),Version::NAME.end()) +
                    L" does not work with the installed ROCK: they were built for different ROCK versions. Install "
                    L"matching versions of ROCK and ROCK Wall Climbing.");
            } catch (...) {}
        }
    }

    bool RockApiClient::initialize() noexcept {
        if (ready()) return true;
        const auto module=GetModuleHandleA("ROCK.dll");
        if (!module) return false;
        const auto query=reinterpret_cast<rock::api::QueryInterfaceV1>(GetProcAddress(module,rock::api::kQueryExportName));
        if (!query) { reportRockMismatch("no ROCKAPI_QueryInterfaceV1 export"); return false; }
        if (const auto status=_client.connect(query,"ROCK_Wall_Climbing"); status!=rock::api::Status::Ok) {
            if (isVersionRefusal(status)) reportRockMismatch("Core refused");
            return false;
        }
        _ownerToken=_client.owner();
        namespace api=rock::api;
        constexpr auto bits=[](auto... values) { return (static_cast<std::uint32_t>(values) | ...); };
        auto status=api::Status::Ok;
        if ((status=_client.acquire(bits(api::core::Permission::Read,api::core::Permission::Callbacks),_core))!=api::Status::Ok ||
            (status=_client.acquire(bits(api::hands::Permission::Read),_hands))!=api::Status::Ok ||
            (status=_client.acquire(bits(api::grab::Permission::Read),_grab))!=api::Status::Ok ||
            (status=_client.acquire(bits(api::touch::Permission::Read,api::touch::Permission::Write),_touch))!=api::Status::Ok ||
            (status=_client.acquire(bits(api::collision::Permission::Read),_collision))!=api::Status::Ok ||
            (status=_client.acquire(bits(api::playercontroller::Permission::Read,api::playercontroller::Permission::Write),_controller))!=api::Status::Ok) {
            logger::error("ROCK is missing a required modular climbing interface: status {}",static_cast<std::uint32_t>(status));
            if (isVersionRefusal(status)) reportRockMismatch("an interface refused");
            shutdown(); return false;
        }
        logger::info("Registered modular ROCK climbing owner={:016X}",_ownerToken);
        return true;
    }
    void RockApiClient::shutdown() noexcept {
        const auto status=_client.close();
        if (status!=rock::api::Status::Ok && status!=rock::api::Status::OwnerNotRegistered) {
            logger::error("ROCK climbing owner teardown failed: {}",static_cast<std::uint32_t>(status)); return;
        }
        _ownerToken=0; _frameCallbackToken=0; _callback=nullptr; _callbackData=nullptr;
        _core=nullptr; _hands=nullptr; _grab=nullptr; _touch=nullptr; _collision=nullptr; _controller=nullptr;
    }
    bool RockApiClient::ready() const noexcept { return _client.owner() && _ownerToken && _core && _touch && _collision && _controller && _hands && _grab; }
    std::uint64_t RockApiClient::ownerToken() const noexcept { return _ownerToken; }
    bool RockApiClient::registerFrameCallback(FrameCallback callback,void* userData) noexcept {
        if (!ready() || !callback) return false;
        if (_frameCallbackToken) return true;
        _callback=callback; _callbackData=userData;
        return _core->registerFrameCallback(_ownerToken,receiveFrame,this,&_frameCallbackToken)==rock::api::Status::Ok;
    }
    void ROCK_CALL RockApiClient::receiveFrame(const rock::api::core::Snapshot* core,void* opaque) {
        auto* self=static_cast<RockApiClient*>(opaque);
        if (!core || !self || !self->_callback) return;
        Frame frame{}; static_cast<rock::api::core::Snapshot&>(frame)=*core;
        rock::api::collision::Environment environment{};
        rock::api::hands::HeadPose head{};
        rock::api::hands::HandFrame right{},left{};
        using rock::api::Status;
        constexpr auto handValid=static_cast<std::uint32_t>(rock::api::hands::HandFrameFlag::Valid);
        const auto frameIndex=core->sample.frameIndex;
        if (self->_collision->getEnvironment(self->_ownerToken,&environment)==Status::Ok &&
            environment.sample.frameIndex==frameIndex && environment.sample.worldGeneration==core->sample.worldGeneration &&
            self->_hands->getHeadPose(self->_ownerToken,&head)==Status::Ok && head.sample.frameIndex==frameIndex &&
            self->_hands->getHandFrame(self->_ownerToken,rock::api::Hand::Right,&right)==Status::Ok && right.sample.frameIndex==frameIndex &&
            self->_hands->getHandFrame(self->_ownerToken,rock::api::Hand::Left,&left)==Status::Ok && left.sample.frameIndex==frameIndex &&
            (right.flags&handValid) && (left.flags&handValid)) {
            frame.gameToHavokScale=environment.gameToHavokScale; frame.havokToGameScale=environment.havokToGameScale;
            frame.physicsScaleRevision=environment.physicsScaleRevision; frame.collisionGeneration=environment.sample.collisionGeneration;
            frame.hmdTransform=head.transform; frame.rightHandTransform=right.transform; frame.leftHandTransform=left.transform;
            frame.enrichmentFlags=128|256;
            if (head.flags&static_cast<std::uint32_t>(rock::api::hands::HeadPoseFlag::TransformValid)) frame.enrichmentFlags|=2;
            if (std::isfinite(core->deltaSeconds) && core->deltaSeconds>0) frame.enrichmentFlags|=1;
        }
        self->_callback(&frame,self->_callbackData);
    }

    rock::api::Status RockApiClient::publishTargets(
        const std::uint64_t scopeToken,
        const std::span<const
            rock::api::touch::TouchGrabTarget> targets)
        const noexcept
    {
        if (!ready()) {
            return rock::api::Status::NotReady;
        }
        return _touch->setTouchGrabTargetsForScope(
            _ownerToken,
            scopeToken,
            targets.data(),
            static_cast<std::uint32_t>(targets.size()));
    }

    rock::api::Status RockApiClient::clearTargets(
        const std::uint64_t scopeToken) const noexcept
    {
        if (!ready()) {
            return rock::api::Status::NotReady;
        }
        return _touch->clearTouchGrabTargetsForScope(
            _ownerToken,
            scopeToken);
    }

    rock::api::Status RockApiClient::copyStates(
        const std::uint64_t scopeToken,
        const std::span<rock::api::touch::TouchGrabState> states,
        std::uint32_t& outCount) const noexcept
    {
        outCount = 0;
        if (!ready()) {
            return rock::api::Status::NotReady;
        }
        for (auto& state:states) state={};
        return _touch->copyTouchGrabStatesForScope(
            _ownerToken,
            scopeToken,
            states.data(),
            static_cast<std::uint32_t>(states.size()),
            &outCount);
    }

    rock::api::Status
        RockApiClient::queryHandInteractionState(
            const rock::api::Hand hand,
            rock::api::grab::HandInteractionState& state)
            const noexcept
    {
        state = {};
        if (!ready()) {
            return rock::api::Status::NotReady;
        }
        return _grab->getHandInteractionState(
            _ownerToken,
            hand,
            &state);
    }

    rock::api::Status RockApiClient::queryWorldRaycast(
        const rock::api::collision::WorldRaycastRequest& request,
        rock::api::collision::WorldRaycastResult& result)
        const noexcept
    {
        result = {};
        if (!ready()) {
            return rock::api::Status::NotReady;
        }
        return _collision->queryWorldRaycast(
            _ownerToken,
            &request,
            &result);
    }

    rock::api::Status
        RockApiClient::queryPlayerControllerState(
            rock::api::playercontroller::PlayerControllerState& state)
            const noexcept
    {
        state = {};
        if (!ready()) {
            return rock::api::Status::NotReady;
        }
        return _controller->getPlayerControllerState(
            _ownerToken,
            &state);
    }

    rock::api::Status
        RockApiClient::requestPlayerControllerJump(
            const rock::api::playercontroller::PlayerControllerJumpRequest& request)
            const noexcept
    {
        if (!ready()) {
            return rock::api::Status::NotReady;
        }
        return _controller->requestPlayerControllerJump(
            _ownerToken,
            &request);
    }

}
