#pragma once
#include <memory>
#include <span>
#include <vector>
#include <cstddef>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/resource/asset/visibility.h>

#include <lux/engine/resource/asset/Asset.hpp>

namespace lux::asset
{
    /**
     * @brief Asset version constant for version 1.
     */
    static inline constexpr asset_version_t asset_version_v1 = 20250128;
    /// v2: AssetInfo gained display_name / source_path / source_mtime, which
    /// enlarges AssetFileHeader. v1 files remain READABLE — loadHeader() detects
    /// the version from the stable {magic,version} prefix and migrates the v1
    /// layout into the current one (see namespace `compat` below). New writes
    /// are always current_asset_version.
    static inline constexpr asset_version_t asset_version_v2 = 20260606;

    template<EAssetType T> struct asset_magic_number_of;
	template<> struct asset_magic_number_of<EAssetType::MESH>      { static inline constexpr uint32_t value = 0x01309141; };
	template<> struct asset_magic_number_of<EAssetType::MODEL>     { static inline constexpr uint32_t value = 0x01309142; };
	template<> struct asset_magic_number_of<EAssetType::TEXTURE>   { static inline constexpr uint32_t value = 0x01309143; };
	template<> struct asset_magic_number_of<EAssetType::SHADER>    { static inline constexpr uint32_t value = 0x01309144; };
	template<> struct asset_magic_number_of<EAssetType::SCRIPT>         { static inline constexpr uint32_t value = 0x01309145; };
	template<> struct asset_magic_number_of<EAssetType::SKELETON>       { static inline constexpr uint32_t value = 0x01309146; };
	template<> struct asset_magic_number_of<EAssetType::ANIMATION_CLIP> { static inline constexpr uint32_t value = 0x01309147; };
	template<> struct asset_magic_number_of<EAssetType::MATERIAL>       { static inline constexpr uint32_t value = 0x01309148; };
	template<> struct asset_magic_number_of<EAssetType::MATERIAL_INSTANCE> { static inline constexpr uint32_t value = 0x01309149; };
	template<> struct asset_magic_number_of<EAssetType::TEXTURE_ATLAS>       { static inline constexpr uint32_t value = 0x0130914A; };
	template<> struct asset_magic_number_of<EAssetType::FLIPBOOK_CLIP>   { static inline constexpr uint32_t value = 0x0130914B; };
	template<> struct asset_magic_number_of<EAssetType::FLOW_GRAPH>         { static inline constexpr uint32_t value = 0x0130914C; };

    /**
     * @brief Current asset version used in the system.
     */
    static inline constexpr asset_version_t current_asset_version = asset_version_v2;

    /**
     * @brief Header structure for asset files.
     *
     * This structure defines the binary layout of asset file headers,
     * containing metadata about where different sections of the file are located
     * and how large they are.
     */
    struct AssetFileHeader
    {
        uint32_t  magic_number; ///< Magic number used to verify file format validity
        asset_version_t version;
        uint64_t  info_offset;  ///< Byte offset from file start to asset information section
        uint64_t  info_size;    ///< Size in bytes of the asset information section
        uint64_t  data_offset;  ///< Byte offset from file start to asset data section
        uint64_t  data_size;    ///< Size in bytes of the asset data section
		AssetInfo info;      ///< Asset metadata structure
    };

    // ── Auxiliary payload region (extensible tagged binary slots) ────────
    //
    // Zero or more optional payloads may be appended AFTER the per-type data
    // section, each as a 16-byte PayloadBlockHeader followed by its bytes:
    //     [header][info][data] [block0][block1]...[blockN]
    //     block = { uint64 tag; uint64 size; } + size bytes
    //
    // The region runs from (data_offset + data_size) to EOF, so it needs NO
    // header/version change: the bounds come from the existing header + file
    // size. Old files (region empty) read as zero payloads, and per-type
    // readers are untouched (they slice only [data_offset, data_offset+
    // data_size] and tolerate trailing bytes). Written/extracted centrally by
    // AssetSerDeser::exportAsLuxAsset / fromLuxAsset, so no per-type SerDeser
    // is involved. The `tag` meaning is owned by the consuming feature, not by
    // the asset module (see LuxAsset payload accessors).
    struct PayloadBlockHeader
    {
        payload_tag_t tag;   ///< Feature-defined magic identifying the payload.
        uint64_t      size;  ///< Bytes of the payload immediately following this header.
    };
    static_assert(std::is_trivially_copyable_v<PayloadBlockHeader>);
    static_assert(sizeof(PayloadBlockHeader) == 16);

