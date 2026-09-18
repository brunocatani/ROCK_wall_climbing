#pragma once
#include <ROCK/Core.h>
#include <ROCK/Hands.h>
#include <ROCK/Grab.h>
#include <ROCK/Touch.h>
#include <ROCK/Collision.h>
#include <ROCK/PlayerController.h>

namespace rock_wall_climbing {
    enum class FrameFlag : std::uint32_t { DeltaSecondsValid=1, HmdTransformValid=2, RightHandTransformValid=128, LeftHandTransformValid=256 };
    struct Frame : rock::api::core::SnapshotV1 {
        float gameToHavokScale{};
        float havokToGameScale{};
        std::uint32_t physicsScaleRevision{};
        std::uint32_t collisionGeneration{};
        std::uint32_t enrichmentFlags{};
        rock::api::Transform hmdTransform{};
        rock::api::Transform rightHandTransform{};
        rock::api::Transform leftHandTransform{};
    };
    using FrameCallback=void(ROCK_CALL*)(const Frame*,void*);
    constexpr bool hasLifecycleFlag(std::uint32_t flags,rock::api::core::LifecycleFlag bit) noexcept {
        return (flags&static_cast<std::uint32_t>(bit))!=0;
    }
}
