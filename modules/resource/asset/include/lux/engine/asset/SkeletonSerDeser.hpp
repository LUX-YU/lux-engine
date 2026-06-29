#pragma once
#include <lux/engine/asset/SkeletonAsset.hpp>
#include <lux/engine/asset/AssetSerDeser.hpp>

namespace lux::asset
{
    /**
     * @brief Serialiser for `.luxasset` files containing skeleton resources.
     *
     * On-disk layout:
     *
     * ```
     * [AssetFileHeader]                                       - generic asset header
     * [info_size bytes]  binary-encoded Skeleton description  - encodeSkeletonDescription()
     * [data_size bytes]  (always 0 for skeletons)
     * ```
     *
     * A Skeleton has no separate runtime payload — bones, bind transforms
     * and inverse bind transforms are all metadata. The codec keeps the
     * `data_size == 0` discipline so the generic AssetFileHeader contract
     * still holds.
     *
     * `fromFileStream` returns UNSUPPORTED — Skeleton has no external
     * format on its own; production-grade .luxasset blobs are emitted by
     * the asset_pipeline tool from Assimp imports (where the skeleton is
     * extracted alongside the mesh).
     */
    class LUX_RESOURCE_PUBLIC SkeletonSerDeser : public TAssetSerDeser<SkeletonLoadConfig>
    {
    public:
        explicit SkeletonSerDeser(std::shared_ptr<AssetManager>);

        /**
         * @brief Decode the PURE Skeleton data out of a complete `.luxasset`
         *        memory image, without touching the AssetManager.
         *
         * This is the thread-safe, side-effect-free decode primitive used by
         * the asynchronous asset-loading path: a worker thread is handed a
         * complete `.luxasset` image (header + info + data + payloads) and
         * turns it into the raw asset data object (here `rdesc::Skeleton`),
         * which the main thread later injects into an already-created asset
         * shell. It only reads the byte stream — it NEVER dereferences
         * `manager_`, registers, or mutates any shared state — so it is safe
         * to call off the main thread.
         *
         * For skeletons the description blob lives in the INFO section
         * (`data_size == 0` by design — see the class doc), so this slices
         * `[info_offset, info_offset + info_size)` and runs the same codec as
         * fromLuxAssetStream(); fromLuxAssetStream() delegates the decode here
         * to avoid duplicating the logic.
         *
         * @param bytes Pointer to the first byte of the `.luxasset` image.
         * @param len   Size of the image in bytes.
         * @return The decoded Skeleton on success, or an EAssetError.
         */
        [[nodiscard]] static lux::cxx::expected<std::unique_ptr<Skeleton>, EAssetError>
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
