#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>

int main()
{
    namespace fs = std::filesystem;
    using namespace lux::asset;

    const auto id = asset_id_t::from_string(
        "10000000-0000-4000-8000-000000000001"
    ).value();
    const std::array payload{
        std::byte{0x4c},
        std::byte{0x55},
        std::byte{0x58},
        std::byte{0x01},
    };
    const auto nonce = std::chrono::steady_clock::now()
        .time_since_epoch()
        .count();
    const auto pak = fs::temp_directory_path()
        / ("lux_asset_pak_public_" + std::to_string(nonce) + ".luxpak");

    std::string error;
    const auto bytes = lux::cxx::SharedBytes<>::copyOf(payload);
    if (!writePakFile(
        pak,
        {PakWriteEntry{id, 0x54534554u, "Opaque/Record", {}, bytes}},
        "/Game",
        &error
    ))
    {
        std::fprintf(stderr, "writePakFile failed: %s\n", error.c_str());
        return 1;
    }

    const auto inspected = inspectPak(pak);
    if (!inspected)
    {
        std::fprintf(stderr, "inspectPak failed: %s\n", inspected.error().c_str());
        return 1;
    }
    if (inspected->mount_hint != "/Game"
        || inspected->entries.size() != 1u
        || inspected->entries.front().id != id
        || inspected->entries.front().magic_number != 0x54534554u)
    {
        std::fputs("inspectPak returned an unexpected manifest\n", stderr);
        return 1;
    }

    const auto provider = PakAssetProvider::loadFromFile(pak);
    if (!provider)
    {
        std::fprintf(
            stderr,
            "PakAssetProvider failed: %s\n",
            provider.error().c_str()
        );
        return 1;
    }
    if ((*provider)->resolve("Opaque/Record") != id)
    {
        std::fputs("PakAssetProvider resolve failed\n", stderr);
        return 1;
    }
    const auto opened = (*provider)->open(id);
    if (!opened || opened->bytes.size() != payload.size())
    {
        std::fputs("PakAssetProvider open failed\n", stderr);
        return 1;
    }

    if (writePakFile(
        pak,
        {PakWriteEntry{{}, 0x54534554u, "Opaque/Nil", {}, bytes}},
        "/Game",
        &error
    ))
        return 1;
    if (writePakFile(
        pak,
        {PakWriteEntry{id, 0u, "Opaque/ZeroMagic", {}, bytes}},
        "/Game",
        &error
    ))
        return 1;
    if (writePakFile(
        pak,
        {
            PakWriteEntry{id, 0x54534554u, "Opaque/A", {}, bytes},
            PakWriteEntry{id, 0x54534554u, "Opaque/B", {}, bytes},
        },
        "/Game",
        &error
    ))
        return 1;

    std::error_code ignored;
    fs::remove(pak, ignored);
    return 0;
}