    // ── Backward-compatibility: frozen older on-disk layouts ─────────────
    //
    // The asset header embeds AssetInfo by value, so growing AssetInfo changes
    // sizeof(AssetFileHeader) and every section offset — old files would be
    // unreadable. To keep them readable we freeze each retired layout here and
    // migrate it into the CURRENT structures at load time. The {magic, version}
    // prefix is layout-stable across all versions, so the reader can identify
    // the format before committing to a layout (see loadHeader / loadHeaderRaw).
    //
    // Rules:
    //   * NEVER modify a struct in here — it mirrors bytes already on disk.
    //   * To retire the current layout, copy it here under the next Vn name,
    //     add an upgradeAssetInfo overload, and a `case` in the loaders.
    namespace compat
    {
        /// v1 (asset_version_v1) AssetInfo: before display_name/source_path/
        /// source_mtime existed.
        struct AssetInfoV1
        {
            asset_id_t id;
            EAssetType type;
            uint64_t   date;
        };
        static_assert(std::is_trivially_copyable_v<AssetInfoV1>);
        static_assert(std::is_standard_layout_v<AssetInfoV1>);

        /// v1 AssetFileHeader: same leading 6 fields, but embeds AssetInfoV1.
        struct AssetFileHeaderV1
        {
            uint32_t        magic_number;
            asset_version_t version;
            uint64_t        info_offset;
            uint64_t        info_size;
            uint64_t        data_offset;
            uint64_t        data_size;
            AssetInfoV1     info;
        };
        static_assert(std::is_trivially_copyable_v<AssetFileHeaderV1>);

        /// Migrate a v1 AssetInfo to the current layout; provenance fields that
        /// did not exist in v1 are left zero/empty.
        inline AssetInfo upgradeAssetInfo(const AssetInfoV1& v1) noexcept
        {
            AssetInfo out{};
            out.id   = v1.id;
            out.type = v1.type;
            out.date = v1.date;
            return out;
        }
    }

    /// Serialized header width for a supported AssetFileHeader version.
    /// Decoders must validate section offsets against the on-disk version,
    /// not against sizeof(the current in-memory header).
    [[nodiscard]] inline constexpr std::size_t assetFileHeaderSize(
        asset_version_t version
    ) noexcept
    {
        if (version == current_asset_version) return sizeof(AssetFileHeader);
        if (version == asset_version_v1) return sizeof(compat::AssetFileHeaderV1);
        return 0u;
    }

    class AssetManager;
    
    /**
     * @brief Type alias for asset pointer and ID pair.
     */
    using AssetIDPair = std::pair<LuxAsset*, asset_id_t>;

    /**
     * @brief Abstract base class for asset serialization and deserialization operations.
     *
     * The AssetSerDeser class provides a common interface for importing assets from
     * external file formats and converting them to/from the engine's internal asset format.
     * Derived classes implement specific serialization logic for different asset types
     * such as textures, meshes, materials, and models.
     */
    class LUX_ASSET_PUBLIC AssetSerDeser
    {
    public:
        /**
         * @brief Virtual destructor for proper cleanup of derived classes.
         */
        virtual ~AssetSerDeser() = default;

        /**
         * @brief Constructs an AssetSerDeser with the specified AssetManager.
         *
         * @param manager Shared pointer to the AssetManager that will manage created assets
         */
        explicit AssetSerDeser(std::shared_ptr<AssetManager> manager)
            : manager_(std::move(manager)) {}

        /**
         * @brief Imports an asset from an external file format.
         *
         * This method converts an asset from an external file (e.g., PNG, GLTF, OBJ)
         * into the engine's internal format and registers it with the AssetManager.
         *
         * @param external_path Absolute filesystem path to the external asset file
         * @return Expected result containing asset pointer and ID on success, or error code on failure
         */
        [[nodiscard]] virtual lux::cxx::expected<AssetIDPair, EAssetError>
        importFromFile(const std::filesystem::path& external_path);

