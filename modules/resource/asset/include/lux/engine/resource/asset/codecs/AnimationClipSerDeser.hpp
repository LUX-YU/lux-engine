#pragma once
#include <lux/engine/resource/asset/visibility.h>
#include <lux/engine/resource/asset/AnimationClipAsset.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>

namespace lux::asset
{
    /**
     * @brief Serialiser for `.luxasset` files containing animation clips.
     *
     * On-disk layout:
     *
     * ```
     * [AssetFileHeader]                                       - generic asset header
     * [info_size bytes]  binary-encoded AnimationClip          - encodeAnimationClipDescription()
     * [data_size bytes]  (always 0 for clips)
     * ```
     *
     * Like SkeletonSerDeser, animation clips have no separate runtime
     * payload — keyframes are pure metadata. The codec keeps the
     * `data_size == 0` discipline so the generic AssetFileHeader
     * contract still holds.
     *
     * `fromFileStream` returns UNSUPPORTED — clips have no standalone
     * external format; production-grade .luxasset blobs are emitted by
     * asset_pipeline from Assimp's aiAnimation extraction.
     */
    class LUX_ASSET_PUBLIC AnimationClipSerDeser
        : public TAssetSerDeser<AnimationClipLoadConfig>
    {
    public:
        explicit AnimationClipSerDeser(std::shared_ptr<AssetManager>);

        /**
         * @brief Decode a complete `.luxasset` memory image into the pure
         *        asset data object (an `AnimationClip`), WITHOUT touching the
         *        AssetManager.
         *
         * Thread-safe, side-effect-free decode entry point for asynchronous
         * asset loading: a worker thread turns a full `.luxasset` image
         * (header + section bytes) into a heap `AnimationClip`, which the main
         * thread later injects into an already-created asset shell. Touches
         * only the byte buffer — never `manager_`.
         *
         * NOTE ON LAYOUT: an `AnimationClip` is pure metadata, so this asset
         * type carries `data_size == 0` and stores the encoded clip in the
         * INFO section (see @ref AnimationClipDescriptionCodec). "Data" here
         * means the asset_data_t payload (the `AnimationClip`), which this
         * function reads from the info section — not the (empty) data section.
         *
         * This is the single source of truth for "bytes → AnimationClip";
         * fromLuxAssetStream() reuses it and only adds AssetInfo + LuxAsset
         * assembly on top.
         *
         * @param bytes Pointer to the first byte of the `.luxasset` image.
         * @param len   Size of the image in bytes.
         * @return The decoded clip on success, or an EAssetError.
         */
        [[nodiscard]] static lux::cxx::expected<
            std::unique_ptr<lux::rdesc::AnimationClip>,
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
