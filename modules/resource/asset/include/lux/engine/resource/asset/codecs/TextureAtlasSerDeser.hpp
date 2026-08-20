#pragma once
#include <lux/engine/resource/asset/visibility.h>
#include <lux/engine/resource/asset/TextureAtlasAssets.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>

namespace lux::asset
{
    /**
     * @brief Serialisers for `.luxasset` image-atlas / image-anim-clip files.
     *
     * On-disk layout (both types, the AnimationClip pure-metadata pattern):
     *
     * ```
     * [AssetFileHeader]                                    - generic asset header
     * [info_size bytes]  binary-encoded description        - TextureAtlasDescriptionCodec
     * [data_size bytes]  (always 0 — pure metadata)
     * ```
     *
     * `fromFileStream` returns UNSUPPORTED for both: there is no canonical
     * standalone external format yet (TexturePacker-style JSON import is a
     * future importer concern); production blobs are authored/exported through
     * the asset pipeline.
     */
    class LUX_ASSET_PUBLIC TextureAtlasSerDeser
        : public TAssetSerDeser<TextureAtlasLoadConfig>
    {
    public:
        explicit TextureAtlasSerDeser(std::shared_ptr<AssetManager>);

        /// Thread-safe, manager-free "bytes → TextureAtlas" (the lazy-inject
        /// decode entry point; see AnimationClipSerDeser::decodeData).
        [[nodiscard]] static lux::cxx::expected<
            std::unique_ptr<lux::rdesc::TextureAtlas>,
            EAssetError>
        decodeData(const void* bytes, std::size_t len) noexcept;

    protected:
        lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
        fromFileStream(std::ifstream& ifs) override;

        lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
        fromLuxAssetStream(std::istream& ifs) override;

        EAssetError
        exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& ofs) override;
    };

    class LUX_ASSET_PUBLIC FlipbookClipSerDeser
        : public TAssetSerDeser<FlipbookClipLoadConfig>
    {
    public:
        explicit FlipbookClipSerDeser(std::shared_ptr<AssetManager>);

        [[nodiscard]] static lux::cxx::expected<
            std::unique_ptr<lux::rdesc::FlipbookClip>,
            EAssetError>
        decodeData(const void* bytes, std::size_t len) noexcept;

    protected:
        lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
        fromFileStream(std::ifstream& ifs) override;

        lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
        fromLuxAssetStream(std::istream& ifs) override;

        EAssetError
        exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& ofs) override;
    };
}