        /**
         * @brief Imports an asset from raw memory data.
         *
         * This method converts asset data from a memory buffer into the engine's
         * internal format and registers it with the AssetManager.
         *
         * @param data Pointer to the raw asset data in memory
         * @param len Size of the memory data in bytes
         * @return Expected result containing asset pointer and ID on success, or error code on failure
         */
        [[nodiscard]] virtual lux::cxx::expected<AssetIDPair, EAssetError>
        importFromMemory(const void* data, size_t len);

        /**
         * @brief Loads an asset from the engine's internal asset file format.
         *
         * Reads a previously serialized asset from a .luxasset file and reconstructs
         * the asset object. The file path is relative to the AssetManager's working directory.
         *
         * @param path Relative path to the .luxasset file within the working directory
         * @return Expected result containing asset pointer and ID on success, or error code on failure
         */
        [[nodiscard]] virtual lux::cxx::expected<AssetIDPair, EAssetError>
        fromLuxAsset(const std::filesystem::path& path);

        /**
         * @brief Loads an asset from a .luxasset image held in memory.
         *
         * Byte-identical semantics to fromLuxAsset(): the buffer must contain a
         * complete .luxasset file (header + sections + optional payload tail).
         * Used by in-memory providers (pak entries, embedded builtins) so they
         * run through the exact same stream readers as loose files.
         *
         * @param data Pointer to the first byte of the .luxasset image
         * @param len  Size of the image in bytes
         * @return Expected result containing asset pointer and ID on success, or error code on failure
         */
        [[nodiscard]] lux::cxx::expected<AssetIDPair, EAssetError>
        fromLuxAssetMemory(const void* data, size_t len);

        /**
         * @brief Parse a .luxasset memory image into a LuxAsset WITHOUT
         *        registering it with the AssetManager.
         *
         * Byte-identical to the PARSE half of fromLuxAssetMemory() — the
         * per-type stream reader (fromLuxAssetStream) plus auxiliary payloads —
         * minus the final submitAsset() registration. This is the thread-safe
         * entry point for asynchronous asset loading: a worker thread parses
         * off the main thread (fromLuxAssetStream is contracted to touch only
         * the stream, never the manager — so manager_ is never dereferenced
         * here), then the main thread registers the returned asset via
         * AssetManager::registerAsset(). fromLuxAssetMemory() is now just this
         * plus submit, so the loose/pak/sync path is unchanged.
         *
         * @param data Pointer to the first byte of the .luxasset image.
         * @param len  Size of the image in bytes.
         * @return The parsed (unregistered) asset on success, or an error.
         */
        [[nodiscard]] lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
        parseLuxAssetMemory(const void* data, size_t len);

        /**
         * @brief Writes an asset associated with a given asset ID to a luxasset formatted file.
         *
         * The file will be written to a path relative to the AssetManager's working directory.
         *
         * @param asset The asset ID corresponding to the asset to be written.
         * @param path Relative path where the luxasset formatted file should be output.
         * @return EAssetError indicating whether the operation was successful.
         */
        virtual EAssetError
        exportAsLuxAsset(const asset_id_t& asset, const std::filesystem::path& path);

        /**
         * @brief Retrieves a reference to the AssetManager.
         *
         * @return Reference to the AssetManager.
         */
        AssetManager& manager() { return *manager_; }

        /**
         * @brief Retrieves a constant reference to the AssetManager.
         *
         * @return Constant reference to the AssetManager.
         */
        [[nodiscard]] const AssetManager& manager() const { return *manager_; }

