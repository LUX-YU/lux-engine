#pragma once
#include <lux/engine/resource/asset/visibility.h>
#include <lux/engine/resource/asset/material/MaterialAsset.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>

namespace lux::asset
{
    /// Materials have no external source format (they are authored in the
    /// editor's node graph), so this config is empty — only the .luxasset
    /// (de)serialization paths are implemented.
    struct MaterialLoadConfig
    {
    };

    /// (De)serializes MaterialAsset to/from the engine .luxasset format.
    /// Layout mirrors the other SerDesers: [AssetFileHeader][info blob], with the
    /// whole payload packed into the info section (data_size == 0). See the .cpp
    /// for the versioned blob layout.
    class LUX_ASSET_PUBLIC MaterialSerDeser : public TAssetSerDeser<MaterialLoadConfig>
    {
    public:
        explicit MaterialSerDeser(std::shared_ptr<AssetManager> manager);
        ~MaterialSerDeser() override;

        /// Decode a complete .luxasset MATERIAL memory image into the PURE asset
        /// data object (MaterialAsset::asset_data_t == MaterialData), WITHOUT the
        /// AssetManager / registration / AssetInfo wrapper. Reads the header to
        /// locate the payload (Material packs everything into the info section,
        /// data_size == 0), then runs the exact field decode fromLuxAssetStream
        /// uses (so the two never drift). Touches only the byte image — never
        /// manager_ — so a background worker can call it off the main thread for
        /// the async lazy-load path (the main thread injects the result into a
        /// pre-existing asset shell). noexcept: any throwing call inside is
        /// caught and surfaced as an EAssetError.
        [[nodiscard]] static lux::cxx::expected<std::unique_ptr<MaterialData>, EAssetError>
        decodeData(const void* bytes, std::size_t len) noexcept;

    protected:
        lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
        fromFileStream(std::ifstream& ifs) override;

        lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
        fromLuxAssetStream(std::istream& ifs) override;

        EAssetError
        exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& ofs) override;
    };

} // namespace lux::asset
