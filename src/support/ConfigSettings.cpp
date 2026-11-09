// ROCK_Wall_Climbing.toml's options: tables, keys, help and defaults, the INI
// keys of earlier versions, and the conversion of loaded values into Config.
// No F4SE or logging here, so the tests use it as the plugin does.
#include "support/Config.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <stdexcept>
#include <string>

namespace rock_wall_climbing
{
    namespace
    {
        using RPS::Settings::Definition;
        using RPS::Settings::Group;
        using RPS::Settings::LegacyKey;
        using RPS::Settings::ValueType;

        struct Range
        {
            float minimum, maximum;
        };
        constexpr Range kMovementScale{ 0.1f, 2.0f };
        constexpr Range kSmoothing{ 1.0f, 80.0f };
        constexpr Range kHandDelta{ 2.0f, 100.0f };
        constexpr Range kTargetSeparation{ 20.0f, 500.0f };
        constexpr Range kHeadClearance{ 0.0f, 40.0f };
        constexpr Range kHistory{ 0.03f, 0.5f };
        constexpr Range kDirectionFilter{ 0.0f, 180.0f };
        constexpr Range kMultiplier{ 0.0f, 3.0f };
        constexpr Range kHorizontalBoost{ 0.0f, 3.0f };
        constexpr Range kMinimumSpeed{ 0.0f, 500.0f };
        constexpr Range kMaximumSpeed{ 1.0f, 1200.0f };

        std::string text(bool value) { return value ? "true" : "false"; }
        std::string text(int value) { return std::to_string(value); }
        std::string text(float value)
        {
            std::array<char, 32> buffer{};
            const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
            return RPS::Settings::normalizeValue(ValueType::Float,
                std::string_view(buffer.data(), static_cast<std::size_t>(written.ptr - buffer.data())))
                .value_or("0.0");
        }
        std::string rangeText(float value)
        {
            std::array<char, 32> buffer{};
            const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
            return std::string(buffer.data(), static_cast<std::size_t>(written.ptr - buffer.data()));
        }
        std::string withRange(std::string_view help, Range range)
        {
            return std::format("{} Range: {} to {}.", help, rangeText(range.minimum), rangeText(range.maximum));
        }

        const std::string& valueOf(const RPS::Settings::Store& store, std::string_view table, std::string_view key)
        {
            const auto* setting = store.find(table, key);
            if (!setting)
                throw std::logic_error(std::format("ROCK Wall Climbing setting {}.{} is not declared", table, key));
            return setting->value;
        }
        float number(const RPS::Settings::Store& store, std::string_view table, std::string_view key, float fallback,
            Range range)
        {
            const auto& value = valueOf(store, table, key);
            float result = fallback;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
            if (parsed.ec != std::errc{} || !std::isfinite(result))
                result = fallback;
            return std::clamp(result, range.minimum, range.maximum);
        }

        struct Old
        {
            std::string_view oldKey, table, key;
            std::string_view oldSection{};
        };
        // The INI keys of earlier versions. Their defaults then were the
        // compiled defaults now, so only changed values carry over. The INI
        // used bEnabled in two sections, so those rows name theirs.
        constexpr std::array kOldKeys{
            Old{ "bEnabled", "General", "Enabled", "General" },
            Old{ "bEnabled", "Launch", "Enabled", "Launch" },
            Old{ "iLogLevel", "General", "LogLevel" },
            Old{ "fMovementScale", "Climbing", "MovementScale" },
            Old{ "fSmoothingSpeed", "Climbing", "SmoothingSpeed" },
            Old{ "fMaximumHandDeltaGameUnits", "Climbing", "MaximumHandDeltaGameUnits" },
            Old{ "fMaximumTargetSeparationGameUnits", "Climbing", "MaximumTargetSeparationGameUnits" },
            Old{ "fHeadClearanceGameUnits", "Climbing", "HeadClearanceGameUnits" },
            Old{ "bFollowMovingSurfaces", "Climbing", "FollowMovingSurfaces" },
            Old{ "fVelocityHistorySeconds", "Launch", "VelocityHistorySeconds" },
            Old{ "fDirectionFilterDegrees", "Launch", "DirectionFilterDegrees" },
            Old{ "fMultiplier", "Launch", "Multiplier" },
            Old{ "fHorizontalBoost", "Launch", "HorizontalBoost" },
            Old{ "fMinimumSpeedGameUnitsPerSecond", "Launch", "MinimumSpeedGameUnitsPerSecond" },
            Old{ "fMaximumSpeedGameUnitsPerSecond", "Launch", "MaximumSpeedGameUnitsPerSecond" },
            Old{ "bDetailedTelemetry", "Diagnostics", "DetailedTelemetry" },
        };
    }

