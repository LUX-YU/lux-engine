#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/AssetCodecCatalog.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>

#include <array>
#include <filesystem>

int main()
{
    static_assert(static_cast<int>(lux::asset::EAssetType::UNKNOWN) >= 0);

    const auto id = lux::asset::asset_id_t::from_string(
        "20000000-0000-4000-8000-000000000001"
    ).value();
    const std::array payload{
        std::byte{0x4c},
        std::byte{0x55},
        std::byte{0x58},
        std::byte{0x01},
    };
    const auto pak = std::filesystem::temp_directory_path()
        / "lux_asset_installed_consumer.luxpak";
    std::string error;
    if (!lux::asset::writePakFile(
            pak,
            {lux::asset::PakWriteEntry{
                id,
                0x54534554u,
                "Installed/Opaque",
                {},
                lux::cxx::SharedBytes<>::copyOf(payload)}},
            "/Game",
            &error
        ))
    {
        return 1;
    }

    const auto inspected = lux::asset::inspectPak(pak);
    const auto provider = lux::asset::PakAssetProvider::loadFromFile(pak);
    const bool valid = inspected
        && inspected->entries.size() == 1u
        && provider
        && (*provider)->resolve("Installed/Opaque") == id
        && (*provider)->open(id);
    std::error_code ignored;
    std::filesystem::remove(pak, ignored);
    return valid ? 0 : 2;
}
