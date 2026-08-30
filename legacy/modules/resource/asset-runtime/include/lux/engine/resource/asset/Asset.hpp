#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <functional>
#include <filesystem>
#include <type_traits>
#include <variant>
#include <vector>
#include <lux/engine/resource/asset/visibility.h>
#include <lux/engine/resource/identity/AssetId.hpp>

namespace lux::asset
{
    /// Type alias for asset version.
    using asset_version_t = uint32_t;
    /// Magic code identifying the MEANING of an auxiliary asset payload
    /// (FourCC/EightCC-like). Defined by the feature that owns the payload
    /// (e.g. a thumbnail tag lives in the thumbnail module, NOT here) so the
    /// asset module stays feature-agnostic.
    using payload_tag_t = std::uint64_t;

    /// One auxiliary payload: a feature-defined tag + its opaque bytes. Assets
    /// carry only a small handful (thumbnail, import settings, …), so they live
    /// in a flat vector — cheap to (de)serialize and fine to linear-scan.
    struct Payload
    {
        payload_tag_t          tag{};
        std::vector<std::byte> data;
    };

    //（EAssetDataState 状态机已删除:freeze/markUploaded 全仓零调用 —— GPU 上传
    //  缓存走「把字节拷进条目」,从不冻结资产;剩下的 NO_MEMORY/BUILDING 两态
    //  恒等于 `data_ != nullptr`。`hasData()` 是唯一真相源。）

    /**
     * @brief Enumeration of asset error codes.
     *
     * These error codes describe possible failures during asset operations,
     * such as loading, saving, or file system related issues.
     */
    enum class EAssetError
    {
        SUCCESS,                       ///< Operation completed successfully.
        RELATED_ASSET_LOAD_ERROR,      ///< An error occurred while loading a related asset.
        FILE_NOT_EXIST,                ///< The specified file does not exist.
        FILE_TYPE_ERROR,               ///< The file type is incorrect or unsupported.
		FILE_OPEN_FAIL,                ///< Failed to open the file.
		WRITE_FILE_FAIL,               ///< Failed to write to the file.
		READ_FILE_FAIL,				   ///< Failed to read from the file.
        OUT_OF_MEMORY,				   ///< The system ran out of memory.
        TARGET_NOT_EXIST,              ///< The target path does not exist.
        TARGET_IS_NOT_DIRECTORY,       ///< The target path is not a directory.
        TARGET_IS_NOT_FILE,            ///< The target path is not a file.
        RELEASED,                      ///< The asset has been released.
        UNSUPPORTED,                   ///< The operation is unsupported.
        ABNORMAL_FILE_SIZE,            ///< The file size is abnormal.
        WRONG_FILE_HEADER,             ///< The file header is invalid.
        ASSET_NO_DATA,                 ///< The asset contains no data.
        ASSET_ALREADY_EXIST,           ///< The asset already exists.
        ASSET_NOT_EXIST,               ///< The asset does not exist.
        ASSET_NO_INFO,                 ///< No information is available for the asset.
		ASSET_DESERIALIZE_FAIL,        ///< Failed to deserialize the asset.
		UNSUPPORTED_VERSION,           ///< The asset version is unsupported.
        UNKNOWN_FILESYSTEM_ERROR,      ///< An unknown file system error occurred.
        UNKNOWN_ERROR                  ///< An unspecified error occurred.
    };

