#include "support/Config.h"

#include "support/Logger.h"

#include <Windows.h>
#include <ShlObj.h>

#include <filesystem>
#include <memory>

namespace rock_wall_climbing
{
    namespace
    {
        // The store lives for the process and is used only on the game thread.
        std::unique_ptr<RPS::Settings::Store> s_store;

        [[nodiscard]] std::filesystem::path resolveConfigDirectory()
        {
            PWSTR documents = nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents)) || !documents) {
                if (documents) {
                    CoTaskMemFree(documents);
                }
                return {};
            }
            std::filesystem::path path(documents);
            CoTaskMemFree(documents);
            return path / L"My Games" / L"Fallout4VR" / L"Mods_Config" / L"ROCK_Wall_Climbing";
        }
    }

    bool Config::reload()
    {
        *this = Config{};
        try {
            const bool startup = !s_store;
            if (startup) {
                const auto directory = resolveConfigDirectory();
                if (directory.empty()) {
                    logger::warn("Could not resolve the Documents known folder; using compiled climbing defaults.");
                    logger::setLevel(logLevel);
                    return false;
                }
                s_store = makeStore(directory);
            }
            s_store->load(startup);
            for (const auto& notice : s_store->notices()) {
                logger::info("{}", notice);
            }
            for (const auto& message : s_store->messages()) {
                logger::warn("{}", message);
            }
            *this = fromStore(*s_store);
        } catch (const std::exception& error) {
            *this = Config{};
            logger::warn("Could not load climbing settings ({}); using compiled climbing defaults.", error.what());
            logger::setLevel(logLevel);
            return false;
        }

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