    std::vector<Definition> Config::definitions()
    {
        const Config d{};
        const auto player = Group::Player;
        constexpr auto general = "01. General";
        constexpr auto climbing = "02. Climbing";
        constexpr auto launch = "03. Launch";
        constexpr auto diagnostics = "04. Diagnostics";
        return {
            { "General", "Enabled", general, "Turns wall climbing on.", player, ValueType::Boolean, text(d.enabled) },
            { "General", "LogLevel", general, "0=off, 1=errors, 2=info, 3=debug, 4=trace.", player, ValueType::Integer,
                text(d.logLevel) },
            { "Climbing", "MovementScale", climbing,
                withRange("Player displacement per unit of inverse tracked-hand displacement.", kMovementScale), player,
                ValueType::Float, text(d.movementScale) },
            { "Climbing", "SmoothingSpeed", climbing,
                withRange("Base target-follow speed; response increases adaptively during larger pulls.", kSmoothing), player,
                ValueType::Float, text(d.smoothingSpeed) },
            { "Climbing", "MaximumHandDeltaGameUnits", climbing,
                withRange("A larger per-frame hand or anchor jump, in game units, is treated as a tracking discontinuity.",
                    kHandDelta),
                player, ValueType::Float, text(d.maximumHandDeltaGameUnits) },
            { "Climbing", "MaximumTargetSeparationGameUnits", climbing,
                withRange("Rebase instead of correcting toward a stale target after a teleport or correction, in game units.",
                    kTargetSeparation),
                player, ValueType::Float, text(d.maximumTargetSeparationGameUnits) },
            { "Climbing", "HeadClearanceGameUnits", climbing,
                withRange("Headset clearance kept along ROCK's guarded world raycast, in game units.", kHeadClearance), player,
                ValueType::Float, text(d.headClearanceGameUnits) },
            { "Climbing", "FollowMovingSurfaces", climbing, "Carry the player with a grabbed moving (keyframed) surface.",
                player, ValueType::Boolean, text(d.followMovingSurfaces) },
            { "Launch", "Enabled", launch, "Letting go while pulling launches you along the pull.", player,
                ValueType::Boolean, text(d.launch.enabled) },
            { "Launch", "VelocityHistorySeconds", launch,
                withRange("Seconds of hand motion the launch velocity is measured over.", kHistory), player, ValueType::Float,
                text(d.launch.historySeconds) },
            { "Launch", "DirectionFilterDegrees", launch,
                withRange("Hand motion further than this many degrees from the launch direction is ignored.", kDirectionFilter),
                player, ValueType::Float, text(d.launch.directionFilterDegrees) },
            { "Launch", "Multiplier", launch, withRange("Scales the launch speed.", kMultiplier), player, ValueType::Float,
                text(d.launch.multiplier) },
            { "Launch", "HorizontalBoost", launch, withRange("Scales the horizontal part of the launch.", kHorizontalBoost),
                player, ValueType::Float, text(d.launch.horizontalBoost) },
            { "Launch", "MinimumSpeedGameUnitsPerSecond", launch,
                withRange("Slower releases do not launch; never more than MaximumSpeedGameUnitsPerSecond.", kMinimumSpeed),
                player, ValueType::Float, text(d.launch.minimumSpeed) },
            { "Launch", "MaximumSpeedGameUnitsPerSecond", launch, withRange("Fastest launch.", kMaximumSpeed), player,
                ValueType::Float, text(d.launch.maximumSpeed) },
            { "Diagnostics", "DetailedTelemetry", diagnostics,
                "Logs bounded, sampled movement telemetry. Off for normal play.", player, ValueType::Boolean,
                text(d.detailedTelemetry) },
        };
    }

