#pragma once

#include <memory>
#include <string_view>
#include <utility>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

namespace rock_wall_climbing::logger
{
    inline std::shared_ptr<spdlog::logger> instance;

    inline void init()
    {
        auto directory = F4SE::log::log_directory();
        const std::string_view expectedGamePath =
            REL::Module::IsVR() ? "Fallout4VR/F4SE" : "Fallout4/F4SE";
        if (!directory.value().generic_string().ends_with(expectedGamePath)) {
            directory =
                directory.value().parent_path().append(expectedGamePath);
        }
        *directory /= "ROCK_Wall_Climbing.log";
        auto sink =
            std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                directory->string(),
                5 * 1024 * 1024,
                3,
                true);
        instance = std::make_shared<spdlog::logger>(
            "ROCK_Wall_Climbing",
            std::move(sink));
        instance->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] %v");
        instance->set_level(spdlog::level::info);
        instance->flush_on(spdlog::level::info);
        spdlog::set_default_logger(instance);
    }

    inline void setLevel(const int configuredLevel) noexcept
    {
        if (!instance) {
            return;
        }
        switch (configuredLevel) {
        case 0:
            instance->set_level(spdlog::level::off);
            break;
        case 1:
            instance->set_level(spdlog::level::err);
            break;
        case 3:
            instance->set_level(spdlog::level::debug);
            break;
        case 4:
            instance->set_level(spdlog::level::trace);
            break;
        case 2:
        default:
            instance->set_level(spdlog::level::info);
            break;
        }
    }

    template <class... Args>
    void trace(spdlog::format_string_t<Args...> format, Args&&... args)
    {
        if (instance) {
            instance->trace(format, std::forward<Args>(args)...);
        }
    }

    template <class... Args>
    void debug(spdlog::format_string_t<Args...> format, Args&&... args)
    {
        if (instance) {
            instance->debug(format, std::forward<Args>(args)...);
        }
    }

    template <class... Args>
    void info(spdlog::format_string_t<Args...> format, Args&&... args)
    {
        if (instance) {
            instance->info(format, std::forward<Args>(args)...);
        }
    }

    template <class... Args>
    void warn(spdlog::format_string_t<Args...> format, Args&&... args)
    {
        if (instance) {
            instance->warn(format, std::forward<Args>(args)...);
        }
    }

    template <class... Args>
    void error(spdlog::format_string_t<Args...> format, Args&&... args)
    {
        if (instance) {
            instance->error(format, std::forward<Args>(args)...);
        }
    }

    template <class... Args>
    void critical(spdlog::format_string_t<Args...> format, Args&&... args)
    {
        if (instance) {
            instance->critical(format, std::forward<Args>(args)...);
        }
    }
}
