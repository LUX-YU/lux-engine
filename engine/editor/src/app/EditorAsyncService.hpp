#pragma once
/**
 * @file EditorAsyncService.hpp
 * @brief Typed editor operations hosted by the process AsyncRuntime.
 *
 * This is composition-root infrastructure, not an event bus. Requesters call
 * a single strong method; results return to the main safe point exactly once.
 * DomainEvents is reserved for facts committed after these results are
 * adopted.
 */

#include "app/AssetFileWatcher.hpp"
#include "app/EditorAsyncTypes.hpp"
#include "app/ImportController.hpp"
#include "panels/MaterialGraphPanel.hpp"
#include "script/FlowForgeCompilerService.hpp"

#include <lux/engine/authoring/world/WorldSource.hpp>
#include <lux/engine/authoring/world/WorldDescriptorIndex.hpp>
#include <lux/engine/authoring/world/WorldTerrainAuthoring.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/resource/entity_scene/EntitySceneIdentifiers.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/runtime/execution/AsyncOperation.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace lux::asset { class AssetVfs; class LuxAsset; }
namespace lux::exec {
    class AsyncRuntime;
    class AsyncScope;
    class AsyncScopeCloseSender;
}

namespace lux::editor
{
    struct ImportAssetValue final
    {
        std::shared_ptr<ImportReport> report;
        std::filesystem::path source;
    };

    struct ImportAssetOperation final
    {
        using Value = ImportAssetValue;
        using Error = EEditorAsyncError;

        std::shared_ptr<const ImportJob> job;
    };

    struct CookContentValue final
    {
        bool ok{false};
        std::size_t asset_count{0};
        std::string message;
        std::filesystem::path out_pak;
    };

    struct CookContentOperation final
    {
        using Value = CookContentValue;
        using Error = EEditorAsyncError;

        std::filesystem::path content_dir;
        std::filesystem::path out_pak;
    };

    struct EntitySceneCookDocument final
    {
        std::string relative_path;
        std::vector<std::byte> bytes;
    };

    /// Main-thread-authored immutable input. ECS access ends before this value
    /// crosses into AsyncRuntime. Component descriptors are an owning frozen
    /// snapshot: their module leases pin every reflected operation used by the
    /// worker-local catalog for the duration of the cook.
    struct EntityScenePlayCookJob final
    {
        /// Temporary Toolchain-only LXWA root. SceneRuntime never receives
        /// this path or an Authoring/legacy World image.
        std::filesystem::path root_document;
        std::filesystem::path source_root_document;
        lux::authoring::WorldSourceDocument source;
        std::vector<lux::authoring::WorldDescriptorPageDocument>
            descriptor_pages;
        std::vector<EntitySceneCookDocument> documents;
        std::vector<lux::ecs::ComponentSchemaDescriptor> component_schemas;
        /// Immutable project asset view retained across the worker cook. Only
        /// Mesh images referenced by the loaded Spatial3D source are copied
        /// into the Toolchain input closure.
        std::shared_ptr<const lux::asset::AssetVfs> asset_vfs;
    };

    struct CookEntitySceneValue final
    {
        std::filesystem::path pak_file;
        lux::entity_scene::EntitySceneId scene_id;
        std::string scene_origin;
        std::string message;
    };

    struct CookEntitySceneOperation final
    {
        using Value = CookEntitySceneValue;
        using Error = EEditorAsyncError;

        std::shared_ptr<const EntityScenePlayCookJob> job;
    };

    struct CollectWorldSourceGarbageValue final
    {
        std::uint64_t live_documents{0u};
        std::uint64_t scanned_documents{0u};
        std::uint64_t removed_documents{0u};
        std::uint64_t deferred_documents{0u};
        bool removal_budget_exhausted{false};
        std::string error;
    };

    struct CollectWorldSourceGarbageOperation final
    {
        using Value = CollectWorldSourceGarbageValue;
        using Error = EEditorAsyncError;