    std::span<const LegacyKey> Config::legacyKeys()
    {
        static const auto keys = [] {
            struct Table
            {
                std::vector<std::string> defaults;
                std::vector<LegacyKey> keys;
            } table;
            const auto all = definitions();
            table.defaults.reserve(kOldKeys.size());
            for (const auto& old : kOldKeys) {
                const auto found = std::ranges::find_if(all,
                    [&](const Definition& definition) { return definition.table == old.table && definition.key == old.key; });
                if (found == all.end())
                    throw std::logic_error(std::format("ROCK Wall Climbing old key {} names no option", old.oldKey));
                LegacyKey key{};
                key.oldKey = old.oldKey;
                key.table = old.table;
                key.key = old.key;
                key.oldDefault = table.defaults.emplace_back(found->defaultValue);
                key.oldSection = old.oldSection;
                table.keys.push_back(key);
            }
            return table;
        }();
        return keys.keys;
    }

    std::unique_ptr<RPS::Settings::Store> Config::makeStore(const std::filesystem::path& directory)
    {
        return std::make_unique<RPS::Settings::Store>(
            RPS::Settings::Files{ directory, "ROCK Wall Climbing", std::string(kFileName), {},
                "## ROCK_Wall_Climbing.toml - ROCK Wall Climbing options\n"
                "## Changes apply when a game is loaded. Each option has its help above it. When the mod starts it adds new\n"
                "## options with their defaults and rewrites its help lines (##). Your own notes are kept when they start with\n"
                "## a single # on the lines above an option.\n",
                {} },
            definitions(), RPS::Settings::LegacyIni{ { directory / "ROCK_Wall_Climbing.ini" }, legacyKeys() });
    }

    Config Config::fromStore(const RPS::Settings::Store& store)
    {
        const Config d{};
        Config config{};
        config.activePath = store.path(Group::Player);
        config.enabled = valueOf(store, "General", "Enabled") == "true";
        const auto& level = valueOf(store, "General", "LogLevel");
        int parsedLevel = d.logLevel;
        std::from_chars(level.data(), level.data() + level.size(), parsedLevel);
        config.logLevel = std::clamp(parsedLevel, 0, 4);
        config.movementScale = number(store, "Climbing", "MovementScale", d.movementScale, kMovementScale);
        config.smoothingSpeed = number(store, "Climbing", "SmoothingSpeed", d.smoothingSpeed, kSmoothing);
        config.maximumHandDeltaGameUnits =
            number(store, "Climbing", "MaximumHandDeltaGameUnits", d.maximumHandDeltaGameUnits, kHandDelta);
        config.maximumTargetSeparationGameUnits = number(store, "Climbing", "MaximumTargetSeparationGameUnits",
            d.maximumTargetSeparationGameUnits, kTargetSeparation);
        config.headClearanceGameUnits =
            number(store, "Climbing", "HeadClearanceGameUnits", d.headClearanceGameUnits, kHeadClearance);
        config.followMovingSurfaces = valueOf(store, "Climbing", "FollowMovingSurfaces") == "true";
        config.launch.enabled = valueOf(store, "Launch", "Enabled") == "true";
        config.launch.historySeconds = number(store, "Launch", "VelocityHistorySeconds", d.launch.historySeconds, kHistory);
        config.launch.directionFilterDegrees =
            number(store, "Launch", "DirectionFilterDegrees", d.launch.directionFilterDegrees, kDirectionFilter);
        config.launch.multiplier = number(store, "Launch", "Multiplier", d.launch.multiplier, kMultiplier);
        config.launch.horizontalBoost = number(store, "Launch", "HorizontalBoost", d.launch.horizontalBoost, kHorizontalBoost);
        config.launch.minimumSpeed =
            number(store, "Launch", "MinimumSpeedGameUnitsPerSecond", d.launch.minimumSpeed, kMinimumSpeed);
        config.launch.maximumSpeed =
            number(store, "Launch", "MaximumSpeedGameUnitsPerSecond", d.launch.maximumSpeed, kMaximumSpeed);
        config.launch.minimumSpeed = (std::min)(config.launch.minimumSpeed, config.launch.maximumSpeed);
        config.detailedTelemetry = valueOf(store, "Diagnostics", "DetailedTelemetry") == "true";
        return config;
    }
}