    /**
     * @brief Enumeration of asset types.
     *
     * These asset types specify the kind of resource represented by an asset.
     */
    enum class EAssetType : std::uint32_t
    {
        TEXTURE = 0u,          ///< Texture asset.
        MODEL = 1u,            ///< Model asset.
        SHADER = 2u,           ///< Shader asset.
        MESH = 3u,             ///< Mesh asset.
        FONT = 4u,             ///< Font asset.
        SOUND = 5u,            ///< Sound asset.
        SCRIPT = 6u,           ///< Compiled script asset (shared library + manifest).
        SKELETON = 7u,         ///< Skeletal animation skeleton (bone hierarchy + bind pose).
        ANIMATION_CLIP = 8u,   ///< Keyframe animation clip targeting a Skeleton.
        MATERIAL = 9u,         ///< Node-graph material (authoring graph + baked SPIR-V + params).
        MATERIAL_INSTANCE = 10u, ///< Parent MATERIAL plus parameter/texture overrides.
        TEXTURE_ATLAS = 11u,   ///< Named UV sub-rects and pivots over one TEXTURE.
        FLIPBOOK_CLIP = 12u,   ///< Frame animation over a TEXTURE_ATLAS.
        FLOW_GRAPH = 13u,      ///< FlowForge authoring graph.
        UNKNOWN = 14u          ///< Unknown or unsupported Resource asset type.
    };

    /**
     * @brief Structure containing metadata for an asset.
     *
     * This structure holds important information about an asset,
     * including its version, unique identifier, type, and timestamp.
     *
     * @note This structure is marked for static reflection using the LUX_REFL macro.
     */
    struct AssetInfo
    {
        asset_id_t      id;      ///< Unique identifier of the asset.
        EAssetType      type;    ///< Type of the asset.
        uint64_t        date;    ///< Creation timestamp (ns since epoch).

        // ---- Added in asset format v2 (provenance + display) ----
        // Fixed-size, trivially-copyable storage so AssetInfo stays embeddable
        // by value inside the memcpy'd AssetFileHeader. Zero-initialized when
        // unset; treat as NUL-terminated C strings.
        char            display_name[64]{};   ///< Human-readable name; "" if unset.
        char            source_path[256]{};   ///< Originating source file path; "" if unset.
        uint64_t        source_mtime{};       ///< Source file mtime (raw file-clock ticks); 0 if unset.
    };
    static_assert(std::is_trivially_copyable_v<AssetInfo>,
        "AssetInfo is memcpy'd into AssetFileHeader; keep it trivially copyable "
        "(fixed-size fields only, no std::string).");
    static_assert(std::is_standard_layout_v<AssetInfo>,
        "AssetInfo is serialized by raw byte copy; keep it standard-layout.");

    /**
     * @brief Base class representing a generic asset.
     *
     * The LuxAsset class encapsulates common functionalities for all assets,
     * including management of asset metadata and providing an interface to access raw asset data.
     */
	class LuxAsset
    {
    public:
        /**
         * @brief Constructs a LuxAsset with the given asset information.
         *
         * @param info Unique pointer to an AssetInfo structure containing the asset metadata.
         */
        explicit LuxAsset(std::unique_ptr<AssetInfo> info)
            : info_(std::move(info)) {
        }

        /**
         * @brief Virtual destructor.
         */
        virtual ~LuxAsset() = default;

        // Delete copy constructor and copy assignment operator.
        LuxAsset(const LuxAsset& other) = delete;
        LuxAsset& operator=(const LuxAsset& other) = delete;

        /**
         * @brief Move constructor.
         *
         * Transfers ownership of the asset metadata from another LuxAsset instance.
         *
         * @param other The LuxAsset object to move from.
         */
        LuxAsset(LuxAsset&& other) noexcept {
            info_ = std::move(other.info_);
            payloads_ = std::move(other.payloads_);
        }

        /**
         * @brief Move assignment operator.
         *
         * Transfers ownership of the asset metadata from another LuxAsset instance.
         *
         * @param other The LuxAsset object to move from.
         * @return Reference to this LuxAsset.
         */
        LuxAsset& operator=(LuxAsset&& other) noexcept {
            info_ = std::move(other.info_);
            payloads_ = std::move(other.payloads_);
            return *this;
        }

        /**
         * @brief Retrieves the asset metadata.
         *
         * @return Pointer to the AssetInfo structure.
         */
        [[nodiscard]] const AssetInfo* info() const
        {
            return info_.get();
        }