        std::filesystem::path world_file;
        std::uint64_t grace_period_seconds{24u * 60u * 60u};
        std::uint32_t maximum_removals_per_pass{4096u};
    };

    struct RebuildWorldDescriptorIndexValue final
    {
        std::shared_ptr<const lux::authoring::WorldDescriptorIndex> index;
        std::string error;
    };

    struct RebuildWorldDescriptorIndexOperation final
    {
        using Value = RebuildWorldDescriptorIndexValue;
        using Error = EEditorAsyncError;

        std::filesystem::path world_file;
        std::filesystem::path cache_file;
        std::shared_ptr<const lux::authoring::WorldSourceDocument> source;
    };

    struct LoadWorldActorProxyValue final
    {
        lux::authoring::WorldActorSourceDescriptor descriptor;
        std::optional<lux::authoring::WorldDescriptorPageDocument> page;
        std::optional<lux::authoring::WorldActorDocument> document;
        std::string error;
    };

    struct LoadWorldActorProxyOperation final
    {
        using Value = LoadWorldActorProxyValue;
        using Error = EEditorAsyncError;

        std::filesystem::path world_file;
        std::shared_ptr<const lux::authoring::WorldSourceDocument> source;
        lux::authoring::WorldDescriptorPageReference page;
        lux::entity_scene::PersistentEntityId actor;
        std::optional<lux::authoring::WorldActorSourceDescriptor> descriptor;
    };

    struct LoadWorldDescriptorPageValue final
    {
        std::optional<lux::authoring::WorldDescriptorPageDocument> page;
        std::string error;
    };

    struct LoadWorldDescriptorPageOperation final
    {
        using Value = LoadWorldDescriptorPageValue;
        using Error = EEditorAsyncError;

        std::filesystem::path world_file;
        std::shared_ptr<const lux::authoring::WorldSourceDocument> source;
        lux::authoring::WorldDescriptorPageReference page;
    };

    struct LoadWorldInstancePageValue final
    {
        lux::authoring::WorldPageSourceDescriptor descriptor;
        std::optional<lux::authoring::WorldInstancePageDocument> page;
        std::string error;
    };

    struct LoadWorldInstancePageOperation final
    {
        using Value = LoadWorldInstancePageValue;
        using Error = EEditorAsyncError;

        std::filesystem::path world_file;
        std::shared_ptr<const lux::authoring::WorldSourceDocument> source;
        lux::authoring::WorldPageSourceDescriptor page;
    };

    struct LoadWorldTerrainRegionValue final
    {
        std::vector<lux::authoring::WorldDescriptorPageDocument>
            descriptor_pages;
        std::vector<lux::authoring::WorldTerrainPageDocument> pages;
        std::string error;
    };

    struct LoadWorldTerrainRegionOperation final
    {
        using Value = LoadWorldTerrainRegionValue;
        using Error = EEditorAsyncError;

        std::filesystem::path world_file;
        std::shared_ptr<const lux::authoring::WorldSourceDocument> source;
        lux::authoring::TerrainSetId terrain;
        lux::authoring::PartitionSpaceId space;
        std::vector<lux::authoring::WorldCellKey> cells;
    };

    enum class EWorldTerrainHeightmapFileMode : std::uint8_t
    {
        READ,
        WRITE
    };

    struct WorldTerrainHeightmapFileValue final
    {
        lux::authoring::WorldTerrainHeightmap16 image;
        std::filesystem::path path;
        std::string error;
    };

    struct WorldTerrainHeightmapFileOperation final
    {
        using Value = WorldTerrainHeightmapFileValue;
        using Error = EEditorAsyncError;

        EWorldTerrainHeightmapFileMode mode{
            EWorldTerrainHeightmapFileMode::READ};
        std::filesystem::path path;
        lux::authoring::WorldTerrainHeightmap16 image;
    };

