#include "PCH.h"

#include "runtime/ClimbingRuntime.h"

#include <Windows.h>

namespace
{
    void reportPluginBoundaryFailure(
        const char* boundary,
        const char* detail) noexcept
    {
        char message[512]{};
        const int written = std::snprintf(
            message,
            sizeof(message),
            "ROCK Wall Climbing: unhandled exception in %s%s%s\n",
            boundary ? boundary : "plugin boundary",
            detail ? ": " : "",
            detail ? detail : "");
        if (written > 0) {
            ::OutputDebugStringA(message);
        }
    }

    void f4seMessageHandler(
        F4SE::MessagingInterface::Message* message) noexcept
    {
        if (!message) {
            return;
        }

        auto& runtime = rock_wall_climbing::ClimbingRuntime::get();
        switch (message->type) {
        case F4SE::MessagingInterface::kGameLoaded:
            runtime.beginSession();
            break;
        case F4SE::MessagingInterface::kGameDataReady:
            static_cast<void>(runtime.connect());
            break;
        case F4SE::MessagingInterface::kPreLoadGame:
            runtime.endSession();
            break;
        case F4SE::MessagingInterface::kPostLoadGame:
        case F4SE::MessagingInterface::kNewGame:
            runtime.beginSession();
            break;
        default:
            break;
        }
    }
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(
    const F4SE::QueryInterface* a_f4se,
    F4SE::PluginInfo* a_info) noexcept
{
    try {
        if (!a_f4se || !a_info) {
            return false;
        }

        rock_wall_climbing::logger::init();
        rock_wall_climbing::logger::info(
            "=== ROCK (and Walls) Climbing v{} query ===",
            Version::NAME);

        a_info->infoVersion = F4SE::PluginInfo::kVersion;
        a_info->name = "ROCK_Wall_Climbing";
        a_info->version = static_cast<std::uint32_t>(
            Version::MAJOR * 10000 +
            Version::MINOR * 100 +
            Version::PATCH);

        if (a_f4se->IsEditor()) {
            rock_wall_climbing::logger::critical(
                "Editor runtime is unsupported.");
            return false;
        }
        if (!REL::Module::IsVR()) {
            rock_wall_climbing::logger::critical(
                "Fallout 4 VR runtime is required.");
            return false;
        }

        const auto requiredRuntime = F4SE::RUNTIME_1_10_138;
        if (a_f4se->RuntimeVersion() < requiredRuntime) {
            rock_wall_climbing::logger::critical(
                "Unsupported F4SE compatibility runtime {} (need >= {}).",
                a_f4se->RuntimeVersion().string(),
                requiredRuntime.string());
            return false;
        }

        const auto executableVersion = REL::Module::get().version();
        if (executableVersion != F4SE::RUNTIME_VR_1_2_72) {
            rock_wall_climbing::logger::critical(
                "Only Fallout4VR.exe 1.2.72 is supported; executable={}, F4SE compatibility runtime={}.",
                executableVersion.string(),
                a_f4se->RuntimeVersion().string());
            return false;
        }

        rock_wall_climbing::logger::info(
            "Query complete: Fallout4VR.exe {}, F4SE compatibility runtime {}.",
            executableVersion.string(),
            a_f4se->RuntimeVersion().string());
        return true;
    } catch (const std::exception& error) {
        reportPluginBoundaryFailure("F4SEPlugin_Query", error.what());
        return false;
    } catch (...) {
        reportPluginBoundaryFailure("F4SEPlugin_Query", nullptr);
        return false;
    }
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(
    const F4SE::LoadInterface* a_f4se) noexcept
{
    try {
        if (!a_f4se) {
            return false;
        }
        F4SE::Init(a_f4se, false);

        const auto* messaging = F4SE::GetMessagingInterface();
        if (!messaging ||
            !messaging->RegisterListener(f4seMessageHandler)) {
            rock_wall_climbing::logger::critical(
                "F4SE messaging registration failed.");
            return false;
        }

        rock_wall_climbing::logger::info(
            "Load complete; waiting for GameLoaded before registering the ROCK V1 climbing consumer.");
        return true;
    } catch (const std::exception& error) {
        reportPluginBoundaryFailure("F4SEPlugin_Load", error.what());
        return false;
    } catch (...) {
        reportPluginBoundaryFailure("F4SEPlugin_Load", nullptr);
        return false;
    }
}
