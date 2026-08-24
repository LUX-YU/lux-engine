#include <lux/game/LaunchManifest.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

int main()
{
    const auto manifest_path = std::filesystem::temp_directory_path() /
        ("lux-game-launch-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".toml");

    lux::game::LaunchManifest manifest;
    manifest.game_pak = "game.pak";
    manifest.base_pak = "base.pak";
    manifest.boot_scene = "Scenes/Boot";
    manifest.render_capacity.set(
        lux::render::kActiveInstancesCapacity,
        lux::render::CapacityValue::exact(100'000u));

    assert(manifest.saveToFile(manifest_path));
    const auto roundtrip =
        lux::game::LaunchManifest::loadFromFile(manifest_path);
    assert(roundtrip);
    assert(roundtrip->game_pak == "game.pak");
    assert(roundtrip->base_pak == "base.pak");
    assert(roundtrip->boot_scene == "Scenes/Boot");

    const auto* instances = roundtrip->render_capacity.find(
        lux::render::kActiveInstancesCapacity);
    assert(instances);
    assert(instances->mode == lux::render::CapacityRequestMode::EXPLICIT);
    assert(instances->value == 100'000u);
    assert(!roundtrip->render_capacity.find(
        lux::render::kClassicMeshRecordsCapacity));


    // Schema 4 remains readable during the migration, but all new writes use
    // schema 5 and the product-neutral base_pak key.
    {
        std::ofstream legacy(manifest_path, std::ios::binary | std::ios::trunc);
        legacy << "[runtime]\n"
               << "schema = 4\n"
               << "title = \"Legacy\"\n"
               << "game_pak = \"game.pak\"\n"
               << "engine_pak = \"engine.pak\"\n"
               << "boot_scene = \"Scenes/Boot\"\n"
               << "\n[capacity]\n"
               << "domains = []\n";
    }
    const auto legacy = lux::game::LaunchManifest::loadFromFile(manifest_path);
    assert(legacy);
    assert(legacy->base_pak == "engine.pak");

    // A boot Scene is a product decision and must never be inferred from Pak
    // contents by the reusable Resource layer.
    {
        std::ofstream missing_boot(
            manifest_path,
            std::ios::binary | std::ios::trunc);
        missing_boot << "[runtime]\n"
                     << "schema = 5\n"
                     << "title = \"Missing boot Scene\"\n"
                     << "game_pak = \"game.pak\"\n"
                     << "base_pak = \"base.pak\"\n"
                     << "\n[capacity]\n"
                     << "domains = []\n";
    }
    assert(!lux::game::LaunchManifest::loadFromFile(manifest_path));

    manifest.render_capacity.set(
        lux::render::kActiveInstancesCapacity,
        lux::render::CapacityValue::exact(
            std::numeric_limits<std::uint64_t>::max()));
    assert(!manifest.saveToFile(manifest_path));

    std::error_code remove_error;
    std::filesystem::remove(manifest_path, remove_error);
    return 0;
}
