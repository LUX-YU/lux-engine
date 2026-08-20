#include "app/EditorAsyncService.hpp"

#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/engine/resource/asset/AssetHeaderProbe.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/AssetVfs.hpp>
#include <lux/engine/authoring/assets/FlowGraphSerDeser.hpp>
#include <lux/engine/authoring/world/WorldSourceCodec.hpp>
#include <lux/engine/toolchain/asset/cook/PakCook.hpp>
#include <lux/engine/toolchain/spatial3d_scene/Spatial3DEntitySceneAdapter.hpp>
#include <lux/engine/editor/import/AssetImporter.hpp>
#include <lux/engine/runtime/execution/AsyncFileService.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScope.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/cxx/core/Format.hpp>

#include <stdexec/execution.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lux::editor
{
    namespace ex = stdexec;

    namespace
    {
        [[nodiscard]] bool copyWorldSourceObject(
            const std::filesystem::path& source_root,
            const std::filesystem::path& destination_root,
            std::string_view relative_path)
        {
            auto source = lux::authoring::resolveWorldSourceDocument(
                source_root, relative_path);
            if (!source)
                return false;
            std::ifstream stream(
                *source, std::ios::binary | std::ios::ate);
            if (!stream)
                return false;
            const auto end = stream.tellg();
            if (end < 0)
                return false;
            std::vector<std::byte> bytes(static_cast<std::size_t>(end));
            stream.seekg(0);
            if (!bytes.empty() && !stream.read(
                    reinterpret_cast<char*>(bytes.data()), end))
            {
                return false;
            }
            return static_cast<bool>(
                lux::authoring::saveWorldSourceDocument(
                    destination_root, relative_path, bytes));
        }

        template <class Operation>
        class Terminal final
        {
        public:
            explicit Terminal(
                lux::exec::AsyncOperationCompletion<Operation> completion)
                : completion_(std::move(completion))
            {}

            void value(typename Operation::Value value) noexcept
            {
                if (!completion_)
                    return;
                auto completion = std::move(*completion_);
                completion_.reset();
                completion.complete(std::move(value));
            }

            void stopped() noexcept
            {
                if (!completion_)
                    return;
                auto completion = std::move(*completion_);
                completion_.reset();
                completion.failRuntime(lux::exec::EAsyncSubmitError::STOPPING);
            }

        private:
            std::optional<lux::exec::AsyncOperationCompletion<Operation>>
                completion_;
        };

        template <class Operation, class Sender>
        void launch(
            Sender sender,
            lux::exec::AsyncOperationContext& context,
            lux::exec::AsyncOperationCompletion<Operation> completion) noexcept
        {
            auto terminal = std::make_shared<Terminal<Operation>>(
                std::move(completion));
            auto pipeline = std::move(sender)
                | ex::then(
                      [terminal](typename Operation::Value value) noexcept
                      {
                          terminal->value(std::move(value));
                      })
                | ex::upon_stopped(
                      [terminal]() noexcept
                      {
                          terminal->stopped();
                      });
            if (!lux::exec::spawn(context.scope(), std::move(pipeline)))
                terminal->stopped();
        }

        struct ReloadBytes final
        {
            lux::asset::asset_id_t id{};
            std::uint64_t generation{0u};
            lux::exec::AsyncFileReadResult bytes;
        };

        struct PreparedEntitySceneCook final
        {
            std::shared_ptr<const EntityScenePlayCookJob> job;
            std::optional<lux::toolchain::Spatial3DAuthoringSource> source;
            lux::toolchain::Spatial3DMeshAssetCatalog mesh_assets;
            std::string error;
        };

        struct CookedEntitySceneWork final
        {
            std::filesystem::path root_document;
            std::optional<
                lux::toolchain::CookedSpatial3DEntitySceneBundle> bundle;
            std::string error;
        };

        [[nodiscard]] std::filesystem::path playEntityScenePakPath(
            const std::filesystem::path& root_document,
            const lux::toolchain::CookedSpatial3DEntitySceneBundle& bundle)
        {
            const auto content_digest = lux::cxx::algorithm::Sha256::hash(
                bundle.encoded_package);
            return root_document.parent_path() / "entity-scene-play-images" /
                (uuids::to_string(bundle.package.id.value()) + "-" +
                    lux::cxx::algorithm::toHex(content_digest) +
                    ".luxpak");
        }

        [[nodiscard]] lux::cxx::SharedBytes<> ownBytes(
            std::vector<std::byte> bytes)
        {
            if (bytes.empty())
                return {};
            auto storage = std::make_shared<std::vector<std::byte>>(
                std::move(bytes));
            auto owned = lux::cxx::SharedBytes<>::fromOwner(
                storage,
                std::span<const std::byte>{
                    storage->data(), storage->size()});
            return owned;
        }

        [[nodiscard]] lux::cxx::expected<
            lux::scene::ScenePackageId,
            std::string>
        publishPlayEntityScenePak(
            lux::toolchain::CookedSpatial3DEntitySceneBundle bundle,
            const std::filesystem::path& pak_file)
        {
            std::vector<lux::toolchain::PakCookMemoryEntry> entries;
            entries.reserve(
                1u + bundle.sections.size() +
                bundle.generated_meshes.size());
            const auto scene_id = bundle.package.id;
            entries.push_back({
                scene_id.value(),
                lux::asset::EAssetType::ENTITY_SCENE,
                "Scenes/Play",
                ownBytes(std::move(bundle.encoded_package))});
            for (auto& section : bundle.sections)
            {
                const auto key = uuids::to_string(section.record.id.value());
                const auto expected_source =
                    "/Game/EntitySections/" + key;
                const auto* stored = std::get_if<
                    lux::scene::StoredSectionSource>(
                        &section.record.source);
                if (stored == nullptr ||
                    stored->content_path != expected_source)
                {
                    return lux::cxx::unexpected(
                        "EntitySection '" + key + "' declares source '" +
                        (stored == nullptr
                            ? std::string{"<generated>"}
                            : stored->content_path) +
                        "'; expected absolute Pak path '" +
                        expected_source + "'");
                }
                entries.push_back({
                    section.record.id.value(),
                    lux::asset::EAssetType::ENTITY_SECTION,
                    "EntitySections/" + key,
                    ownBytes(std::move(section.encoded_image))});
            }
            for (auto& mesh : bundle.generated_meshes)
            {
                entries.push_back({
                    mesh.id,
                    lux::asset::EAssetType::MESH,
                    std::move(mesh.virtual_path),
                    ownBytes(std::move(mesh.encoded_image))});
            }
            if (std::ranges::any_of(
                    entries,
                    [](const auto& entry) { return entry.image.empty(); }))
            {
                return lux::cxx::unexpected(
                    std::string{"cannot own cooked Play EntityScene bytes"});
            }
            auto cooked = lux::toolchain::cookMemoryEntriesToPak(
                std::move(entries), pak_file, "/Game");
            if (!cooked)
                return lux::cxx::unexpected(std::move(cooked.error()));
            return scene_id;
        }

        ReloadAssetValue decodeReload(ReloadBytes input) noexcept
        {
            const auto codecs =
                lux::authoring::authoringAssetCodecCatalog();
            ReloadAssetValue value{
                .id = input.id,
                .generation = input.generation};
            if (!input.bytes)
            {
                value.error = lux::format(
                    "file read failed (error={}, system={})",
                    static_cast<int>(input.bytes.error().error),
                    input.bytes.error().system_error.value());
                return value;
            }
            if (input.bytes->size() < sizeof(std::uint32_t))
            {
                value.error = "file too small for a .luxasset header";
                return value;
            }

            auto storage = std::make_shared<lux::exec::AsyncFileBuffer>(
                std::move(*input.bytes));
            auto shared = lux::cxx::SharedBytes<>::fromOwner(
                storage,
                std::span<const std::byte>{
                    storage->data(), storage->size()});
            if (shared.empty())
            {
                value.error = "failed to retain file bytes";
                return value;
            }
            auto shell = lux::asset::makeShellFromMemory(
                *codecs,
                shared.data(), shared.size());
            if (!shell)
            {
                value.error = lux::format(
                    "shell decode failed (err={})",
                    static_cast<int>(shell.error()));
                return value;
            }
            if ((*shell)->id() != input.id)
            {
                value.error = "decoded bytes claim a different asset id";
                return value;
            }

            auto injector = codecs->decode(std::move(shared));
            if (!injector)
            {
                value.error = lux::format(
                    "decode failed (err={})",
                    static_cast<int>(injector.error()));
                return value;
            }
            (*injector)(**shell);
            value.asset = std::move(*shell);
            return value;
        }
    }

    struct EditorAsyncService::State final
    {
        lux::exec::AsyncRuntime* runtime{nullptr};
        std::unique_ptr<lux::exec::AsyncScope> scope;
        lux::exec::AsyncOperationClient<ImportAssetOperation> import;
        lux::exec::AsyncOperationClient<CookContentOperation> cook;
        lux::exec::AsyncOperationClient<CookEntitySceneOperation>
            entity_scene_cook;
        lux::exec::AsyncOperationClient<CollectWorldSourceGarbageOperation>
            world_source_gc;
        lux::exec::AsyncOperationClient<
            RebuildWorldDescriptorIndexOperation> descriptor_index;
        lux::exec::AsyncOperationClient<LoadWorldActorProxyOperation>
            actor_proxy;
        lux::exec::AsyncOperationClient<LoadWorldDescriptorPageOperation>
            descriptor_page;
        lux::exec::AsyncOperationClient<LoadWorldInstancePageOperation>
            instance_page;
        lux::exec::AsyncOperationClient<LoadWorldTerrainRegionOperation>
            terrain_region;
        lux::exec::AsyncOperationClient<WorldTerrainHeightmapFileOperation>
            terrain_heightmap_file;
        lux::exec::AsyncOperationClient<ReloadAssetOperation> reload;
        lux::exec::AsyncOperationClient<CompileMaterialOperation> material;
        lux::exec::AsyncOperationClient<CompileFlowGraph> flow_graph;
        bool closing{false};

        template <lux::exec::AsyncOperation Operation>
        [[nodiscard]] auto& client() noexcept
        {
            if constexpr (std::is_same_v<Operation, ImportAssetOperation>)
                return import;
            else if constexpr (std::is_same_v<Operation, CookContentOperation>)
                return cook;
            else if constexpr (std::is_same_v<
                                   Operation,
                                   CookEntitySceneOperation>)
                return entity_scene_cook;
            else if constexpr (std::is_same_v<
                                   Operation,
                                   CollectWorldSourceGarbageOperation>)
                return world_source_gc;
            else if constexpr (std::is_same_v<Operation, ReloadAssetOperation>)
                return reload;
            else if constexpr (std::is_same_v<
                                   Operation,
                                   RebuildWorldDescriptorIndexOperation>)
                return descriptor_index;
            else if constexpr (std::is_same_v<
                                   Operation,
                                   LoadWorldActorProxyOperation>)
                return actor_proxy;
            else if constexpr (std::is_same_v<
                                   Operation,
                                   LoadWorldDescriptorPageOperation>)
                return descriptor_page;
            else if constexpr (std::is_same_v<
                                   Operation,
                                   LoadWorldInstancePageOperation>)
                return instance_page;
            else if constexpr (std::is_same_v<
                                   Operation,
                                   LoadWorldTerrainRegionOperation>)
                return terrain_region;
            else if constexpr (std::is_same_v<
                                   Operation,
                                   WorldTerrainHeightmapFileOperation>)
                return terrain_heightmap_file;
            else if constexpr (std::is_same_v<Operation, CompileMaterialOperation>)
                return material;
            else
                return flow_graph;
        }

        template <lux::exec::AsyncOperation Operation>
        bool submit(
            Operation operation,
            EditorAsyncService::Completion<Operation> completion)
        {
            if (closing || runtime == nullptr || !scope)
                return false;
            lux::exec::AsyncSubmitOptions options{};
            if constexpr (std::is_same_v<
                              Operation,
                              WorldTerrainHeightmapFileOperation>)
            {
                options.accounted_bytes = operation.image.samples.size() *
                    sizeof(std::uint16_t);
            }
            else if constexpr (std::is_same_v<
                                   Operation,
                                   CookEntitySceneOperation>)
            {
                if (operation.job)
                {
                    const auto add = [&](std::size_t bytes)
                    {
                        const auto remaining =
                            std::numeric_limits<std::size_t>::max() -
                            options.accounted_bytes;
                        options.accounted_bytes +=
                            std::min(bytes, remaining);
                    };
                    add(sizeof(EntityScenePlayCookJob));
                    for (const auto& document : operation.job->documents)
                    {
                        add(document.relative_path.size());
                        add(document.bytes.size());
                    }
                    for (const auto& schema :
                         operation.job->component_schemas)
                    {
                        add(sizeof(schema));
                        add(schema.cpp_type.name.size());
                        add(schema.schema_id.name.size());
                        add(schema.provider.size());
                    }
                }
            }
            auto pipeline = lux::exec::execute(
                    client<Operation>(),
                    std::move(operation),
                    options)
                | ex::continues_on(lux::exec::mainThreadScheduler(*runtime))
                | ex::then(
                      [completion = std::move(completion)](
                          lux::exec::AsyncOutcome<Operation> outcome)
                          mutable noexcept
                      {
                          completion(std::move(outcome));
                      });
            return lux::exec::spawn(*scope, std::move(pipeline));
        }

        static bool submitFlowGraph(
            void* raw,
            CompileFlowGraph operation,
            FlowGraphCompileClient::Completion completion)
        {
            return static_cast<State*>(raw)->submit(
                std::move(operation),
                std::move(completion));
        }

    };

    bool FlowGraphCompileClient::submit(
        CompileFlowGraph operation,
        Completion completion) const
    {
        if (!submit_)
            return false;
        return submit_(context_, std::move(operation), std::move(completion));
    }

    lux::cxx::expected<EditorAsyncService, lux::exec::AsyncAssemblyFailure>
    EditorAsyncService::addTo(lux::exec::AsyncRuntimeBuilder& builder)
    {
        auto import = builder.addOperation<ImportAssetOperation>(
            [](ImportAssetOperation&& operation,
               lux::exec::AsyncOperationContext& context,
               lux::exec::AsyncOperationCompletion<ImportAssetOperation>&& completion) noexcept
            {
                auto sender = ex::schedule(
                        lux::exec::blockingIoScheduler(context.runtime()))
                    | ex::then(
                          [job = std::move(operation.job)]() noexcept
                          {
                              ImportAssetValue value;
                              if (!job)
                              {
                                  value.report = std::make_shared<ImportReport>();
                                  value.report->result = ImportResult::ImportFailed;
                                  value.report->message = "missing import job";
                                  return value;
                              }
                              value.source = job->source;
                              value.report = std::make_shared<ImportReport>(
                                  importExternalFileDetached(
                                      job->source,
                                      job->dest_root,
                                      job->options));
                              return value;
                          });
                launch<ImportAssetOperation>(
                    std::move(sender), context, std::move(completion));
            }, {}, lux::exec::AsyncOperationQueueConfig{
                .capacity = 256,
                .byte_budget = 64u * 1024u * 1024u,
                .drain_batch = 32});
        if (!import)
            return lux::cxx::unexpected(import.error());

        auto cook = builder.addOperation<CookContentOperation>(
            [](CookContentOperation&& operation,
               lux::exec::AsyncOperationContext& context,
               lux::exec::AsyncOperationCompletion<CookContentOperation>&& completion) noexcept
            {
                auto sender = ex::schedule(
                        lux::exec::blockingIoScheduler(context.runtime()))
                    | ex::then(
                          [operation = std::move(operation)]() mutable noexcept
                          {
                              CookContentValue value;
                              value.out_pak = operation.out_pak;
                              std::error_code create_error;
                              std::filesystem::create_directories(
                                  operation.out_pak.parent_path(),
                                  create_error);
                              if (create_error)
                              {
                                  value.message = lux::format(
                                      "cannot create cook directory ({})",
                                      create_error.message());
                                  return value;
                              }
                              auto cooked = lux::toolchain::cookDirectoryToPak(
                                  operation.content_dir,
                                  operation.out_pak,
                                  "/Game");
                              if (cooked)
                              {
                                  value.ok = true;
                                  value.asset_count = cooked->asset_count;
                              }
                              else
                                  value.message = cooked.error();
                              return value;
                          });
                launch<CookContentOperation>(
                    std::move(sender), context, std::move(completion));
            }, {}, lux::exec::AsyncOperationQueueConfig{
                .capacity = 128,
                .byte_budget = 64u * 1024u * 1024u,
                .drain_batch = 16});
        if (!cook)
            return lux::cxx::unexpected(cook.error());

        auto entity_scene_cook =
            builder.addOperation<CookEntitySceneOperation>(
            [](CookEntitySceneOperation&& operation,
               lux::exec::AsyncOperationContext& context,
               lux::exec::AsyncOperationCompletion<
                   CookEntitySceneOperation>&& completion) noexcept
            {
                auto sender = ex::schedule(
                        lux::exec::blockingIoScheduler(context.runtime()))
                    | ex::then(
                          [job = std::move(operation.job)]() mutable noexcept
                          {
                              PreparedEntitySceneCook prepared;
                              prepared.job = std::move(job);
                              if (!prepared.job)
                              {
                                  prepared.error =
                                      "missing EntityScene Cook job";
                                  return prepared;
                              }
                              for (const auto& document :
                                   prepared.job->documents)
                              {
                                  auto saved =
                                      lux::authoring::saveWorldSourceDocument(
                                          prepared.job->root_document,
                                          document.relative_path,
                                          document.bytes);
                                  if (!saved)
                                  {
                                      prepared.error = saved.error();
                                      return prepared;
                                  }
                              }
                              if (auto saved = lux::authoring::saveWorldSource(
                                      prepared.job->root_document,
                                      prepared.job->source);
                                  !saved)
                              {
                                  prepared.error = saved.error();
                                  return prepared;
                              }
                              std::unordered_set<std::string> authored;
                              authored.reserve(
                                  prepared.job->documents.size());
                              for (const auto& document :
                                   prepared.job->documents)
                              {
                                  authored.insert(document.relative_path);
                              }
                              const auto copy_if_needed = [&] (
                                  std::string_view relative)
                              {
                                  const auto path = std::string{relative};
                                  if (authored.contains(path))
                                      return true;
                                  if (relative.empty() ||
                                      prepared.job->source_root_document.empty()
                                      || !copyWorldSourceObject(
                                          prepared.job->source_root_document,
                                          prepared.job->root_document,
                                          relative))
                                  {
                                      return false;
                                  }
                                  authored.insert(path);
                                  return true;
                              };
                              const auto copy_page_children = [&] (
                                  const auto& page)
                              {
                                  for (const auto& actor : page.actors)
                                  {
                                      if (!copy_if_needed(actor.document_path))
                                          return false;
                                  }
                                  for (const auto& content : page.pages)
                                  {
                                      if (!copy_if_needed(
                                              content.document_path))
                                      {
                                          return false;
                                      }
                                  }
                                  return true;
                              };
                              for (const auto& reference :
                                   prepared.job->source.descriptor_pages)
                              {
                                  const auto authored_page = std::ranges::find(
                                      prepared.job->descriptor_pages,
                                      reference.id,
                                      &lux::authoring::
                                          WorldDescriptorPageDocument::id);
                                  if (authored_page !=
                                      prepared.job->descriptor_pages.end())
                                  {
                                      if (!authored.contains(
                                              reference.document_path) ||
                                          !copy_page_children(*authored_page))
                                      {
                                          prepared.error =
                                              "cannot prepare authored Descriptor Page closure";
                                          return prepared;
                                      }
                                      continue;
                                  }
                                  if (prepared.job->source_root_document.empty())
                                  {
                                      prepared.error =
                                          "unloaded Authoring content has no source root";
                                      return prepared;
                                  }
                                  auto page = lux::authoring::
                                      loadWorldDescriptorPage(
                                          prepared.job->source_root_document,
                                          prepared.job->source,
                                          reference);
                                  if (!page ||
                                      !copy_if_needed(
                                          reference.document_path) ||
                                      !copy_page_children(*page))
                                  {
                                      prepared.error =
                                          "cannot prepare unloaded Descriptor Page closure";
                                      return prepared;
                                  }
                              }
                              auto source = lux::toolchain::
                                  loadSpatial3DAuthoringSource(
                                      prepared.job->root_document);
                              if (!source)
                              {
                                  prepared.error = std::move(source.error());
                                  return prepared;
                              }
                              prepared.source = std::move(*source);
                              std::set<std::string, std::less<>> mesh_ids;
                              for (const auto& page :
                                   prepared.source->instance_pages)
                              for (const auto& instance : page.instances)
                              {
                                  mesh_ids.insert(
                                      uuids::to_string(instance.mesh));
                              }
                              prepared.mesh_assets.meshes.reserve(
                                  mesh_ids.size());
                              for (const auto& key : mesh_ids)
                              {
                                  const auto id =
                                      uuids::uuid::from_string(key);
                                  if (!id || !prepared.job->asset_vfs)
                                  {
                                      prepared.error =
                                          "Play Cook has no asset source for Mesh '" +
                                          key + "'";
                                      return prepared;
                                  }
                                  auto image =
                                      prepared.job->asset_vfs->open(*id);
                                  if (!image || image->bytes.empty())
                                  {
                                      prepared.error =
                                          "Play Cook cannot open Mesh '" +
                                          key + "'";
                                      return prepared;
                                  }
                                  const auto bytes = image->bytes.view();
                                  prepared.mesh_assets.meshes.push_back({
                                      *id,
                                      std::vector<std::byte>{
                                          bytes.begin(), bytes.end()}});
                              }
                              return prepared;
                          })
                    | ex::continues_on(
                          lux::exec::backgroundCpuScheduler(context.runtime()))
                    | ex::then(
                          [](PreparedEntitySceneCook prepared) noexcept
                          {
                              CookedEntitySceneWork value;
                              if (!prepared.job)
                              {
                                  value.error = std::move(prepared.error);
                                  return value;
                              }
                              value.root_document =
                                  prepared.job->root_document;
                              if (!prepared.error.empty())
                              {
                                  value.error = std::move(prepared.error);
                                  return value;
                              }
                              lux::ecs::ComponentTypeCatalog components;
                              auto registered = components.registerSchemas(
                                  prepared.job->component_schemas);
                              if (!registered)
                              {
                                  value.error =
                                      "frozen component schema registration failed at '" +
                                      registered.error().name + "'";
                                  return value;
                              }
                              if (*registered == 0u)
                              {
                                  value.error =
                                      "frozen component schema snapshot is empty";
                                  return value;
                              }
                              if (!prepared.source)
                              {
                                  value.error =
                                      "prepared Spatial3D Authoring source is absent";
                                  return value;
                              }
                              auto cooked = lux::toolchain::
                                  adaptSpatial3DEntityScene(
                                      *prepared.source,
                                      components,
                                      prepared.mesh_assets);
                              if (!cooked)
                              {
                                  value.error = cooked.error().detail;
                                  return value;
                              }
                              value.bundle = std::move(*cooked);
                              return value;
                          })
                    | ex::continues_on(
                          lux::exec::blockingIoScheduler(context.runtime()))
                    | ex::then(
                          [](CookedEntitySceneWork work) noexcept
                          {
                              CookEntitySceneValue value;
                              if (!work.bundle)
                              {
                                  value.message = std::move(work.error);
                                  return value;
                              }
                              value.pak_file = playEntityScenePakPath(
                                  work.root_document,
                                  *work.bundle);
                              std::error_code create_error;
                              std::filesystem::create_directories(
                                  value.pak_file.parent_path(),
                                  create_error);
                              if (create_error)
                              {
                                  value.pak_file.clear();
                                  value.message =
                                      "cannot create Play EntityScene directory: " +
                                      create_error.message();
                                  return value;
                              }
                              auto published = publishPlayEntityScenePak(
                                  std::move(*work.bundle),
                                  value.pak_file);
                              if (!published)
                              {
                                  value.pak_file.clear();
                                  value.message = std::move(published.error());
                                  return value;
                              }
                              value.scene_id = *published;
                              value.scene_origin =
                                  value.pak_file.generic_string() +
                                  "#/Game/Scenes/Play";
                              return value;
                          });
                launch<CookEntitySceneOperation>(
                    std::move(sender), context, std::move(completion));
            }, {}, lux::exec::AsyncOperationQueueConfig{
                .capacity = 8,
                .byte_budget = 512u * 1024u * 1024u,
                .drain_batch = 2});
        if (!entity_scene_cook)
            return lux::cxx::unexpected(entity_scene_cook.error());

        auto world_source_gc =
            builder.addOperation<CollectWorldSourceGarbageOperation>(
                [](CollectWorldSourceGarbageOperation&& operation,
                   lux::exec::AsyncOperationContext& context,
                   lux::exec::AsyncOperationCompletion<
                       CollectWorldSourceGarbageOperation>&& completion)
                    noexcept
                {
                    auto sender = ex::schedule(
                            lux::exec::blockingIoScheduler(context.runtime()))
                        | ex::then(
                              [operation = std::move(operation)]()
                                  mutable noexcept
                              {
                                  CollectWorldSourceGarbageValue value;
                                  auto collected = lux::authoring::
                                      collectWorldSourceGarbage(
                                          operation.world_file,
                                          lux::authoring::
                                              WorldSourceGarbageCollectionConfig{
                                                  .grace_period =
                                                      std::chrono::seconds{
                                                          operation.
                                                              grace_period_seconds},
                                                  .maximum_removals_per_pass =
                                                      operation.
                                                          maximum_removals_per_pass});
                                  if (!collected)
                                  {
                                      value.error = collected.error();
                                      return value;
                                  }
                                  value.live_documents =
                                      collected->live_documents;
                                  value.scanned_documents =
                                      collected->scanned_documents;
                                  value.removed_documents =
                                      collected->removed_documents;
                                  value.deferred_documents =
                                      collected->deferred_documents;
                                  value.removal_budget_exhausted =
                                      collected->removal_budget_exhausted;
                                  return value;
                              });
                    launch<CollectWorldSourceGarbageOperation>(
                        std::move(sender), context, std::move(completion));
                }, {}, lux::exec::AsyncOperationQueueConfig{
                    .capacity = 8,
                    .byte_budget = 1u * 1024u * 1024u,
                    .drain_batch = 1});
        if (!world_source_gc)
            return lux::cxx::unexpected(world_source_gc.error());

        auto descriptor_index =
            builder.addOperation<RebuildWorldDescriptorIndexOperation>(
                [](RebuildWorldDescriptorIndexOperation&& operation,
                   lux::exec::AsyncOperationContext& context,
                   lux::exec::AsyncOperationCompletion<
                       RebuildWorldDescriptorIndexOperation>&& completion)
                    noexcept
                {
                    auto sender = ex::schedule(
                            lux::exec::blockingIoScheduler(context.runtime()))
                        | ex::then(
                              [operation = std::move(operation)]()
                                  mutable noexcept
                              {
                                  RebuildWorldDescriptorIndexValue value;
                                  if (!operation.source)
                                  {
                                      value.error =
                                          "missing Authoring World snapshot";
                                      return value;
                                  }
                                  auto index = lux::authoring::
                                      WorldDescriptorIndex::rebuild(
                                          operation.world_file,
                                          *operation.source,
                                          operation.cache_file);
                                  if (!index)
                                  {
                                      value.error = index.error();
                                      return value;
                                  }
                                  value.index = std::make_shared<const
                                      lux::authoring::WorldDescriptorIndex>(
                                          std::move(*index));
                                  return value;
                              });
                    launch<RebuildWorldDescriptorIndexOperation>(
                        std::move(sender), context, std::move(completion));
                },
                {},
                lux::exec::AsyncOperationQueueConfig{
                    .capacity = 4,
                    .byte_budget = 64u * 1024u * 1024u,
                    .drain_batch = 1});
        if (!descriptor_index)
            return lux::cxx::unexpected(descriptor_index.error());

        auto actor_proxy = builder.addOperation<LoadWorldActorProxyOperation>(
            [](LoadWorldActorProxyOperation&& operation,
               lux::exec::AsyncOperationContext& context,
               lux::exec::AsyncOperationCompletion<
                   LoadWorldActorProxyOperation>&& completion) noexcept
            {
                auto sender = ex::schedule(
                        lux::exec::blockingIoScheduler(context.runtime()))
                    | ex::then(
                          [operation = std::move(operation)]() mutable noexcept
                          {
                              LoadWorldActorProxyValue value;
                              if (!operation.source ||
                                  operation.actor.empty())
                              {
                                  value.error =
                                      "missing Authoring proxy request data";
                                  return value;
                              }
                              if (operation.descriptor)
                              {
                                  if (operation.descriptor->id !=
                                      operation.actor)
                                  {
                                      value.error =
                                          "cached Descriptor identity does not match Actor request";
                                      return value;
                                  }
                                  value.descriptor =
                                      std::move(*operation.descriptor);
                              }
                              else
                              {
                                  auto page =
                                      lux::authoring::loadWorldDescriptorPage(
                                          operation.world_file,
                                          *operation.source,
                                          operation.page);
                                  if (!page)
                                  {
                                      value.error = page.error();
                                      return value;
                                  }
                                  const auto descriptor = std::ranges::find(
                                      page->actors,
                                      operation.actor,
                                      &lux::authoring::
                                          WorldActorSourceDescriptor::id);
                                  if (descriptor == page->actors.end())
                                  {
                                      value.error =
                                          "Descriptor Page does not contain Actor";
                                      return value;
                                  }
                                  value.descriptor = *descriptor;
                                  value.page = std::move(*page);
                              }
                              auto document =
                                  lux::authoring::loadWorldActorDocument(
                                      operation.world_file,
                                      value.descriptor);
                              if (!document)
                              {
                                  value.error = document.error();
                                  return value;
                              }
                              if (document->world != operation.source->world ||
                                  document->actor != value.descriptor.id ||
                                  document->actor_class !=
                                      value.descriptor.actor_class ||
                                  document->space != value.descriptor.space ||
                                  document->position !=
                                      value.descriptor.position ||
                                  document->data_layers !=
                                      value.descriptor.data_layers ||
                                  document->references !=
                                      value.descriptor.references)
                              {
                                  value.error =
                                      "LXAD Actor source metadata does not match its Descriptor";
                                  return value;
                              }
                              value.document = std::move(*document);
                              return value;
                          });
                launch<LoadWorldActorProxyOperation>(
                    std::move(sender), context, std::move(completion));
            },
            {},
            lux::exec::AsyncOperationQueueConfig{
                .capacity = 256,
                .byte_budget = 256u * 1024u * 1024u,
                .drain_batch = 16});
        if (!actor_proxy)
            return lux::cxx::unexpected(actor_proxy.error());

        auto descriptor_page =
            builder.addOperation<LoadWorldDescriptorPageOperation>(
                [](LoadWorldDescriptorPageOperation&& operation,
                   lux::exec::AsyncOperationContext& context,
                   lux::exec::AsyncOperationCompletion<
                       LoadWorldDescriptorPageOperation>&& completion)
                    noexcept
                {
                    auto sender = ex::schedule(
                            lux::exec::blockingIoScheduler(context.runtime()))
                        | ex::then(
                              [operation = std::move(operation)]()
                                  mutable noexcept
                              {
                                  LoadWorldDescriptorPageValue value;
                                  if (!operation.source ||
                                      operation.page.id.is_nil())
                                  {
                                      value.error =
                                          "missing Descriptor Page request data";
                                      return value;
                                  }
                                  auto page = lux::authoring::
                                      loadWorldDescriptorPage(
                                          operation.world_file,
                                          *operation.source,
                                          operation.page);
                                  if (!page)
                                  {
                                      value.error = page.error();
                                      return value;
                                  }
                                  value.page = std::move(*page);
                                  return value;
                              });
                    launch<LoadWorldDescriptorPageOperation>(
                        std::move(sender), context, std::move(completion));
                },
                {},
                lux::exec::AsyncOperationQueueConfig{
                    .capacity = 128,
                    .byte_budget = 64u * 1024u * 1024u,
                    .drain_batch = 8});
        if (!descriptor_page)
            return lux::cxx::unexpected(descriptor_page.error());

        auto instance_page =
            builder.addOperation<LoadWorldInstancePageOperation>(
                [](LoadWorldInstancePageOperation&& operation,
                   lux::exec::AsyncOperationContext& context,
                   lux::exec::AsyncOperationCompletion<
                       LoadWorldInstancePageOperation>&& completion)
                    noexcept
                {
                    auto sender = ex::schedule(
                            lux::exec::blockingIoScheduler(context.runtime()))
                        | ex::then(
                              [operation = std::move(operation)]()
                                  mutable noexcept
                              {
                                  LoadWorldInstancePageValue value;
                                  value.descriptor = operation.page;
                                  if (!operation.source ||
                                      operation.page.kind !=
                                          lux::authoring::
                                              EWorldPageSourceKind::INSTANCE)
                                  {
                                      value.error =
                                          "invalid Instance Page request";
                                      return value;
                                  }
                                  auto page = lux::authoring::
                                      loadWorldInstancePage(
                                          operation.world_file,
                                          operation.page.document_path,
                                          *operation.source,
                                          {});
                                  if (!page)
                                  {
                                      value.error = page.error();
                                      return value;
                                  }
                                  const auto* owner = std::get_if<
                                      lux::authoring::InstanceSetId>(
                                          &operation.page.owner);
                                  const auto canonical = lux::authoring::
                                      encodeWorldInstancePage(
                                          *operation.source, *page);
                                  if (!owner || page->instance_set != *owner ||
                                      page->space != operation.page.space ||
                                      page->cell != operation.page.cell ||
                                      !canonical ||
                                      lux::cxx::algorithm::Sha256::hash(*canonical) !=
                                          operation.page.content_digest)
                                  {
                                      value.error =
                                          "LXIP identity or digest does not match LXAI";
                                      return value;
                                  }
                                  value.page = std::move(*page);
                                  return value;
                              });
                    launch<LoadWorldInstancePageOperation>(
                        std::move(sender), context, std::move(completion));
                },
                {},
                lux::exec::AsyncOperationQueueConfig{
                    .capacity = 256,
                    .byte_budget = 256u * 1024u * 1024u,
                    .drain_batch = 8});
        if (!instance_page)
            return lux::cxx::unexpected(instance_page.error());

        auto terrain_region =
            builder.addOperation<LoadWorldTerrainRegionOperation>(
                [](LoadWorldTerrainRegionOperation&& operation,
                   lux::exec::AsyncOperationContext& context,
                   lux::exec::AsyncOperationCompletion<
                       LoadWorldTerrainRegionOperation>&& completion) noexcept
                {
                    auto sender = ex::schedule(
                            lux::exec::blockingIoScheduler(context.runtime()))
                        | ex::then(
                              [operation = std::move(operation)]()
                                  mutable noexcept
                              {
                                  LoadWorldTerrainRegionValue value;
                                  if (!operation.source ||
                                      operation.terrain.empty() ||
                                      operation.space.empty() ||
                                      operation.cells.empty())
                                  {
                                      value.error =
                                          "missing Terrain region request data";
                                      return value;
                                  }
                                  const auto space = std::ranges::find(
                                      operation.source->spaces,
                                      operation.space,
                                      &lux::authoring::
                                          PartitionSpaceDescriptor::id);
                                  if (space == operation.source->spaces.end() ||
                                      space->topology !=
                                          lux::authoring::EPartitionTopology::
                                              PLANAR_XZ)
                                  {
                                      value.error =
                                          "Terrain region names an invalid Space";
                                      return value;
                                  }
                                  std::unordered_map<std::string, std::size_t>
                                      loaded_macros;
                                  for (const auto& cell : operation.cells)
                                  {
                                      if (cell.topology !=
                                              lux::authoring::EPartitionTopology::
                                                  PLANAR_XZ ||
                                          !std::holds_alternative<
                                              lux::authoring::PlanarCellCoord>(
                                              cell.coordinate))
                                      {
                                          value.error =
                                              "Terrain request contains a non-XZ Cell";
                                          return value;
                                      }
                                      const auto macro =
                                          lux::authoring::macroCoordOf(
                                              cell,
                                              space->macro_edge_cells);
                                      if (!macro)
                                      {
                                          value.error =
                                              "Terrain Cell cannot map to a Macro";
                                          return value;
                                      }
                                      const auto reference = std::ranges::find_if(
                                          operation.source->descriptor_pages,
                                          [&](const auto& candidate)
                                          {
                                              return candidate.space ==
                                                      operation.space &&
                                                  candidate.macro == *macro;
                                          });
                                      if (reference == operation.source->
                                              descriptor_pages.end())
                                      {
                                          value.error =
                                              "Terrain Macro Descriptor Page is absent";
                                          return value;
                                      }
                                      const auto macro_key =
                                          uuids::to_string(reference->id);
                                      auto loaded = loaded_macros.find(macro_key);
                                      if (loaded == loaded_macros.end())
                                      {
                                          auto page = lux::authoring::
                                              loadWorldDescriptorPage(
                                                  operation.world_file,
                                                  *operation.source,
                                                  *reference);
                                          if (!page)
                                          {
                                              value.error = page.error();
                                              return value;
                                          }
                                          loaded = loaded_macros.emplace(
                                              macro_key,
                                              value.descriptor_pages.size()).first;
                                          value.descriptor_pages.push_back(
                                              std::move(*page));
                                      }
                                      const auto& descriptor_page =
                                          value.descriptor_pages[loaded->second];
                                      const auto descriptor = std::ranges::find_if(
                                          descriptor_page.pages,
                                          [&](const auto& candidate)
                                          {
                                              return candidate.kind ==
                                                      lux::authoring::
                                                          EWorldPageSourceKind::
                                                              TERRAIN &&
                                                  candidate.owner ==
                                                      lux::authoring::
                                                          WorldPageSourceOwner{
                                                              operation.terrain} &&
                                                  candidate.space ==
                                                      operation.space &&
                                                  candidate.cell == cell;
                                          });
                                      if (descriptor ==
                                          descriptor_page.pages.end())
                                      {
                                          value.error =
                                              "Terrain Cell is absent from Descriptor Page";
                                          return value;
                                      }
                                      auto page = lux::authoring::
                                          loadWorldTerrainPage(
                                              operation.world_file,
                                              descriptor->document_path,
                                              *operation.source);
                                      if (!page)
                                      {
                                          value.error = page.error();
                                          return value;
                                      }
                                      auto canonical = lux::authoring::
                                          encodeWorldTerrainPage(
                                              *operation.source, *page);
                                      if (!canonical ||
                                          lux::cxx::algorithm::Sha256::hash(*canonical) !=
                                              descriptor->content_digest ||
                                          page->world !=
                                              operation.source->world ||
                                          page->terrain_set !=
                                              operation.terrain ||
                                          page->space != descriptor->space ||
                                          page->cell != descriptor->cell)
                                      {
                                          value.error =
                                              "LXTP digest or identity mismatch";
                                          return value;
                                      }
                                      value.pages.push_back(std::move(*page));
                                  }
                                  return value;
                              });
                    launch<LoadWorldTerrainRegionOperation>(
                        std::move(sender), context, std::move(completion));
                },
                {},
                lux::exec::AsyncOperationQueueConfig{
                    .capacity = 32,
                    .byte_budget = 512u * 1024u * 1024u,
                    .drain_batch = 4});
        if (!terrain_region)
            return lux::cxx::unexpected(terrain_region.error());

        auto terrain_heightmap_file =
            builder.addOperation<WorldTerrainHeightmapFileOperation>(
                [](WorldTerrainHeightmapFileOperation&& operation,
                   lux::exec::AsyncOperationContext& context,
                   lux::exec::AsyncOperationCompletion<
                       WorldTerrainHeightmapFileOperation>&& completion)
                    noexcept
                {
                    auto sender = ex::schedule(
                            lux::exec::blockingIoScheduler(context.runtime()))
                        | ex::then(
                              [operation = std::move(operation)]()
                                  mutable noexcept
                              {
                                  WorldTerrainHeightmapFileValue value;
                                  value.path = operation.path;
                                  value.image = std::move(operation.image);
                                  const auto sample_count =
                                      static_cast<std::uint64_t>(
                                          value.image.width) *
                                      value.image.height;
                                  if (value.path.empty() ||
                                      sample_count == 0u ||
                                      sample_count >
                                          512ull * 1024ull * 1024ull)
                                  {
                                      value.error =
                                          "invalid RAW16 heightmap request";
                                      return value;
                                  }
                                  const auto byte_count = sample_count * 2u;
                                  if (operation.mode ==
                                      EWorldTerrainHeightmapFileMode::READ)
                                  {
                                      std::ifstream stream(
                                          value.path,
                                          std::ios::binary | std::ios::ate);
                                      if (!stream || stream.tellg() < 0 ||
                                          static_cast<std::uint64_t>(
                                              stream.tellg()) != byte_count)
                                      {
                                          value.error =
                                              "RAW16 file size does not match the Terrain region";
                                          return value;
                                      }
                                      stream.seekg(0);
                                      std::vector<std::byte> bytes(
                                          static_cast<std::size_t>(byte_count));
                                      if (!bytes.empty() && !stream.read(
                                              reinterpret_cast<char*>(
                                                  bytes.data()),
                                              static_cast<std::streamsize>(
                                                  bytes.size())))
                                      {
                                          value.error =
                                              "cannot read RAW16 heightmap";
                                          return value;
                                      }
                                      auto decoded = lux::authoring::
                                          decodeWorldTerrainRaw16(
                                              bytes,
                                              value.image.width,
                                              value.image.height,
                                              value.image.height_min,
                                              value.image.height_max);
                                      if (!decoded)
                                      {
                                          value.error = decoded.error().detail;
                                          return value;
                                      }
                                      value.image = std::move(*decoded);
                                      return value;
                                  }
                                  auto encoded = lux::authoring::
                                      encodeWorldTerrainRaw16(value.image);
                                  if (!encoded)
                                  {
                                      value.error = encoded.error().detail;
                                      return value;
                                  }
                                  std::error_code create_error;
                                  if (!value.path.parent_path().empty())
                                  {
                                      std::filesystem::create_directories(
                                          value.path.parent_path(),
                                          create_error);
                                  }
                                  if (create_error)
                                  {
                                      value.error =
                                          "cannot create RAW16 output directory";
                                      return value;
                                  }
                                  std::ofstream stream(
                                      value.path,
                                      std::ios::binary | std::ios::trunc);
                                  if (!stream)
                                  {
                                      value.error =
                                          "cannot open RAW16 output";
                                      return value;
                                  }
                                  if (!encoded->empty() && !stream.write(
                                          reinterpret_cast<const char*>(
                                              encoded->data()),
                                          static_cast<std::streamsize>(
                                              encoded->size())))
                                  {
                                      value.error =
                                          "cannot write RAW16 heightmap";
                                      return value;
                                  }
                                  return value;
                              });
                    launch<WorldTerrainHeightmapFileOperation>(
                        std::move(sender), context, std::move(completion));
                },
                {},
                lux::exec::AsyncOperationQueueConfig{
                    .capacity = 8,
                    .byte_budget = 2ull * 1024ull * 1024ull * 1024ull,
                    .drain_batch = 2});
        if (!terrain_heightmap_file)
            return lux::cxx::unexpected(terrain_heightmap_file.error());

        auto reload = builder.addOperation<ReloadAssetOperation>(
            [](ReloadAssetOperation&& operation,
               lux::exec::AsyncOperationContext& context,
               lux::exec::AsyncOperationCompletion<ReloadAssetOperation>&& completion) noexcept
            {
                constexpr auto kSettleDuration =
                    std::chrono::milliseconds{500};
                auto files = context.runtime().fileService().client();
                auto sender = lux::exec::scheduleAfter(
                        context.runtime(), kSettleDuration)
                    | ex::let_value(
                          [files = std::move(files),
                           id = operation.id,
                           generation = operation.generation,
                           path = std::move(operation.abs_path)]()
                              mutable noexcept
                          {
                              return lux::exec::readFile(
                                         std::move(files),
                                         std::move(path))
                                  | ex::then(
                                        [id, generation](
                                            lux::exec::AsyncFileReadResult bytes)
                                            mutable noexcept
                                        {
                                            return ReloadBytes{
                                                id,
                                                generation,
                                                std::move(bytes)};
                                        });
                          })
                    | ex::continues_on(
                          lux::exec::backgroundCpuScheduler(context.runtime()))
                    | ex::then(&decodeReload);
                launch<ReloadAssetOperation>(
                    std::move(sender), context, std::move(completion));
            }, {}, lux::exec::AsyncOperationQueueConfig{
                .capacity = 512,
                .byte_budget = 64u * 1024u * 1024u,
                .drain_batch = 32});
        if (!reload)
            return lux::cxx::unexpected(reload.error());

        auto material = builder.addOperation<CompileMaterialOperation>(
            [](CompileMaterialOperation&& operation,
               lux::exec::AsyncOperationContext& context,
               lux::exec::AsyncOperationCompletion<CompileMaterialOperation>&& completion) noexcept
            {
                auto sender = ex::schedule(
                        lux::exec::backgroundCpuScheduler(context.runtime()))
                    | ex::then(
                          [job = std::move(operation.job)]() noexcept
                          {
                              return job
                                  ? compileMaterialJob(*job)
                                  : std::make_shared<MaterialCompileOutcome>(
                                        MaterialCompileOutcome{
                                            .ok = false,
                                            .status = "missing material compile job"});
                          });
                launch<CompileMaterialOperation>(
                    std::move(sender), context, std::move(completion));
            }, {}, lux::exec::AsyncOperationQueueConfig{
                .capacity = 128,
                .byte_budget = 64u * 1024u * 1024u,
                .drain_batch = 16});
        if (!material)
            return lux::cxx::unexpected(material.error());

        auto flow_graph = builder.addOperation<CompileFlowGraph>(
            [](CompileFlowGraph&& operation,
               lux::exec::AsyncOperationContext& context,
               lux::exec::AsyncOperationCompletion<CompileFlowGraph>&& completion) noexcept
            {
                auto sender = ex::schedule(
                        lux::exec::backgroundCpuScheduler(context.runtime()))
                    | ex::then(
                          [job = std::move(operation.job)]() noexcept
                          {
                              return job
                                  ? compileFlowGraphJob(*job)
                                  : FlowGraphCompileResult{
                                        .error = "missing FlowGraph compile job"};
                          });
                launch<CompileFlowGraph>(
                    std::move(sender), context, std::move(completion));
            }, {}, lux::exec::AsyncOperationQueueConfig{
                .capacity = 128,
                .byte_budget = 64u * 1024u * 1024u,
                .drain_batch = 16});
        if (!flow_graph)
            return lux::cxx::unexpected(flow_graph.error());

        auto state = std::make_shared<State>();
        state->import = std::move(*import);
        state->cook = std::move(*cook);
        state->entity_scene_cook = std::move(*entity_scene_cook);
        state->world_source_gc = std::move(*world_source_gc);
        state->descriptor_index = std::move(*descriptor_index);
        state->actor_proxy = std::move(*actor_proxy);
        state->descriptor_page = std::move(*descriptor_page);
        state->instance_page = std::move(*instance_page);
        state->terrain_region = std::move(*terrain_region);
        state->terrain_heightmap_file =
            std::move(*terrain_heightmap_file);
        state->reload = std::move(*reload);
        state->material = std::move(*material);
        state->flow_graph = std::move(*flow_graph);
        return EditorAsyncService{std::move(state)};
    }

    EditorAsyncService::~EditorAsyncService()
    {
        if (state_ && state_->scope)
            state_->scope->requestStop();
    }

    EditorAsyncService::EditorAsyncService(EditorAsyncService&& other) noexcept
        : state_(std::move(other.state_))
    {}

    EditorAsyncService& EditorAsyncService::operator=(
        EditorAsyncService&& other) noexcept
    {
        if (this == &other)
            return *this;
        if (state_ && state_->scope)
            state_->scope->requestStop();
        state_ = std::move(other.state_);
        return *this;
    }

    void EditorAsyncService::bind(lux::exec::AsyncRuntime& runtime)
    {
        if (!state_)
            return;
        state_->runtime = &runtime;
        state_->scope = std::make_unique<lux::exec::AsyncScope>(runtime);
    }

    lux::exec::AsyncScopeCloseSender
    EditorAsyncService::closeAsync() noexcept
    {
        if (!state_ || !state_->scope)
            return {};
        state_->closing = true;
        return state_->scope->closeAsync();
    }

    FlowGraphCompileClient
    EditorAsyncService::flowGraphCompileClient() const noexcept
    {
        if (!state_)
            return {};
        return FlowGraphCompileClient{
            state_,
            state_.get(),
            &State::submitFlowGraph};
    }

    bool EditorAsyncService::importAsset(
        ImportAssetOperation operation,
        Completion<ImportAssetOperation> completion)
    {
        return state_ && state_->submit(
            std::move(operation), std::move(completion));
    }

    bool EditorAsyncService::cookContent(
        CookContentOperation operation,
        Completion<CookContentOperation> completion)
    {
        return state_ && state_->submit(
            std::move(operation), std::move(completion));
    }

    bool EditorAsyncService::reloadAsset(
        ReloadAssetOperation operation,
        Completion<ReloadAssetOperation> completion)
    {
        return state_ && state_->submit(
            std::move(operation), std::move(completion));
    }

    bool EditorAsyncService::cookEntityScene(
        CookEntitySceneOperation operation,
        Completion<CookEntitySceneOperation> completion)
    {
        return state_ && state_->submit(
            std::move(operation), std::move(completion));
    }

    bool EditorAsyncService::collectWorldSourceGarbage(
        CollectWorldSourceGarbageOperation operation,
        Completion<CollectWorldSourceGarbageOperation> completion)
    {
        return state_ && state_->submit(
            std::move(operation), std::move(completion));
    }

    bool EditorAsyncService::rebuildWorldDescriptorIndex(
        RebuildWorldDescriptorIndexOperation operation,
        Completion<RebuildWorldDescriptorIndexOperation> completion)
    {
        return state_ && state_->submit(
            std::move(operation), std::move(completion));
    }

    bool EditorAsyncService::loadWorldActorProxy(
        LoadWorldActorProxyOperation operation,
        Completion<LoadWorldActorProxyOperation> completion)
    {
        return state_ && state_->submit(
            std::move(operation), std::move(completion));
    }

    bool EditorAsyncService::loadWorldDescriptorPage(
        LoadWorldDescriptorPageOperation operation,
        Completion<LoadWorldDescriptorPageOperation> completion)
    {
        return state_ && state_->submit(
            std::move(operation), std::move(completion));
    }

    bool EditorAsyncService::loadWorldInstancePage(
        LoadWorldInstancePageOperation operation,
        Completion<LoadWorldInstancePageOperation> completion)
    {
        return state_ && state_->submit(
            std::move(operation), std::move(completion));
    }

    bool EditorAsyncService::loadWorldTerrainRegion(
        LoadWorldTerrainRegionOperation operation,
        Completion<LoadWorldTerrainRegionOperation> completion)
    {
        return state_ && state_->submit(
            std::move(operation), std::move(completion));
    }

    bool EditorAsyncService::worldTerrainHeightmapFile(
        WorldTerrainHeightmapFileOperation operation,
        Completion<WorldTerrainHeightmapFileOperation> completion)
    {
        return state_ && state_->submit(
            std::move(operation), std::move(completion));
    }

    bool EditorAsyncService::compileMaterial(
        CompileMaterialOperation operation,
        Completion<CompileMaterialOperation> completion)
    {
        return state_ && state_->submit(
            std::move(operation), std::move(completion));
    }

}
