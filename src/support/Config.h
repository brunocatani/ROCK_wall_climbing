#pragma once

#include "runtime/ClimbingPolicy.h"

#include <RPS/Settings/Store.h>

#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace rock_wall_climbing
{
    struct Config
    {
        static constexpr std::string_view kFileName = "ROCK_Wall_Climbing.toml";

        bool enabled{ true };
        int logLevel{ 2 };

        float movementScale{ 1.0f };
        float smoothingSpeed{ 13.0f };
        float maximumHandDeltaGameUnits{ 24.0f };
        float maximumTargetSeparationGameUnits{ 96.0f };
        float headClearanceGameUnits{ 12.0f };
        bool followMovingSurfaces{ true };

        policy::LaunchSettings launch{};
        bool detailedTelemetry{ false };

        std::filesystem::path activePath{};

        // Reads ROCK_Wall_Climbing.toml through RPS Framework's settings store.
        // The first call in a process also organizes the file and migrates
        // ROCK_Wall_Climbing.ini of earlier versions once. Game thread only.
        [[nodiscard]] bool reload();

        // Every option's table, key, help and default, and the old INI keys.
        [[nodiscard]] static std::vector<RPS::Settings::Definition> definitions();
        [[nodiscard]] static std::span<const RPS::Settings::LegacyKey> legacyKeys();
        [[nodiscard]] static std::unique_ptr<RPS::Settings::Store> makeStore(const std::filesystem::path& directory);
        // Loaded values as the climbing runtime uses them, clamped to their ranges.
        [[nodiscard]] static Config fromStore(const RPS::Settings::Store& store);
    };
}
