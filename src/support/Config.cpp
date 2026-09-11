#include "support/Config.h"

#include "support/Logger.h"

#include <Windows.h>
#include <ShlObj.h>
#include <SimpleIni.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace rock_wall_climbing
{
    namespace
    {
        [[nodiscard]] std::filesystem::path resolveActiveIniPath()
        {
            PWSTR documents = nullptr;
            const HRESULT result = SHGetKnownFolderPath(
                FOLDERID_Documents,
                KF_FLAG_DEFAULT,
                nullptr,
                &documents);
            if (FAILED(result) || !documents) {
                if (documents) {
                    CoTaskMemFree(documents);
                }
                return {};
            }

            std::filesystem::path path(documents);
            CoTaskMemFree(documents);
            path /= L"My Games";
            path /= L"Fallout4VR";
            path /= L"Mods_Config";
            path /= L"ROCK_Wall_Climbing";
            path /= L"ROCK_Wall_Climbing.ini";
            return path;
        }

        [[nodiscard]] float readClampedFloat(
            const CSimpleIniA& ini,
            const char* section,
            const char* key,
            const float current,
            const float fallback,
            const float minimum,
            const float maximum)
        {
            const float value = static_cast<float>(
                ini.GetDoubleValue(section, key, current));
            if (!std::isfinite(value)) {
                logger::warn(
                    "Invalid {}.{}; using compiled default {}.",
                    section,
                    key,
                    fallback);
                return fallback;
            }
            return std::clamp(value, minimum, maximum);
        }
    }

    bool Config::reload()
    {
        *this = Config{};
        activePath = resolveActiveIniPath();
        if (activePath.empty()) {
            logger::warn(
                "Could not resolve the Documents known folder; using compiled climbing defaults.");
            logger::setLevel(logLevel);
            return false;
        }

        std::FILE* file = nullptr;
        if (_wfopen_s(&file, activePath.c_str(), L"rb") != 0 || !file) {
            logger::warn(
                "Could not load '{}'; using compiled climbing defaults.",
                activePath.string());
            logger::setLevel(logLevel);
            return false;
        }

        CSimpleIniA ini;
        ini.SetUnicode();
        const SI_Error loadResult = ini.LoadFile(file);
        std::fclose(file);
        if (loadResult < 0) {
            logger::warn(
                "Could not parse '{}'; using compiled climbing defaults.",
                activePath.string());
            logger::setLevel(logLevel);
            return false;
        }

        enabled = ini.GetBoolValue("General", "bEnabled", enabled);
        logLevel = static_cast<int>(std::clamp<long>(
            ini.GetLongValue("General", "iLogLevel", logLevel),
            0,
            4));

        movementScale = readClampedFloat(
            ini, "Climbing", "fMovementScale", movementScale,
            1.0f, 0.1f, 2.0f);
        smoothingSpeed = readClampedFloat(
            ini, "Climbing", "fSmoothingSpeed", smoothingSpeed,
            13.0f, 1.0f, 80.0f);
        maximumHandDeltaGameUnits = readClampedFloat(
            ini, "Climbing", "fMaximumHandDeltaGameUnits",
            maximumHandDeltaGameUnits, 24.0f, 2.0f, 100.0f);
        maximumTargetSeparationGameUnits = readClampedFloat(
            ini, "Climbing", "fMaximumTargetSeparationGameUnits",
            maximumTargetSeparationGameUnits, 96.0f, 20.0f, 500.0f);
        headClearanceGameUnits = readClampedFloat(
            ini, "Climbing", "fHeadClearanceGameUnits",
            headClearanceGameUnits, 12.0f, 0.0f, 40.0f);
        followMovingSurfaces = ini.GetBoolValue(
            "Climbing",
            "bFollowMovingSurfaces",
            followMovingSurfaces);

        launch.enabled = ini.GetBoolValue(
            "Launch", "bEnabled", launch.enabled);
        launch.historySeconds = readClampedFloat(
            ini, "Launch", "fVelocityHistorySeconds",
            launch.historySeconds, 0.10f, 0.03f, 0.5f);
        launch.directionFilterDegrees = readClampedFloat(
            ini, "Launch", "fDirectionFilterDegrees",
            launch.directionFilterDegrees, 75.0f, 0.0f, 180.0f);
        launch.multiplier = readClampedFloat(
            ini, "Launch", "fMultiplier",
            launch.multiplier, 1.15f, 0.0f, 3.0f);
        launch.horizontalBoost = readClampedFloat(
            ini, "Launch", "fHorizontalBoost",
            launch.horizontalBoost, 1.05f, 0.0f, 3.0f);
        launch.minimumSpeed = readClampedFloat(
            ini, "Launch", "fMinimumSpeedGameUnitsPerSecond",
            launch.minimumSpeed, 35.0f, 0.0f, 500.0f);
        launch.maximumSpeed = readClampedFloat(
            ini, "Launch", "fMaximumSpeedGameUnitsPerSecond",
            launch.maximumSpeed, 450.0f, 1.0f, 1200.0f);
        launch.minimumSpeed = std::min(
            launch.minimumSpeed,
            launch.maximumSpeed);

        detailedTelemetry = ini.GetBoolValue(
            "Diagnostics",
            "bDetailedTelemetry",
            detailedTelemetry);

        logger::setLevel(logLevel);
        logger::info(
            "Loaded '{}' enabled={} movementScale={:.2f} smoothing={:.1f} movingSurfaces={} launch={} launchRange=({:.1f},{:.1f}) telemetry={}.",
            activePath.string(),
            enabled ? "yes" : "no",
            movementScale,
            smoothingSpeed,
            followMovingSurfaces ? "yes" : "no",
            launch.enabled ? "yes" : "no",
            launch.minimumSpeed,
            launch.maximumSpeed,
            detailedTelemetry ? "yes" : "no");
        return true;
    }
}
