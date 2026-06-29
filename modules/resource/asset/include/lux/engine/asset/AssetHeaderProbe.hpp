#pragma once
// ============================================================================
//  AssetHeaderProbe — cheap, header-only read of a .luxasset/.luxmodel file's
//  AssetFileHeader (magic + UUID) WITHOUT loading the payload, plus the single
//  runtime magic->EAssetType table. This is the SSOT for "what type is this
//  file" — the editor (AssetBrowser/AssetRegistry), the import pipeline and
//  the pak cooker all dispatch through here so the table can never fork.
// ============================================================================

#include "Asset.hpp"          // EAssetType, asset_id_t
#include "AssetSerDeser.hpp"  // AssetFileHeader, asset_magic_number_of

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

    /// Map a 4-byte header magic to its EAssetType (mirrors asset_magic_number_of;
    /// kept in sync by hand because the specs aren't runtime-queryable).
    inline EAssetType assetTypeOfMagic(std::uint32_t magic) noexcept
    {
        if (magic == asset_magic_number_of<EAssetType::MATERIAL>::value)          return EAssetType::MATERIAL;
        if (magic == asset_magic_number_of<EAssetType::MATERIAL_INSTANCE>::value) return EAssetType::MATERIAL_INSTANCE;
        if (magic == asset_magic_number_of<EAssetType::MESH>::value)              return EAssetType::MESH;
        if (magic == asset_magic_number_of<EAssetType::MODEL>::value)             return EAssetType::MODEL;
        if (magic == asset_magic_number_of<EAssetType::TEXTURE>::value)           return EAssetType::TEXTURE;
        if (magic == asset_magic_number_of<EAssetType::SHADER>::value)            return EAssetType::SHADER;
        if (magic == asset_magic_number_of<EAssetType::SCRIPT>::value)            return EAssetType::SCRIPT;
        if (magic == asset_magic_number_of<EAssetType::SKELETON>::value)          return EAssetType::SKELETON;
        if (magic == asset_magic_number_of<EAssetType::ANIMATION_CLIP>::value)    return EAssetType::ANIMATION_CLIP;
        return EAssetType::UNKNOWN;
    }

} // namespace lux::asset
