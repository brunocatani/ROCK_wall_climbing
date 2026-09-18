#include "api/RockApiClient.h"

#include "support/Logger.h"

#include <cstring>
#include <cmath>
#include <Windows.h>

namespace rock_wall_climbing
{
    RockApiClient& rockApiClient() noexcept
    {
        static RockApiClient client;
        return client;
    }

    bool RockApiClient::initialize() noexcept {
        if (ready()) return true;
        const auto module=GetModuleHandleA("ROCK.dll");
        const auto query=module?reinterpret_cast<rock::api::QueryInterfaceV1>(GetProcAddress(module,rock::api::kQueryExportName)):nullptr;
        if (_client.connect(query,"ROCK_Wall_Climbing")!=rock::api::Status::Ok) return false;
        _ownerToken=_client.owner();
        if (_client.acquire(5,_core)!=rock::api::Status::Ok || _client.acquire(1,_hands)!=rock::api::Status::Ok ||
            _client.acquire(1,_grab)!=rock::api::Status::Ok || _client.acquire(3,_touch)!=rock::api::Status::Ok ||
            _client.acquire(1,_collision)!=rock::api::Status::Ok || _client.acquire(3,_controller)!=rock::api::Status::Ok) {
            logger::error("ROCK is missing a required modular climbing interface"); shutdown(); return false;
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
        return _core->registerFrameCallbackForOwnerV1(_ownerToken,receiveFrame,this,&_frameCallbackToken)==rock::api::Status::Ok;
    }
    void ROCK_CALL RockApiClient::receiveFrame(const rock::api::core::SnapshotV1* core,void* opaque) {
        auto* self=static_cast<RockApiClient*>(opaque);
        if (!core || !self || !self->_callback) return;
        Frame frame{}; static_cast<rock::api::core::SnapshotV1&>(frame)=*core;
        rock::api::collision::EnvironmentV1 environment{};
        rock::api::hands::HeadPoseV1 head{};
        rock::api::hands::HandFrameV1 right{},left{};
        using rock::api::Status;
        if (self->_collision->getEnvironment(self->_ownerToken,&environment)==Status::Ok &&
            environment.sample.frameIndex==core->frameIndex && environment.sample.worldGeneration==core->worldGeneration &&
            self->_hands->getHeadPose(self->_ownerToken,&head)==Status::Ok && head.sample.frameIndex==core->frameIndex &&
            self->_hands->getHandFrameV1(self->_ownerToken,rock::api::Hand::Right,&right)==Status::Ok && right.frameIndex==core->frameIndex &&
            self->_hands->getHandFrameV1(self->_ownerToken,rock::api::Hand::Left,&left)==Status::Ok && left.frameIndex==core->frameIndex &&
            (right.flags&1) && (left.flags&1)) {
            frame.gameToHavokScale=environment.gameToHavokScale; frame.havokToGameScale=environment.havokToGameScale;
            frame.physicsScaleRevision=environment.physicsScaleRevision; frame.collisionGeneration=environment.sample.collisionGeneration;
            frame.hmdTransform=head.transform; frame.rightHandTransform=right.transform; frame.leftHandTransform=left.transform;
            frame.enrichmentFlags=128|256;
            if (head.valid&1) frame.enrichmentFlags|=2;
            if (std::isfinite(core->deltaSeconds) && core->deltaSeconds>0) frame.enrichmentFlags|=1;
        }
        self->_callback(&frame,self->_callbackData);
    }

    rock::api::Status RockApiClient::publishTargets(
        const std::uint64_t scopeToken,
        const std::span<const
            rock::api::touch::TouchGrabTargetV1> targets)
        const noexcept
    {
        if (!ready()) {
            return rock::api::Status::NotReady;
        }
        return _touch->setTouchGrabTargetsForScopeV1(
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
        return _touch->clearTouchGrabTargetsForScopeV1(
            _ownerToken,
            scopeToken);
    }

    rock::api::Status RockApiClient::copyStates(
        const std::uint64_t scopeToken,
        const std::span<rock::api::touch::TouchGrabStateV1> states,
        std::uint32_t& outCount) const noexcept
    {
        outCount = 0;
        if (!ready()) {
            return rock::api::Status::NotReady;
        }
        for (auto& state:states) state={};
        return _touch->copyTouchGrabStatesForScopeV1(
            _ownerToken,
            scopeToken,
            states.data(),
            static_cast<std::uint32_t>(states.size()),
            &outCount);
    }

    rock::api::Status
        RockApiClient::queryHandInteractionState(
            const rock::api::Hand hand,
            rock::api::grab::HandInteractionStateV1& state)
            const noexcept
    {
        state = {};
        if (!ready()) {
            return rock::api::Status::NotReady;
        }
        return _grab->getHandInteractionStateV1(
            _ownerToken,
            hand,
            &state);
    }

    rock::api::Status RockApiClient::queryWorldRaycast(
        const rock::api::collision::WorldRaycastRequestV1& request,
        rock::api::collision::WorldRaycastResultV1& result)
        const noexcept
    {
        result = {};
        if (!ready()) {
            return rock::api::Status::NotReady;
        }
        return _collision->queryWorldRaycastV1(
            _ownerToken,
            &request,
            &result);
    }

    rock::api::Status
        RockApiClient::queryPlayerControllerState(
            rock::api::playercontroller::PlayerControllerStateV1& state)
            const noexcept
    {
        state = {};
        if (!ready()) {
            return rock::api::Status::NotReady;
        }
        return _controller->getPlayerControllerStateV1(
            _ownerToken,
            &state);
    }

    rock::api::Status
        RockApiClient::requestPlayerControllerJump(
            const rock::api::playercontroller::PlayerControllerJumpRequestV1& request)
            const noexcept
    {
        if (!ready()) {
            return rock::api::Status::NotReady;
        }
        return _controller->requestPlayerControllerJumpV1(
            _ownerToken,
            &request);
    }

}
