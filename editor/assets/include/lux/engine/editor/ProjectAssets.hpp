#pragma once

#include <lux/engine/core/execution/TaskInfo.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/engine/toolchain/asset/model/ModelCookConfiguration.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lux::asset
{
    class ModelAsset;
}

namespace lux::scene
{
    struct MeshQueryGeometry;
}

namespace lux::editor
{
    class EditorContext;

    struct ProjectAsset final
    {
        asset::AssetId id;
        std::string name;
        std::filesystem::path source;
        toolchain::ModelCookConfiguration configuration;
        std::uint64_t revision{};
        std::string package; // Immutable product path relative to the project root.
        std::vector<asset::AssetId> products;
    };

    // Small, copied UI intent. Receiving tools recheck this revision; payload memory is frame-borrowed.
    inline constexpr std::string_view kModelAssetPayload = "lux.editor.model_asset";

    struct ModelAssetReference final
    {
        std::array<std::uint8_t, 16> id{};
        std::uint64_t revision{};
    };

    enum class EAssetImportError : std::uint8_t
    {
        NOT_READY,
        BUSY,
        INVALID_INPUT,
        CANCELLED,
        READ,
        COOK,
        ENCODE,
        PUBLICATION,
        STALE_CATALOG,
        INVALID_CATALOG,
        EXECUTION
    };

    struct AssetImportFailure final
    {
        EAssetImportError code{};
        std::string detail;
        // True also for uncertain root durability; never claim that cancellation removed published data.
        bool catalog_published{};
    };
    template <class T> using AssetImportResult = cxx::expected<T, AssetImportFailure>;

    // Editor authoring catalog. Owns import records and VFS mounts, never GPU handles or a worker pool.
    // Accepted tasks own their inputs; Main completion resolves the existing ObjectId before adoption.
    class ProjectAssets final : public object::LuxObject
    {
    public:
        explicit ProjectAssets(EditorContext&) noexcept;
        ~ProjectAssets() noexcept;

        [[nodiscard]] AssetImportResult<execution::TaskId> open() noexcept;
        [[nodiscard]] AssetImportResult<execution::TaskId> importModel(
            std::filesystem::path source,
            toolchain::ModelCookConfiguration configuration = {}
        ) noexcept;
        [[nodiscard]] AssetImportResult<execution::TaskId> reimport(asset::AssetId) noexcept;
        [[nodiscard]] bool ready() const noexcept;
        [[nodiscard]] bool busy() const noexcept;
        [[nodiscard]] std::uint64_t revision() const noexcept;
        [[nodiscard]] std::span<const ProjectAsset> records() const noexcept;
        [[nodiscard]] const asset::ModelAsset* model(asset::AssetId) const noexcept;
        // Lazy CPU geometry projection used by spatial queries; all read/decode/BVH work stays off Main.
        [[nodiscard]] AssetImportResult<void> prepareMeshGeometry(asset::AssetId) noexcept;
        [[nodiscard]] std::shared_ptr<const scene::MeshQueryGeometry> meshGeometry(asset::AssetId) const noexcept;
        [[nodiscard]] const std::optional<AssetImportFailure>& failure() const noexcept;

        object::TSignal<> changed{*this};

    private:
        [[nodiscard]] AssetImportResult<execution::TaskId> import(ProjectAsset) noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
