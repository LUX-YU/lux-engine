#include <lux/engine/toolchain/game_export/GameExporter.hpp>

#include <lux/engine/authoring/world/WorldSourceCodec.hpp>
#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/engine/resource/asset/AssetCodecCatalog.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/scene/ScenePackageCodec.hpp>
#include <lux/engine/toolchain/asset/cook/PakCook.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/MeshSerDeser.hpp>
#include <lux/engine/resource/asset/PakAssetProvider.hpp>
#include <lux/engine/resource/classic_mesh/ClassicMeshBatch.hpp>
#include <lux/game/LaunchManifest.hpp>

#include <chrono>
#include <atomic>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

namespace
{
    struct TempTree final
    {
        std::filesystem::path root;

        ~TempTree()
        {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    };

    bool writeProject(
        const std::filesystem::path& path,
        bool with_default_world = false)
    {
        std::ofstream stream(path, std::ios::binary);
        stream <<
            "[project]\n"
            "name = \"ExporterSmoke\"\n"
            "display_name = \"Exporter Smoke\"\n"
            "project_guid = \"550e8400-e29b-41d4-a716-446655440000\"\n"
            "engine_version = \"0.0.1\"\n"
            "binary_name = \"ExporterSmoke\"\n";
        if (with_default_world)
            stream << "default_world = \"Worlds/Main.luxworld\"\n";
        stream <<
            "\n[[extensions]]\n"
            "id = \"org.lux.test.editor-only\"\n"
            "path = \"missing-editor-module\"\n"
            "target = \"editor\"\n"
            "major = 1\n"
            "minimum_minor = 0\n";
#if defined(LUX_GAME_EXPORT_TEST_WITH_EXTENSION)
        stream <<
            "\n[[extensions]]\n"
            "id = \"org.lux.physics2d\"\n"
            "path = \"" <<
                std::filesystem::path{LUX_GAME_EXPORT_TEST_EXTENSION_DIR}
                    .generic_string() << "\"\n"
            "target = \"runtime\"\n"
            "major = 1\n"
            "minimum_minor = 0\n";
#endif
        return stream.good();
    }

    lux::rdesc::Mesh exportHlodMesh(float x_offset)
    {
        lux::rdesc::Mesh mesh;
        mesh.vertices.resize(3u);
        mesh.vertices[0].position = {x_offset, 0.0f, 0.0f};
        mesh.vertices[1].position = {x_offset + 1.0f, 0.0f, 0.0f};
        mesh.vertices[2].position = {x_offset, 1.0f, 0.0f};
        for (auto& vertex : mesh.vertices)
        {
            vertex.normal = {0.0f, 0.0f, 1.0f};
            vertex.tangent = {1.0f, 0.0f, 0.0f};
            vertex.bitangent = {0.0f, 1.0f, 0.0f};
            vertex.uv = {vertex.position.x(), vertex.position.y()};
            vertex.bone = {};
        }
        mesh.indices = {0u, 1u, 2u};
        mesh.bounds = lux::math::AABB{
            {x_offset, 0.0f, 0.0f},
            {x_offset + 1.0f, 1.0f, 0.0f}};
        return mesh;
    }

