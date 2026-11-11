// ROCK_Wall_Climbing.toml contracts: the example file is what the mod writes,
// documented defaults are the compiled defaults, values are clamped, and the
// INI of earlier versions migrates once, section by section, and is deleted.
#include "support/Config.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace
{
    namespace fs = std::filesystem;
    using rock_wall_climbing::Config;

    std::string read(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        assert(input.good());
        return { std::istreambuf_iterator<char>(input), {} };
    }

    void write(const fs::path& path, std::string_view text)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
        assert(output.good());
    }

    std::string body(std::string text)
    {
        std::erase(text, '\r');
        const auto start = text.find("\n## === ");
        assert(start != std::string::npos);
        return text.substr(start);
    }

    constexpr std::string_view kExampleHeader =
        "## ROCK_Wall_Climbing_example.toml - every option with its default and help\n"
        "## Reference only: the mod never reads this file. The first time it starts it writes this same text as\n"
        "## ROCK_Wall_Climbing.toml in Documents\\My Games\\Fallout4VR\\Mods_Config\\ROCK_Wall_Climbing.\n";

    bool sameValues(const Config& a, const Config& b)
    {
        return a.enabled == b.enabled && a.logLevel == b.logLevel && a.movementScale == b.movementScale &&
               a.smoothingSpeed == b.smoothingSpeed && a.maximumHandDeltaGameUnits == b.maximumHandDeltaGameUnits &&
               a.maximumTargetSeparationGameUnits == b.maximumTargetSeparationGameUnits &&
               a.headClearanceGameUnits == b.headClearanceGameUnits && a.followMovingSurfaces == b.followMovingSurfaces &&
               a.launch.enabled == b.launch.enabled && a.launch.historySeconds == b.launch.historySeconds &&
               a.launch.directionFilterDegrees == b.launch.directionFilterDegrees &&
               a.launch.multiplier == b.launch.multiplier && a.launch.horizontalBoost == b.launch.horizontalBoost &&
               a.launch.minimumSpeed == b.launch.minimumSpeed && a.launch.maximumSpeed == b.launch.maximumSpeed &&
               a.launch.gravity == b.launch.gravity && a.catchWithHeldGrip == b.catchWithHeldGrip &&
               a.detailedTelemetry == b.detailedTelemetry;
    }
}

int main(int argc, char** argv)
{
    assert(argc == 2 || argc == 3);
    const fs::path root(ROCK_WALL_CLIMBING_SETTINGS_TEST_DIR);
    std::error_code ignored;
    fs::remove_all(root, ignored);

    const auto fresh = root / "fresh";
    fs::create_directories(fresh);
    auto store = Config::makeStore(fresh);
    store->load(true);
    assert(store->messages().empty());
    const auto written = body(read(fresh / "ROCK_Wall_Climbing.toml"));
    const auto examples = fs::path(argv[argc - 1]);
    if (argc == 3 && std::string_view(argv[1]) == "--write-examples") {
        write(examples / "ROCK_Wall_Climbing_example.toml", std::string(kExampleHeader) + written);
        fs::remove_all(root, ignored);
        return 0;
    }
    assert(body(read(examples / "ROCK_Wall_Climbing_example.toml")) == written);
    for (const auto& setting : store->settings())
        assert(setting.specified && setting.value == setting.defaultValue);
    assert(sameValues(Config::fromStore(*store), Config{}));

    // Clamped as documented; the launch minimum never exceeds its maximum.
    write(fresh / "ROCK_Wall_Climbing.toml",
        "[General]\nLogLevel = 9\n[Climbing]\nMovementScale = 5.0\n[Launch]\nMinimumSpeedGameUnitsPerSecond = 400.0\n"
        "TopSpeedGameUnitsPerSecond = 100.0\n");
    store->load(false);
    auto loaded = Config::fromStore(*store);
    assert(loaded.logLevel == 4 && loaded.movementScale == 2.0f);
    assert(loaded.launch.maximumSpeed == 100.0f && loaded.launch.minimumSpeed == 100.0f);

    // Both [General] and [Launch] bEnabled carry over to their own option.
    const auto upgrade = root / "upgrade";
    fs::create_directories(upgrade);
    write(upgrade / "ROCK_Wall_Climbing.ini",
        "[General]\r\nbEnabled = false\r\niLogLevel = 2\r\n[Climbing]\r\nfMovementScale = 1.4\r\n"
        "[Launch]\r\nbEnabled = true\r\nfMultiplier = 0.8\r\n[Diagnostics]\r\nbDetailedTelemetry = true\r\n");
    store = Config::makeStore(upgrade);
    store->load(true);
    assert(!fs::exists(upgrade / "ROCK_Wall_Climbing.ini"));
    loaded = Config::fromStore(*store);
    assert(!loaded.enabled && loaded.launch.enabled);
    assert(loaded.movementScale == 1.4f && loaded.launch.multiplier == 0.8f && loaded.detailedTelemetry);

    fs::remove_all(root, ignored);
    return 0;
}