        /**
         * @brief Creates a header for an asset file.
         *
         * This templated function constructs an AssetFileHeader using the provided asset information
         * and the given data size. It then serializes the header into a byte vector.
         *
         * @tparam InfoType The type of the asset info; must have a standard layout.
         * @param info Asset information (currently not directly used in header creation).
         * @param data_size Size of the asset data in bytes.
         * @return A vector of bytes representing the serialized header.
         */
		template<typename InfoType, EAssetType T>  
        requires (std::is_standard_layout_v<InfoType>&& std::is_trivially_copyable_v<AssetFileHeader>)
        static std::vector<std::byte> makeHeader(const AssetInfo& ainfo, const InfoType& info, size_t data_size)
        {
            const AssetFileHeader header{
                .magic_number = asset_magic_number_of<T>::value,
                .version      = current_asset_version,
                .info_offset  = sizeof(AssetFileHeader),
                .info_size    = sizeof(InfoType),
                .data_offset  = sizeof(AssetFileHeader) + sizeof(InfoType),
                .data_size    = data_size,
                .info         = ainfo
            };

            std::vector<std::byte> bytes(sizeof(AssetFileHeader) + sizeof(InfoType));
            std::memcpy(bytes.data(), &header, sizeof(AssetFileHeader));
            std::memcpy(bytes.data() + sizeof(AssetFileHeader), &info, sizeof(InfoType));
            return bytes;
        }

        template<EAssetType T>
        static std::vector<std::byte> makeHeaderRaw(const AssetInfo& ainfo, size_t info_size, size_t data_size)
        {
            const AssetFileHeader header{
                .magic_number = asset_magic_number_of<T>::value,
                .version      = current_asset_version,
                .info_offset  = sizeof(AssetFileHeader),
                .info_size    = info_size,
                .data_offset  = sizeof(AssetFileHeader) + info_size,
                .data_size    = data_size,
                .info         = ainfo
            };

            std::vector<std::byte> bytes(sizeof(AssetFileHeader));
            std::memcpy(bytes.data(), &header, sizeof(AssetFileHeader));
            return bytes;
        }

        /**
         * @brief Loads the header and asset information from a serialized byte vector.
         *
         * This templated function validates the header, ensures the file size is sufficient,
         * and then extracts the asset information into the provided structure.
         *
         * @tparam InfoType The type of the asset info; must have a standard layout.
         * @param data Byte vector containing the serialized header and asset info.
         * @param header Reference to an AssetFileHeader structure to populate.
         * @param info Reference to an InfoType structure to populate with asset info.
         * @return EAssetError::SUCCESS on success, or an appropriate error code on failure.
         */
        template<typename InfoType, EAssetType T> requires std::is_standard_layout_v<InfoType>
        static EAssetError loadHeader(std::span<const std::byte> data, AssetFileHeader& header, InfoType& info)
        {
            uint32_t        magic{};
            asset_version_t version{};
            if (const auto e = peekHeaderPrefix(data, magic, version); e != EAssetError::SUCCESS)
                return e;
            if (magic != asset_magic_number_of<T>::value)
                return EAssetError::WRONG_FILE_HEADER;

            if (version == current_asset_version)
            {
                if (data.size() < sizeof(AssetFileHeader))
                    return EAssetError::ABNORMAL_FILE_SIZE;
                std::memcpy(&header, data.data(), sizeof(AssetFileHeader));
                if (header.info_size != sizeof(InfoType))
                    return EAssetError::WRONG_FILE_HEADER;
                if (header.info_offset != sizeof(AssetFileHeader))
                    return EAssetError::WRONG_FILE_HEADER;
                if (header.data_offset != header.info_offset + header.info_size)
                    return EAssetError::WRONG_FILE_HEADER;
                if (data.size() < header.info_offset + header.info_size)
                    return EAssetError::ABNORMAL_FILE_SIZE;
                if (data.size() < header.data_offset + header.data_size)
                    return EAssetError::ABNORMAL_FILE_SIZE;
                std::memcpy(&info, data.data() + header.info_offset, sizeof(InfoType));
                return EAssetError::SUCCESS;
            }
            if (version == asset_version_v1)
                return loadHeaderV1<InfoType, T>(data, header, info);

            return EAssetError::UNSUPPORTED_VERSION;
        }

        template<EAssetType T>
        static EAssetError loadHeaderRaw(std::span<const std::byte> data, AssetFileHeader& header)
        {
            uint32_t        magic{};
            asset_version_t version{};
            if (const auto e = peekHeaderPrefix(data, magic, version); e != EAssetError::SUCCESS)
                return e;
            if (magic != asset_magic_number_of<T>::value)
                return EAssetError::WRONG_FILE_HEADER;

            if (version == current_asset_version)
            {
                if (data.size() < sizeof(AssetFileHeader))
                    return EAssetError::ABNORMAL_FILE_SIZE;
                std::memcpy(&header, data.data(), sizeof(AssetFileHeader));
                return EAssetError::SUCCESS;
            }
            if (version == asset_version_v1)
            {
                if (data.size() < sizeof(compat::AssetFileHeaderV1))
                    return EAssetError::ABNORMAL_FILE_SIZE;
                compat::AssetFileHeaderV1 v1{};
                std::memcpy(&v1, data.data(), sizeof(v1));
                migrateV1HeaderInto(v1, header);
                return EAssetError::SUCCESS;
            }
            return EAssetError::UNSUPPORTED_VERSION;
        }

