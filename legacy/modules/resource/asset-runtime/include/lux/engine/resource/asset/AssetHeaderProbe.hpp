#pragma once
// ============================================================================
//  AssetHeaderProbe — cheap, header-only read of a .luxasset/.luxmodel file's
//  AssetFileHeader (magic + UUID) WITHOUT loading the payload. Type dispatch
//  belongs to the immutable product-composed AssetCodecCatalog.
// ============================================================================

#include <lux/engine/resource/asset/Asset.hpp>          // EAssetType, asset_id_t
#include <lux/engine/resource/asset/AssetSerDeser.hpp>  // AssetFileHeader, asset_magic_number_of

#include <cstdint>
#include <filesystem>
#include <fstream>

namespace lux::asset
{
    /// Magic + UUID from a file's AssetFileHeader (offset 0, fixed-size). magic==0
    /// / nil id on any read error — caller treats that as an unrecognized file.
    struct AssetHeaderProbe
    {
        std::uint32_t magic{ 0 };
        asset_id_t    id{};
    };

    inline AssetHeaderProbe readAssetHeader(const std::filesystem::path& p) noexcept
    {
        AssetHeaderProbe out;
        std::ifstream f(p, std::ios::binary);
        if (!f) return out;
        AssetFileHeader h{};
        f.read(reinterpret_cast<char*>(&h), sizeof(h));
        if (f.gcount() != static_cast<std::streamsize>(sizeof(h))) return out;
        out.magic = h.magic_number;
        out.id    = h.info.id;
        return out;
    }

} // namespace lux::asset