        /**
         * @brief Mutable access to the asset metadata.
         *
         * Intended for the import pipeline to stamp provenance fields
         * (display_name / source_path / source_mtime) after creation and before
         * persisting. The id/type/date are set at construction and must not be
         * changed afterwards (the id is the manager's map key).
         *
         * @return Pointer to the AssetInfo, or nullptr if none.
         */
        [[nodiscard]] AssetInfo* mutableInfo()
        {
            return info_.get();
        }

        // ---- Auxiliary payloads (extensible tagged binary slots) ----------
        // Beyond the primary `data`, an asset may carry any number of optional
        // binary payloads, each keyed by a feature-defined payload_tag_t (a
        // magic code). They are persisted after the data section in the
        // .luxasset (see AssetSerDeser) and are OPAQUE to the asset module:
        // features (thumbnails, import settings, LODs, …) define their own tag
        // + byte format and own the meaning. New features add a new tag with
        // zero asset-format changes.
        [[nodiscard]] bool hasPayload(payload_tag_t tag) const noexcept
        {
            for (const auto& p : payloads_)
                if (p.tag == tag) return true;
            return false;
        }

        /// Bytes for @p tag, or nullptr if the asset has no such payload.
        [[nodiscard]] const std::vector<std::byte>* payload(payload_tag_t tag) const noexcept
        {
            for (const auto& p : payloads_)
                if (p.tag == tag) return &p.data;
            return nullptr;
        }

        /// Set (replace if the tag already exists, else append) a payload.
        void setPayload(payload_tag_t tag, std::vector<std::byte> bytes)
        {
            for (auto& p : payloads_)
            {
                if (p.tag == tag) { p.data = std::move(bytes); return; }
            }
            payloads_.push_back(Payload{ tag, std::move(bytes) });
        }

        void removePayload(payload_tag_t tag) noexcept
        {
            std::erase_if(payloads_, [tag](const Payload& p) { return p.tag == tag; });
        }

        /// All payloads, in insertion order (serialized verbatim).
        [[nodiscard]] const std::vector<Payload>& payloads() const noexcept { return payloads_; }

        /// Bulk-assign (used by the loader after reading the payload region).
        void setPayloads(std::vector<Payload> payloads) { payloads_ = std::move(payloads); }

        /**
         * @brief Retrieves the asset's unique identifier.
         *
         * @return Constant reference to the asset ID.
         */
        [[nodiscard]] const asset_id_t& id() const
        {
            return info_->id;
        }

        /**
         * @brief Retrieves the type of the asset.
         *
         * @return The asset type as defined by EAssetType.
         */
        [[nodiscard]] EAssetType type() const
        {
            return info_->type;
        }

        /**
         * @brief Safe downcast to a concrete asset type — no RTTI.
         *
         * Keys the stored EAssetType (see type()) against the target's
         * `asset_type` constant (every concrete asset declares one). A base or
         * self type (LuxAsset) is always a valid upcast. Returns
         * nullptr on a type mismatch — exactly the contract of the dynamic_cast
         * it replaces, but without depending on RTTI.
         *
         * @tparam T The target type to cast to.
         * @return Pointer to the casted type, or nullptr if the type mismatches.
         */
        template<typename T>
        T* as() noexcept
        {
            if constexpr (std::is_base_of_v<T, LuxAsset>)
                return static_cast<T*>(this);
            else
                return type() == T::asset_type ? static_cast<T*>(this) : nullptr;
        }

        /**
         * @brief Const overload of as<T>() — see the mutable version.
         *
         * @tparam T The target type to cast to.
         * @return Const pointer to the casted type, or nullptr if it mismatches.
         */
        template<typename T>
        const T* as() const noexcept
        {
            if constexpr (std::is_base_of_v<T, LuxAsset>)
                return static_cast<const T*>(this);
            else
                return type() == T::asset_type ? static_cast<const T*>(this) : nullptr;
        }

        /**
         * @brief Checks whether the asset has associated data.
         *
         * This is a pure virtual function that must be implemented by derived classes.
         *
         * @return true if the asset has data; false otherwise.
         */
        [[nodiscard]] virtual bool hasData() const = 0;