    private:
        /// Read the layout-stable {magic, version} prefix shared by every
        /// format version. Lets the loaders identify the format before
        /// committing to a struct layout.
        static EAssetError peekHeaderPrefix(std::span<const std::byte> data,
                                            uint32_t& magic, asset_version_t& version)
        {
            constexpr std::size_t kPrefix = sizeof(uint32_t) + sizeof(asset_version_t);
            if (data.size() < kPrefix)
                return EAssetError::ABNORMAL_FILE_SIZE;
            std::memcpy(&magic,   data.data(),                  sizeof(magic));
            std::memcpy(&version, data.data() + sizeof(magic),  sizeof(version));
            return EAssetError::SUCCESS;
        }

        /// Copy a migrated v1 header into the current AssetFileHeader. The v1
        /// section offsets/sizes are correct for the v1 file and kept verbatim;
        /// only AssetInfo is upgraded to the current layout.
        static void migrateV1HeaderInto(const compat::AssetFileHeaderV1& v1,
                                        AssetFileHeader& header) noexcept
        {
            header.magic_number = v1.magic_number;
            header.version      = v1.version;
            header.info_offset  = v1.info_offset;
            header.info_size    = v1.info_size;
            header.data_offset  = v1.data_offset;
            header.data_size    = v1.data_size;
            header.info         = compat::upgradeAssetInfo(v1.info);
        }

        /// Backward-compat decoder for asset_version_v1 (typed-info path). Parses
        /// the frozen v1 layout, then exposes it through the current header so
        /// all downstream code stays version-agnostic. Add a sibling decoder +
        /// loader `case` to support a future retired version.
        template<typename InfoType, EAssetType T> requires std::is_standard_layout_v<InfoType>
        static EAssetError loadHeaderV1(std::span<const std::byte> data, AssetFileHeader& header, InfoType& info)
        {
            if (data.size() < sizeof(compat::AssetFileHeaderV1))
                return EAssetError::ABNORMAL_FILE_SIZE;
            compat::AssetFileHeaderV1 v1{};
            std::memcpy(&v1, data.data(), sizeof(v1));
            // Per-type InfoType did NOT change between v1 and v2, so its size /
            // placement checks still apply — only AssetInfo grew.
            if (v1.info_size != sizeof(InfoType))
                return EAssetError::WRONG_FILE_HEADER;
            if (v1.info_offset != sizeof(compat::AssetFileHeaderV1))
                return EAssetError::WRONG_FILE_HEADER;
            if (v1.data_offset != v1.info_offset + v1.info_size)
                return EAssetError::WRONG_FILE_HEADER;
            if (data.size() < v1.info_offset + v1.info_size)
                return EAssetError::ABNORMAL_FILE_SIZE;
            if (data.size() < v1.data_offset + v1.data_size)
                return EAssetError::ABNORMAL_FILE_SIZE;
            std::memcpy(&info, data.data() + v1.info_offset, sizeof(InfoType));
            migrateV1HeaderInto(v1, header);
            return EAssetError::SUCCESS;
        }

    public:

    protected:
        /**
         * @brief Loads an asset from an external file stream.
         *
         * Default implementation returns an unsupported error.
         *
         * @param external_path Input file stream for the external file.
         * @return On success, an expected unique pointer to a LuxAsset;
         *         on failure, an EAssetError.
         */
        [[nodiscard]] virtual lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
        fromFileStream(std::ifstream& ifs)
        {
            return lux::cxx::unexpected<EAssetError>(EAssetError::UNSUPPORTED);
        }

