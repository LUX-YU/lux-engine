#pragma once
#include <lux/engine/resource/asset/visibility.h>
#include <lux/engine/resource/asset/ScriptAsset.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>

namespace lux::asset
{
    /**
     * @brief Configuration for ScriptSerDeser::importFromFile.
     *
     * The ".luxasset" path is the canonical script artefact format; loose
     * `.dll` import is supported through @ref accept_loose_library, which
     * lets a developer wrap a hand-built native plugin in an asset on the
     * fly. The accompanying binary manifest (same basename, `.luxscriptmeta`)
     * is then required so the resulting asset has a fully-populated
     * description.
     */
    struct ScriptLoadConfig
    {
        bool accept_loose_library = true;

        /// Non-empty → the imported asset's id derives deterministically from
        /// this seed (the toolchain importer convention): re-importing the same
        /// source keeps the same id, which is what makes hot reload / re-import
        /// keep existing ScriptComponent references valid.
        std::string deterministic_seed;
    };

    /**
     * @brief Serialiser for `.luxasset` files containing script artefacts.
     *
     * On-disk layout:
     *
     * ```
     * [AssetFileHeader]                                   - generic asset header
     * [info_size bytes]   binary-encoded Script metadata  - encodeScriptDescription()
     * [data_size bytes]   payload (lua source / dll / ...)- copied verbatim
     * ```
     *
     * Payload semantics are dictated by `Script::kind`. The metadata blob
     * never contains the payload; the payload lives in the data section so
     * streaming readers can mmap it directly.
     */
    class LUX_ASSET_PUBLIC ScriptSerDeser : public TAssetSerDeser<ScriptLoadConfig>
    {
    public:
        /// Pure asset-data type carried by ScriptAsset (its asset_data_t).
        using DATA = ScriptAsset::asset_data_t;   // == lux::rdesc::Script

        explicit ScriptSerDeser(std::shared_ptr<AssetManager>);

        [[nodiscard]] lux::cxx::expected<AssetIDPair, EAssetError>
        importFromFile(const std::filesystem::path& p) override;

        /**
         * @brief Decode a complete `.luxasset` memory image into the pure
         *        Script DESCRIPTION (asset_data_t), with no AssetManager
         *        involvement whatsoever.
         *
         * Thread-safe, allocation-only entry point for the asynchronous
         * lazy-load pipeline: a worker thread turns the raw `.luxasset` bytes
         * (header + info + data + optional payload tail) into the pure data
         * object, and the main thread later injects it into an already-created
         * asset shell. This touches ONLY the byte buffer — `manager_` is never
         * dereferenced — so it is safe to call off the main thread and is even
         * usable on a manager-less SerDeser.
         *
         * @note For SCRIPT the pure data object (`Script`) is the *description*
         *       stored in the info section. The data section holds the raw
         *       payload bytes (lua source / dll / ...) whose ownership belongs
         *       to the ScriptAsset wrapper, NOT to the Script data object; it
         *       is therefore intentionally not part of what decodeData returns.
         *       fromLuxAssetStream() reuses decodeData() for the description and
         *       slices the payload separately when assembling the ScriptAsset.
         *
         * @param bytes Pointer to the first byte of the `.luxasset` image.
         * @param len   Size of the image in bytes.
         * @return The decoded Script description on success, or an EAssetError.
         */
        [[nodiscard]] static lux::cxx::expected<std::unique_ptr<DATA>, EAssetError>
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
