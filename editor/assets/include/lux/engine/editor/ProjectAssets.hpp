#pragma once

#include <lux/engine/core/execution/TaskInfo.hpp>
#include <lux/engine/editor/AssetImporter.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lux::editor
{
    class EditorContext;

    struct ProjectAsset final
    {
        asset::AssetId id;
        asset::AssetTypeId type;
        std::string name;
        std::filesystem::path source;
        std::string importer;
        std::uint32_t importer_version{};
        std::vector<std::byte> configuration;
        std::uint64_t revision{};
        std::string package; // Immutable location, shared by this import's typed products.
        std::vector<AssetProduct> products;
    };

    // Frame-borrowed drag value; the receiver verifies type and revision before loading.
    inline constexpr std::string_view kAssetPayload = "lux.editor.asset";

    struct AssetReference final
    {
        std::array<std::uint8_t, 16> id{};
        asset::AssetTypeId type;
        std::uint64_t revision{};
    };

    // Editor authoring catalog. Owns import records and VFS mounts, never GPU handles or a worker pool.
    // Accepted tasks own their inputs; Main completion resolves the existing ObjectId before adoption.
    class ProjectAssets final : public object::LuxObject
    {
    public:
        ProjectAssets(EditorContext&, std::vector<AssetImportDeclaration>) noexcept;
        ~ProjectAssets() noexcept;

        [[nodiscard]] AssetImportResult<execution::TaskId> open() noexcept;
        [[nodiscard]] AssetImportResult<execution::TaskId> importAsset(
            std::string_view importer,
            std::uint32_t version,
            std::filesystem::path source,
            std::vector<std::byte> configuration
        ) noexcept;
        [[nodiscard]] std::span<const AssetImportDeclaration> importers() const noexcept;
        [[nodiscard]] const ProjectAsset* find(asset::AssetId) const noexcept;
        [[nodiscard]] AssetImportResult<execution::TaskId> reimport(asset::AssetId) noexcept;
        [[nodiscard]] bool ready() const noexcept;
        [[nodiscard]] bool busy() const noexcept;
        [[nodiscard]] std::uint64_t revision() const noexcept;
        [[nodiscard]] std::span<const ProjectAsset> records() const noexcept;
        [[nodiscard]] const std::optional<AssetImportFailure>& failure() const noexcept;

        object::TSignal<> changed{*this};

    private:
        [[nodiscard]] AssetImportResult<execution::TaskId> import(ProjectAsset) noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