        /**
         * @brief Loads an asset from raw memory data.
         *
         * Default implementation returns an unsupported error.
         *
         * @param data Pointer to the memory data.
         * @param len Length of the memory data in bytes.
         * @return On success, an expected unique pointer to a LuxAsset;
         *         on failure, an EAssetError.
         */
        [[nodiscard]] virtual lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
        fromMemory(const void* data, size_t len)
        {
            return lux::cxx::unexpected<EAssetError>(EAssetError::UNSUPPORTED);
        }

        /**
         * @brief Loads an asset from a luxasset formatted stream.
         *
         * Takes a generic std::istream (not ifstream) so the same per-type
         * reader serves loose files AND in-memory images (pak entries) —
         * see fromLuxAssetMemory(). Implementations may only use seekg/
         * tellg/read. Default implementation returns an unsupported error.
         *
         * @param ifs Input stream positioned at the start of the luxasset image.
         * @return On success, an expected unique pointer to a LuxAsset;
         *         on failure, an EAssetError.
         */
        [[nodiscard]] virtual lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
        fromLuxAssetStream(std::istream& ifs)
        {
            return lux::cxx::unexpected<EAssetError>(EAssetError::UNSUPPORTED);
        }

        /**
         * @brief Writes a LuxAsset to an output file stream in luxasset format.
         *
         * This pure virtual function must be implemented by derived classes.
         *
         * @param asset The LuxAsset to be serialized.
         * @param path Output file stream where the luxasset will be written.
         * @return EAssetError indicating whether the write operation was successful.
         */
        virtual EAssetError
        exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& ofs) = 0;

        /**
         * @brief Checks if a directory exists or creates it if it does not.
         *
         * This utility function ensures that the specified directory exists.
         *
         * @param path Filesystem path to the directory.
         * @return true if the directory already exists or was successfully created, false otherwise.
         */
        static bool existsOrMkdir(const std::filesystem::path& path);

        /**
         * @brief Serializes asset information into a byte vector.
         *
         * @param asset Reference to the AssetInfo structure to serialize.
         * @return A vector of bytes containing the serialized asset information.
         */
        static std::vector<std::byte> serializeAssetInfo(const AssetInfo &asset);

        /**
         * @brief Deserializes asset information from a byte vector.
         *
         * @param data Vector of bytes containing the serialized asset information.
         * @param asset Reference to an AssetInfo structure to populate.
         * @return true if the asset information was successfully deserialized, false otherwise.
         */
        static bool deserializeAssetInfo(const std::vector<std::byte>& data, AssetInfo& asset);

        /**
         * @brief Retrieves the shared pointer to the AssetManager.
         *
         * @return Shared pointer to the AssetManager.
         */
        [[nodiscard]] std::shared_ptr<AssetManager> managerPtr() const { return manager_; }

        /**
         * @brief Submits a LuxAsset for management.
         *
         * @param asset Unique pointer to the LuxAsset to be submitted.
         * @return true if the asset was successfully submitted, false otherwise.
         */
        [[nodiscard]] bool submitAsset(std::unique_ptr<LuxAsset>);

    private:
        std::shared_ptr<AssetManager> manager_; ///< Shared pointer to the AssetManager instance.
    };

    /// RTTI-free factory trampoline stored by AssetCodecCatalog.
    using AssetSerDeserFactoryFn = std::unique_ptr<AssetSerDeser> (*)(
        EAssetType,
        std::shared_ptr<AssetManager>
    );

    // ─── Lazy loading: pure-data decode + shell injection ──────────────────
    //
    // Lazy loading (see .internal/async-asset-redesign-2026-06-17.md §1.5):
    // startup only creates the info-only shell; the pure data is decoded on a
    // background thread on first use, then injected into the shell at a sync
    // point.

    /// Type-erased "inject already-decoded pure data into an (already
    /// registered) shell" step. Produced by decodeAssetData on a background
    /// thread — it captures the decoded data and knows which concrete asset
    /// type's setData to call — and invoked on the shell at a main-thread sync
    /// point to complete the injection. move-only (captures a unique_ptr to
    /// the data).
    template<typename Config>
	class TAssetSerDeser : public AssetSerDeser
    {
    public:
        using AssetSerDeser::AssetSerDeser;

        Config& config() { return config_;}
        const Config& config() const { return config_; };

    private:
        Config config_;
    };
}