        /// Open-world (W2c): drop the CPU-side data back to an empty shell to
        /// free CPU memory. Returns whether data was actually released. VRAM is
        /// already freed by the bridge's reap when refcount reaches 0; this
        /// step only reclaims CPU memory — on the next wake-up, requestLoad
        /// simply re-decodes it. The default (data-less asset types) has
        /// nothing to release, so returns false.
        [[nodiscard]] virtual bool unloadData() { return false; }

    protected:
        std::unique_ptr<AssetInfo> info_; ///< Unique pointer to the asset's metadata.
        /// Optional auxiliary payloads (tagged binary slots). Small N, flat
        /// vector. Opaque to the asset module.
        std::vector<Payload> payloads_;
    };

    /**
     * @brief Template class for assets with a specific data type.
     *
     * TAsset is a templated asset class that inherits from LuxAsset and encapsulates
     * an asset's data of a specified type. It provides methods for setting, assigning,
     * and accessing the asset data.
     *
     * @tparam DataType The type of the asset's data.
     */
    template<typename DataType>
    class TAsset : public LuxAsset
    {
    public:
        /// Alias for the asset data type.
        using asset_data_t = DataType;

        /**
         * @brief Constructs a TAsset with the provided asset metadata.
         *
         * @param info Unique pointer to an AssetInfo structure containing asset metadata.
         */
        explicit TAsset(std::unique_ptr<AssetInfo> info, std::unique_ptr<DataType> data = nullptr)
            : LuxAsset(std::move(info)), data_(std::move(data)) {
        }

        // Delete copy constructor and copy assignment operator.
        TAsset(const TAsset&) = delete;
        TAsset& operator=(const TAsset&) = delete;

        /**
         * @brief Default move constructor.
         *
         * Transfers ownership of asset data and metadata from another TAsset instance.
         */
        TAsset(TAsset&&) noexcept = default;

        /**
         * @brief Default move assignment operator.
         *
         * Transfers ownership of asset data and metadata from another TAsset instance.
         *
         * @return Reference to this TAsset.
         */
        TAsset& operator=(TAsset&&) noexcept = default;

        /**
         * @brief Sets the asset data.
         *
         * @param data Unique pointer to the asset data.
         */
        void setData(std::unique_ptr<DataType> data) {
            data_ = std::move(data);
        }

        /**
         * @brief Assigns new asset data by constructing a DataType object.
         *
         * Forwards the provided arguments to the constructor of DataType.
         *
         * @tparam Args Variadic template parameters.
         * @param args Arguments forwarded to the DataType constructor.
         */
        template<typename... Args>
        void assignData(Args&&... args)
        {
            data_ = std::make_unique<DataType>(
                std::forward<Args>(args)...
            );
        }

        /**
         * @brief Clears the asset data by resetting the unique pointer.
		 */
        void clearData() {
            data_.reset();
        }

        /**
         * @return true if the asset data pointer is not null; false otherwise.
         */
        [[nodiscard]] bool hasData() const override
        {
            return data_ != nullptr;
        }

        /**
         * @brief Retrieves the asset data cast to the specific DataType.
         *
         * @return Pointer to the asset data of type DataType.
         */
        DataType* data() {
            return data_.get();
        }

        /**
         * @brief Retrieves the asset data cast to the specific DataType (const version).
         *
         * @return Constant pointer to the asset data of type DataType.
         */
        const DataType* data() const {
            return data_.get();
        }

        //（freezeForUpload / markUploaded / dataState 已删:全仓零调用。上传路径
        //  从不借用资产字节 —— GPU 缓存把 SPIR-V/数据拷进自己的条目。）

        /// See LuxAsset::unloadData. A no-op when already data-less.
        [[nodiscard]] bool unloadData() override
        {
            if (!data_) return false;
            data_.reset();
            return true;
        }

    protected:
        std::unique_ptr<DataType> data_{ nullptr }; ///< Unique pointer to the asset's data.
    };
}
