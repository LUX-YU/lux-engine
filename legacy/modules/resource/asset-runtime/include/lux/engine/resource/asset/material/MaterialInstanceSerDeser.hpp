#pragma once
#include <lux/engine/resource/asset/visibility.h>
#include <lux/engine/resource/asset/material/MaterialInstanceAsset.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>

namespace lux::asset
{
    /// Material instances are authored in the editor (no external source format),
    /// so this config is empty — only the .luxasset paths are implemented.
    struct MaterialInstanceLoadConfig
    {
    };

    /// (De)serializes MaterialInstanceAsset to/from the engine .luxasset format.
    /// Like the other SerDesers: [AssetFileHeader][info blob], payload packed into
    /// the info section (data_size == 0). See the .cpp for the versioned layout.
    class LUX_ASSET_PUBLIC MaterialInstanceSerDeser : public TAssetSerDeser<MaterialInstanceLoadConfig>
    {
    public:
        explicit MaterialInstanceSerDeser(std::shared_ptr<AssetManager> manager);
        ~MaterialInstanceSerDeser() override;

        /// Decode the PURE payload (MaterialInstanceData = the asset_data_t) out of
        /// a complete .luxasset memory image — header + sections — WITHOUT touching
        /// the AssetManager, registering, or constructing a LuxAsset shell. This is
        /// the thread-safe entry point for asynchronous loading: a worker thread
        /// decodes the bytes off the main thread, the main thread then injects the
        /// returned data into a pre-existing asset shell.
        ///
        /// NOTE: a material-instance has NO separate data section — its overrides are
        /// packed into the .luxasset INFO section (data_size == 0). decodeData still
        /// applies: it locates that section in the image and decodes it. fromLuxAsset-
        /// Stream() now reuses this for the payload (DRY) and only adds the AssetInfo
        /// + LuxAsset assembly on top.
        ///
        /// noexcept: all error paths go through expected<>; no exceptions escape.
        [[nodiscard]] static lux::cxx::expected<std::unique_ptr<MaterialInstanceData>, EAssetError>
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
