#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rock_wall_climbing::controller_policy
{
    enum class VelocityImplementation : std::uint8_t
    {
        Unknown,
        Proxy,
        RigidBody,
    };

    struct VelocityDispatchSpec
    {
        VelocityImplementation implementation{
            VelocityImplementation::Unknown
        };
        std::uintptr_t vtableRva{ 0 };
        std::uintptr_t functionRva{ 0 };
        std::array<std::uint8_t, 16> functionPrefix{};
    };

    // FO4VR 1.2.72 bhkCharacterController subobject slot 0x3C. The
    // CommonLibF4VR declaration currently names slot 0x3A, which is the
    // concrete SetTransform implementation and must never receive a velocity.
    inline constexpr std::size_t VELOCITY_SLOT_INDEX = 0x3C;
    inline constexpr std::size_t VELOCITY_SLOT_OFFSET =
        VELOCITY_SLOT_INDEX * sizeof(std::uintptr_t);

    inline constexpr std::array<VelocityDispatchSpec, 2>
        VELOCITY_DISPATCH_SPECS{
            VelocityDispatchSpec{
                VelocityImplementation::Proxy,
                0x2E89328,
                0x1E4E5B0,
                {
                    0x48, 0x8B, 0x81, 0x70, 0x04, 0x00, 0x00, 0x48,
                    0x85, 0xC0, 0x74, 0x0A, 0x0F, 0x28, 0x02, 0x0F,
                },
            },
            VelocityDispatchSpec{
                VelocityImplementation::RigidBody,
                0x2E89B28,
                0x1E53EB0,
                {
                    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
                    0xEC, 0x20, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xFA,
                },
            },
        };

    [[nodiscard]] constexpr const VelocityDispatchSpec*
        selectVelocityDispatch(
            const std::uintptr_t moduleBase,
            const std::uintptr_t vtableAddress,
            const std::uintptr_t functionAddress) noexcept
    {
        if (moduleBase == 0 || vtableAddress < moduleBase ||
            functionAddress < moduleBase) {
            return nullptr;
        }

        const std::uintptr_t vtableRva = vtableAddress - moduleBase;
        const std::uintptr_t functionRva = functionAddress - moduleBase;
        for (const auto& spec : VELOCITY_DISPATCH_SPECS) {
            if (spec.vtableRva == vtableRva &&
                spec.functionRva == functionRva) {
                return &spec;
            }
        }
        return nullptr;
    }

    enum class GravityRestoreDecision : std::uint8_t
    {
        Restore,
        Defer,
        Retire,
    };

    [[nodiscard]] constexpr GravityRestoreDecision decideGravityRestore(
        const bool controllerAvailable,
        const std::uintptr_t savedControllerIdentity,
        const std::uintptr_t currentControllerIdentity) noexcept
    {
        if (!controllerAvailable) {
            return GravityRestoreDecision::Defer;
        }
        return savedControllerIdentity == currentControllerIdentity ?
            GravityRestoreDecision::Restore :
            GravityRestoreDecision::Retire;
    }
}