    struct ReloadAssetValue final
    {
        lux::asset::asset_id_t id{};
        std::uint64_t generation{0u};
        std::unique_ptr<lux::asset::LuxAsset> asset;
        std::string error;
    };

    struct ReloadAssetOperation final
    {
        using Value = ReloadAssetValue;
        using Error = EEditorAsyncError;

        lux::asset::asset_id_t id{};
        std::uint64_t generation{0u};
        std::filesystem::path abs_path;
    };

    struct CompileMaterialOperation final
    {
        using Value = std::shared_ptr<MaterialCompileOutcome>;
        using Error = EEditorAsyncError;

        std::shared_ptr<const MaterialCompileJob> job;
    };

    class EditorAsyncService final
    {
    public:
        template <class Operation>
        using Completion = lux::cxx::move_only_function<void(
            lux::exec::AsyncOutcome<Operation>)>;

        [[nodiscard]] static lux::cxx::expected<
            EditorAsyncService,
            lux::exec::AsyncAssemblyFailure>
        addTo(lux::exec::AsyncRuntimeBuilder& builder);

        EditorAsyncService() noexcept = default;
        ~EditorAsyncService();
        EditorAsyncService(const EditorAsyncService&) = delete;
        EditorAsyncService& operator=(const EditorAsyncService&) = delete;
        EditorAsyncService(EditorAsyncService&&) noexcept;
        EditorAsyncService& operator=(EditorAsyncService&&) noexcept;

        void bind(lux::exec::AsyncRuntime& runtime);
        [[nodiscard]] lux::exec::AsyncScopeCloseSender closeAsync() noexcept;

        [[nodiscard]] bool importAsset(
            ImportAssetOperation operation,
            Completion<ImportAssetOperation> completion);
        [[nodiscard]] bool cookContent(
            CookContentOperation operation,
            Completion<CookContentOperation> completion);
        [[nodiscard]] bool cookEntityScene(
            CookEntitySceneOperation operation,
            Completion<CookEntitySceneOperation> completion);
        [[nodiscard]] bool collectWorldSourceGarbage(
            CollectWorldSourceGarbageOperation operation,
            Completion<CollectWorldSourceGarbageOperation> completion);
        [[nodiscard]] bool rebuildWorldDescriptorIndex(
            RebuildWorldDescriptorIndexOperation operation,
            Completion<RebuildWorldDescriptorIndexOperation> completion);
        [[nodiscard]] bool loadWorldActorProxy(
            LoadWorldActorProxyOperation operation,
            Completion<LoadWorldActorProxyOperation> completion);
        [[nodiscard]] bool loadWorldDescriptorPage(
            LoadWorldDescriptorPageOperation operation,
            Completion<LoadWorldDescriptorPageOperation> completion);
        [[nodiscard]] bool loadWorldInstancePage(
            LoadWorldInstancePageOperation operation,
            Completion<LoadWorldInstancePageOperation> completion);
        [[nodiscard]] bool loadWorldTerrainRegion(
            LoadWorldTerrainRegionOperation operation,
            Completion<LoadWorldTerrainRegionOperation> completion);
        [[nodiscard]] bool worldTerrainHeightmapFile(
            WorldTerrainHeightmapFileOperation operation,
            Completion<WorldTerrainHeightmapFileOperation> completion);
        [[nodiscard]] bool reloadAsset(
            ReloadAssetOperation operation,
            Completion<ReloadAssetOperation> completion);
        [[nodiscard]] bool compileMaterial(
            CompileMaterialOperation operation,
            Completion<CompileMaterialOperation> completion);
        [[nodiscard]] bool compileFlowForge(
            CompileFlowForgeOperation operation,
            Completion<CompileFlowForgeOperation> completion);

        [[nodiscard]] FlowForgeCompileClient flowForgeCompileClient() const noexcept;
    private:
        struct State;

        explicit EditorAsyncService(std::shared_ptr<State> state) noexcept
            : state_(std::move(state))
        {}

        std::shared_ptr<State> state_;
    };
}
