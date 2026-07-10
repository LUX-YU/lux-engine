#pragma once
#include <lux/engine/asset/Sprite2DAssets.hpp>
#include <lux/engine/asset/AssetSerDeser.hpp>

namespace lux::asset
{
    /**
     * @brief Serialisers for `.luxasset` sprite-atlas / sprite-anim-clip files.
     *
     * On-disk layout (both types, the AnimationClip pure-metadata pattern):
     *
     * ```
     * [AssetFileHeader]                                    - generic asset header
     * [info_size bytes]  binary-encoded description        - Sprite2DDescriptionCodec
     * [data_size bytes]  (always 0 — pure metadata)
     * ```
     *
     * `fromFileStream` returns UNSUPPORTED for both: there is no canonical
     * standalone external format yet (TexturePacker-style JSON import is a
     * future importer concern); production blobs are authored/exported through
     * the asset pipeline.
     */
    class LUX_RESOURCE_PUBLIC SpriteAtlasSerDeser
        : public TAssetSerDeser<SpriteAtlasLoadConfig>
    {
    public:
        explicit SpriteAtlasSerDeser(std::shared_ptr<AssetManager>);

        /// Thread-safe, manager-free "bytes → SpriteAtlas" (the lazy-inject
        /// decode entry point; see AnimationClipSerDeser::decodeData).
        [[nodiscard]] static lux::cxx::expected<std::unique_ptr<SpriteAtlas>, EAssetError>
        decodeData(const void* bytes, std::size_t len) noexcept;

    protected:
        lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
        fromFileStream(std::ifstream& ifs) override;

        lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
        fromLuxAssetStream(std::istream& ifs) override;

        EAssetError
        exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& ofs) override;
    };

    class LUX_RESOURCE_PUBLIC SpriteAnimClipSerDeser
        : public TAssetSerDeser<SpriteAnimClipLoadConfig>
    {
    public:
        explicit SpriteAnimClipSerDeser(std::shared_ptr<AssetManager>);

        [[nodiscard]] static lux::cxx::expected<std::unique_ptr<SpriteAnimClip>, EAssetError>
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
