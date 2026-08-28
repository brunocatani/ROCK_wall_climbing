#pragma once

#include "runtime/ClimbingPolicy.h"

#include <filesystem>

namespace rock_wall_climbing
{
    struct Config
    {
        bool enabled{ true };
        int logLevel{ 2 };

        float movementScale{ 1.0f };
        float smoothingSpeed{ 18.0f };
        float maximumHandDeltaGameUnits{ 24.0f };
        float maximumTargetSeparationGameUnits{ 96.0f };
        float headClearanceGameUnits{ 12.0f };
        bool followMovingSurfaces{ true };

        policy::LaunchSettings launch{};
        bool detailedTelemetry{ false };

        std::filesystem::path activePath{};

        [[nodiscard]] bool reload();
    };
}