    bool writeWorldSource(const std::filesystem::path& path)
    {
        const auto world_id = uuids::uuid::from_string(
            "560e8400-e29b-41d4-a716-446655440000").value();
        const auto actor_id = uuids::uuid::from_string(
            "560e8400-e29b-41d4-a716-446655440001").value();
        lux::authoring::WorldActorDocument actor_document;
        actor_document.world = lux::authoring::WorldId{world_id};
        actor_document.actor =
            lux::authoring::WorldActorId{actor_id};
        actor_document.name_table = {
            std::byte{1u}, std::byte{0u},
            std::byte{0u}, std::byte{0u}};
        using namespace lux::authoring;
        lux::authoring::WorldSourceDocument source;
        source.world = lux::authoring::WorldId{world_id};
        PartitionSpaceDescriptor space;
        space.id = PartitionSpaceId{
            uuids::uuid::from_string(
                "560e8400-e29b-41d4-a716-446655440010").value()};
        space.topology = EPartitionTopology::PLANAR_XZ;
        space.cell_edge = 128.0f;
        space.macro_edge_cells = 32u;
        source.spaces.push_back(space);
        lux::authoring::WorldActorSourceDescriptor actor;
        actor.id = lux::authoring::WorldActorId{actor_id};
        actor.display_name = "Exporter Smoke Actor";
        actor.actor_class = "org.lux.test.exporter_actor";
        actor.space = space.id;
        actor.position = lux::spatial::Position3D{0.0, 0.0, 0.0};
        actor_document.actor_class = actor.actor_class;
        actor_document.space = actor.space;
        actor_document.position = actor.position;
        actor_document.data_layers = actor.data_layers;
        actor_document.references = actor.references;
        auto encoded_actor = lux::authoring::encodeWorldActorDocument(
            actor_document);
        if (!encoded_actor)
            return false;
        actor.content_digest = lux::cxx::algorithm::Sha256::hash(*encoded_actor);
        actor.document_path =
            lux::authoring::makeWorldActorDocumentPath(
                actor.id, actor.content_digest);
        const auto actor_relative =
            std::filesystem::path{actor.document_path};
        std::error_code error;
        std::filesystem::create_directories(
            (path.parent_path() / actor_relative).parent_path(), error);
        if (error)
            return false;
        std::ofstream stream(
            path.parent_path() / actor_relative,
            std::ios::binary | std::ios::trunc);
        stream.write(
            reinterpret_cast<const char*>(encoded_actor->data()),
            static_cast<std::streamsize>(encoded_actor->size()));
        stream.close();
        if (!stream)
            return false;
        lux::authoring::WorldDescriptorPageDocument descriptor_page;
        descriptor_page.world = source.world;
        descriptor_page.space = space.id;
        descriptor_page.macro = {
            EPartitionTopology::PLANAR_XZ,
            PlanarMacroCoord{0, 0}};
        descriptor_page.id = lux::authoring::makeWorldDescriptorPageId(
            source.world,
            descriptor_page.space,
            descriptor_page.macro);
        descriptor_page.actors.push_back(std::move(actor));

        const std::array mesh_ids{
            uuids::uuid::from_string(
                "560e8400-e29b-41d4-a716-446655440021").value(),
            uuids::uuid::from_string(
                "560e8400-e29b-41d4-a716-446655440022").value()};
        const auto material = uuids::uuid::from_string(
            "560e8400-e29b-41d4-a716-446655440023").value();
        for (std::size_t index = 0u; index < mesh_ids.size(); ++index)
        {
            const auto mesh = exportHlodMesh(
                static_cast<float>(index) * 0.25f);
            const auto mesh_image = lux::asset::MeshSerDeser::encodeData(
                mesh_ids[index], mesh);
            if (!mesh_image)
                return false;
            const auto mesh_path = path.parent_path().parent_path() /
                "Content" /
                ("hlod-source-" + std::to_string(index) + ".luxasset");
            std::ofstream mesh_stream(
                mesh_path, std::ios::binary | std::ios::trunc);
            mesh_stream.write(
                reinterpret_cast<const char*>(mesh_image->data()),
                static_cast<std::streamsize>(mesh_image->size()));
            if (!mesh_stream)
                return false;

            lux::authoring::WorldInstancePageDocument page;
            page.world = source.world;
            page.instance_set = InstanceSetId{
                uuids::uuid::from_string(index == 0u
                    ? "560e8400-e29b-41d4-a716-446655440031"
                    : "560e8400-e29b-41d4-a716-446655440032").value()};
            source.instance_sets.push_back({page.instance_set, 2u});
            page.space = space.id;
            page.cell = {
                EPartitionTopology::PLANAR_XZ,
                PlanarCellCoord{static_cast<std::int64_t>(index), 0}};
            lux::authoring::EditableWorldInstance instance;
            instance.id = {page.instance_set, 1u};
            instance.position = lux::spatial::Position3D{
                static_cast<double>(index) * space.cell_edge + 4.0,
                0.0,
                4.0};
            instance.mesh = mesh_ids[index];
            instance.material_instance = material;
            page.instances.push_back(std::move(instance));
            const auto encoded_page =
                lux::authoring::encodeWorldInstancePage(source, page);
            if (!encoded_page)
                return false;
            const auto page_digest = lux::cxx::algorithm::Sha256::hash(*encoded_page);
            const auto page_path =
                lux::authoring::makeWorldInstancePagePath(
                    page.instance_set, page.cell, page_digest);
            if (!lux::authoring::saveWorldSourceDocument(
                    path, page_path, *encoded_page))
            {
                return false;
            }
            descriptor_page.pages.push_back({
                uuids::uuid::from_string(index == 0u
                    ? "560e8400-e29b-41d4-a716-446655440041"
                    : "560e8400-e29b-41d4-a716-446655440042").value(),
                lux::authoring::EWorldPageSourceKind::INSTANCE,
                lux::authoring::WorldPageSourceOwner{page.instance_set},
                page_path,
                page.space,
                page.cell,
                page_digest});
        }
        auto descriptor_bytes = lux::authoring::encodeWorldDescriptorPage(
            source,
            descriptor_page);
        if (!descriptor_bytes)
            return false;
        const auto descriptor_digest = lux::cxx::algorithm::Sha256::hash(*descriptor_bytes);
        const auto descriptor_path =
            lux::authoring::makeWorldDescriptorPagePath(
                descriptor_page.id,
                descriptor_digest);
        if (!lux::authoring::saveWorldSourceDocument(
                path,
                descriptor_path,
                *descriptor_bytes))
            return false;
        source.descriptor_pages.push_back({
            descriptor_page.id,
            descriptor_page.space,
            descriptor_page.macro,
            descriptor_path,
            descriptor_digest,
            1u,
            static_cast<std::uint32_t>(descriptor_page.pages.size())});
        return static_cast<bool>(
            lux::authoring::saveWorldSource(path, source));
    }
}

int main()
{
    std::error_code error;
    const auto nonce = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    TempTree tree{
        std::filesystem::temp_directory_path() /
        ("lux-game-export-smoke-" + std::to_string(nonce))
    };
    const auto project_root = tree.root / "project";
    const auto cooked_output = tree.root / "cooked";
    const auto output = tree.root / "output";
    std::filesystem::create_directories(project_root / "Content", error);
    std::filesystem::create_directories(project_root / "Worlds", error);
    if (error)
        return 1;

    const auto project_file = project_root / "ExporterSmoke.luxproject";
    if (!writeProject(project_file, true) ||
        !writeWorldSource(
            project_root / "Worlds" / "Main.luxworld"))
        return 2;

    std::filesystem::copy_file(
        std::filesystem::path{LUX_GAME_EXPORT_TEST_ASSET},
        project_root / "Content" / "test-shader.luxasset",
        std::filesystem::copy_options::none,
        error
    );
    if (error)
        return 3;

    const auto source_asset = project_root / "Content" / "test-shader.luxasset";
    const auto runtime_image_size = std::filesystem::file_size(source_asset, error);
    if (error)
        return 3;
    {
        std::ofstream stream(source_asset, std::ios::binary | std::ios::app);
        const lux::asset::PayloadBlockHeader authoring_block{
            0x545345544755584Cull,
            4u
        };
        constexpr std::byte payload[4]{
            std::byte{0x10}, std::byte{0x20},
            std::byte{0x30}, std::byte{0x40}
        };
        stream.write(
            reinterpret_cast<const char*>(&authoring_block),
            sizeof(authoring_block)
        );
        stream.write(
            reinterpret_cast<const char*>(payload),
            sizeof(payload)
        );
        if (!stream)
            return 3;
    }
    {
        std::ofstream stream(
            project_root / "Content" / "gameplay.lua",
            std::ios::binary
        );
        stream << "return { tick = function() return 1 end }\n";
        if (!stream)
            return 3;
    }
    {
        // 1x1 24-bit BMP (one red pixel + row padding). This exercises the
        // Toolchain texture source transform without relying on a fixture file.
        constexpr std::array<unsigned char, 58> bitmap{
            0x42, 0x4d, 0x3a, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x28, 0x00,
            0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
            0x00, 0x00, 0x01, 0x00, 0x18, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00};
        std::ofstream stream(
            project_root / "Content" / "probe.bmp",
            std::ios::binary
        );
        stream.write(
            reinterpret_cast<const char*>(bitmap.data()),
            static_cast<std::streamsize>(bitmap.size())
        );
        if (!stream)
            return 3;
    }

    // Stage 1 is intentionally target- and runtime-root-independent.
    auto cooked = lux::toolchain::cookGame(
        lux::toolchain::GameCookRequest{
            project_file,
            cooked_output
        }
    );
    if (!cooked ||
        !std::filesystem::is_regular_file(cooked->cook_receipt) ||
        !std::filesystem::is_regular_file(cooked->game_pak))
    {
        if (!cooked)
            std::cerr << "cook failed: " << cooked.error().detail << '\n';
        else
            std::cerr << "unexpected cooked asset count: " <<
                cooked->asset_count << '\n';
        return 4;
    }

    auto pak = lux::toolchain::inspectPak(cooked->game_pak);
    const auto shader_entry = pak
        ? std::find_if(
              pak->entries.begin(),
              pak->entries.end(),
              [runtime_image_size](const auto& entry)
              {
                  return entry.size == runtime_image_size;
              })
        : decltype(pak->entries.begin()){};
    const auto scene_count = pak
        ? std::ranges::count_if(
              pak->entries,
              [](const auto& entry)
              {
                  return entry.type == lux::asset::EAssetType::ENTITY_SCENE;
              })
        : 0u;
    const auto section_count = pak
        ? std::ranges::count_if(
              pak->entries,
              [](const auto& entry)
              {
                  return entry.type == lux::asset::EAssetType::ENTITY_SECTION;
              })
        : 0u;
    const auto mesh_count = pak
        ? std::ranges::count_if(
              pak->entries,
              [](const auto& entry)
              {
                  return entry.type == lux::asset::EAssetType::MESH;
              })
        : 0u;
    const auto scene_entry = pak
        ? std::find_if(
              pak->entries.begin(),
              pak->entries.end(),
              [](const auto& entry)
              {
                  return entry.type ==
                      lux::asset::EAssetType::ENTITY_SCENE;
              })
        : decltype(pak->entries.begin()){};
    const auto generated_hlod_entry = pak
        ? std::find_if(
              pak->entries.begin(),
              pak->entries.end(),
              [](const auto& entry)
              {
                  return entry.type == lux::asset::EAssetType::MESH &&
                      entry.vpath.starts_with("Generated/HlodMeshes/");
              })
        : decltype(pak->entries.begin()){};
    const auto runtime_codecs = lux::asset::runtimeAssetCodecCatalog();
    const auto payload_types_are_runtime = pak && std::ranges::all_of(
        pak->entries,
        [&runtime_codecs](const auto& entry)
        {
            if (entry.type == lux::asset::EAssetType::ENTITY_SCENE ||
                entry.type == lux::asset::EAssetType::ENTITY_SECTION)
            {
                return true;
            }
            const auto* codec = runtime_codecs->find(entry.type);
            return codec != nullptr &&
                codec->shipping ==
                    lux::asset::EAssetShippingClass::RUNTIME;
        });
    if (!pak || cooked->asset_count != pak->entries.size() ||
        section_count < 2u ||
        pak->entries.size() != section_count + 7u || scene_count != 1u ||
        mesh_count != 3u ||
        !payload_types_are_runtime || scene_entry == pak->entries.end() ||
        generated_hlod_entry == pak->entries.end() ||
        scene_entry->vpath != "Scenes/Main" ||
        shader_entry == pak->entries.end())
    {
        // Auxiliary authoring blocks must not enter the Player pak.
        return 5;
    }

    // LXWA is Authoring-only. The Player Pak contains one LXSC ScenePackage plus
    // its startup, fine-cell and coarse-LOD LXES images, each linked through
    // one absolute mounted source.
    auto provider = lux::asset::PakAssetProvider::loadFromFile(
        cooked->game_pak);
    if (!provider)
        return 5;
    const auto scene_image = (*provider)->open(scene_entry->id);
    if (!scene_image)
        return 5;
    const auto scene_package = lux::scene::decodeScenePackage(
        scene_image->bytes.view());
    if (!scene_package ||
        scene_package->sections.size() != section_count ||
        scene_package->startup_sections.size() != 1u)
    {
        return 5;
    }
    std::size_t published_entities = 0u;
    std::size_t published_attachments = 0u;
    bool startup_found = false;
    bool generated_hlod_referenced = false;
    for (const auto& record : scene_package->sections)
    {
        const auto section_entry = std::find_if(
            pak->entries.begin(),
            pak->entries.end(),
            [&record](const auto& entry)
            {
                return entry.type ==
                           lux::asset::EAssetType::ENTITY_SECTION &&
                    entry.id == record.id.value();
            });
        if (section_entry == pak->entries.end())
            return 5;
        const auto* stored = std::get_if<
            lux::scene::StoredSectionSource>(&record.source);
        if (stored == nullptr ||
            stored->content_path != "/Game/" + section_entry->vpath ||
            section_entry->vpath != "EntitySections/" +
                uuids::to_string(section_entry->id))
        {
            return 5;
        }
        const auto section_image = (*provider)->open(section_entry->id);
        if (!section_image ||
            lux::cxx::algorithm::Sha256::hash(section_image->bytes.view()) !=
                record.content_digest)
        {
            return 5;
        }
        const auto section =
            lux::ecs::scene_format::decodeEntitySectionImage(
            section_image->bytes.view());
        if (!section || section->section.value() != section_entry->id ||
            section->entities.size() != record.entity_count)
        {
            return 5;
        }
        published_entities += section->entities.size();
        published_attachments += section->attachments.size();
        for (const auto& attachment : section->attachments)
        {
            if (attachment.reference.type.name() !=
                lux::classic_mesh::kClassicMeshBatchContentTypeName)
            {
                continue;
            }
            const auto batch =
                lux::classic_mesh::decodeClassicMeshBatchBlob(
                    attachment.payload);
            if (!batch)
                return 5;
            generated_hlod_referenced = generated_hlod_referenced ||
                std::ranges::any_of(
                    batch->instances,
                    [&generated_hlod_entry](const auto& instance)
                    {
                        return instance.mesh_asset ==
                            generated_hlod_entry->id;
                    });
        }
        startup_found = startup_found ||
            record.id == scene_package->startup_sections.front();
    }
    if (!startup_found || !generated_hlod_referenced ||
        published_entities == 0u ||
        published_attachments == 0u)
    {
        return 5;
    }
    const auto generated_hlod_image =
        (*provider)->open(generated_hlod_entry->id);
    if (!generated_hlod_image)
        return 5;
    const auto generated_hlod_mesh = lux::asset::MeshSerDeser::decodeData(
        generated_hlod_image->bytes.data(),
        generated_hlod_image->bytes.size());
    if (!generated_hlod_mesh || !*generated_hlod_mesh ||
        (*generated_hlod_mesh)->vertices.empty() ||
        (*generated_hlod_mesh)->indices.empty())
    {
        return 5;
    }

    // The immutable Pak index must permit independent Section/asset reads.
    // This catches regressions to a shared seek cursor.
    std::atomic<bool> concurrent_reads_ok{true};
    std::vector<std::thread> readers;
    for (int worker = 0; worker < 8; ++worker)
    {
        readers.emplace_back(
            [provider = *provider, &pak, &concurrent_reads_ok]() noexcept
            {
                for (int iteration = 0; iteration < 8; ++iteration)
                {
                    for (const auto& entry : pak->entries)
                    {
                        if (!provider->open(entry.id))
                            concurrent_reads_ok.store(
                                false,
                                std::memory_order_release);
                    }
                }
            });
    }
    for (auto& reader : readers)
        reader.join();
    if (!concurrent_reads_ok.load(std::memory_order_acquire))
        return 5;

    // Unsupported authored input must not disappear behind a successful cook.
    const auto unsupported_project_root = tree.root / "unsupported-project";
    std::filesystem::create_directories(
        unsupported_project_root / "Content",
        error
    );
    std::filesystem::create_directories(
        unsupported_project_root / "Worlds",
        error
    );
    const auto unsupported_project =
        unsupported_project_root / "ExporterSmoke.luxproject";
    if (error || !writeProject(unsupported_project))
        return 5;
    {
        std::ofstream stream(
            unsupported_project_root / "Content" / "raw-model.obj",
            std::ios::binary
        );
        stream << "o NotSilentlyDropped\n";
    }
    auto unsupported = lux::toolchain::cookGame(
        lux::toolchain::GameCookRequest{
            unsupported_project,
            tree.root / "unsupported-cooked"}
    );
    if (unsupported || unsupported.error().code !=
            lux::toolchain::EGameExportError::COOK_FAILED)
    {
        return 5;
    }

    const auto baked_project_root = tree.root / "baked-only-project";
    std::filesystem::create_directories(
        baked_project_root / "Content",
        error
    );
    std::filesystem::create_directories(
        baked_project_root / "Worlds",
        error
    );
    const auto baked_project =
        baked_project_root / "ExporterSmoke.luxproject";
    if (error || !writeProject(baked_project))
        return 5;
    std::filesystem::copy_file(
        source_asset,
        baked_project_root / "Content" / "baked.luxasset",
        std::filesystem::copy_options::none,
        error
    );
    auto baked_only = lux::toolchain::cookGame(
        lux::toolchain::GameCookRequest{
            baked_project,
            tree.root / "baked-only-cooked"}
    );
    if (error || !baked_only || baked_only->asset_count != 1u)
        return 5;

    // Target and binary mode are explicit. Unsupported adapters fail before
    // writing a misleading package.
    auto unspecified = lux::toolchain::assembleGame(
        lux::toolchain::GameAssemblyRequest{
            cooked_output,
            std::filesystem::path{LUX_GAME_EXPORT_TEST_RUNTIME_ROOT},
            tree.root / "unspecified",
            lux::toolchain::ETargetPlatform::UNSPECIFIED,
            "RelWithDebInfo",
            {}}
    );
    if (unspecified ||
        unspecified.error().code !=
            lux::toolchain::EGameExportError::INVALID_ARGUMENT)
    {
        return 6;
    }

    lux::toolchain::GameBinaryRecipe native_recipe;
    native_recipe.mode = lux::toolchain::EGameBinaryMode::NATIVE_PROJECT;
    native_recipe.native_project_file = project_file;
    auto native = lux::toolchain::assembleGame(
        lux::toolchain::GameAssemblyRequest{
            cooked_output,
            std::filesystem::path{LUX_GAME_EXPORT_TEST_RUNTIME_ROOT},
            tree.root / "native",
            lux::toolchain::ETargetPlatform::WINDOWS,
            "RelWithDebInfo",
            native_recipe}
    );
    if (native ||
        native.error().code !=
            lux::toolchain::EGameExportError::BINARY_MODE_UNSUPPORTED)
    {
        return 7;
    }

    auto android = lux::toolchain::assembleGame(
        lux::toolchain::GameAssemblyRequest{
            cooked_output,
            std::filesystem::path{LUX_GAME_EXPORT_TEST_RUNTIME_ROOT},
            tree.root / "android",
            lux::toolchain::ETargetPlatform::ANDROID,
            "RelWithDebInfo",
            {}}
    );
    if (android ||
        android.error().code !=
            lux::toolchain::EGameExportError::TARGET_PLATFORM_UNSUPPORTED)
    {
        return 8;
    }

    // Stage 2 consumes only the cooked receipt/pak plus a target runtime.
    auto assembled = lux::toolchain::assembleGame(
        lux::toolchain::GameAssemblyRequest{
            cooked_output,
            std::filesystem::path{LUX_GAME_EXPORT_TEST_RUNTIME_ROOT},
            output,
            lux::toolchain::ETargetPlatform::WINDOWS,
            "RelWithDebInfo",
            {}}
    );
    if (!assembled)
        return 9;
    if (!std::filesystem::is_regular_file(assembled->executable) ||
        !std::filesystem::is_regular_file(assembled->runtime_manifest) ||
        !std::filesystem::is_regular_file(assembled->game_pak))
    {
        return 10;
    }
#if defined(LUX_GAME_EXPORT_TEST_WITH_EXTENSION)
    if (assembled->extension_count != 1u ||
        !std::filesystem::is_regular_file(
            output / "extensions" / "org.lux.physics2d.runtime.dll"))
    {
        return 11;
    }
#else
    if (assembled->extension_count != 0u)
        return 11;
#endif

    auto launch_manifest = lux::game::LaunchManifest::loadFromFile(
        assembled->runtime_manifest);
    if (!launch_manifest ||
        launch_manifest->game_pak != "ExporterSmoke.luxpak" ||
        launch_manifest->title != "Exporter Smoke" ||
        launch_manifest->boot_scene != "Scenes/Main")
    {
        return 12;
    }
    return 0;
}
