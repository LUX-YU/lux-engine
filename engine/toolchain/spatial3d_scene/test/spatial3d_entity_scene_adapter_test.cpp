#include <lux/engine/toolchain/spatial3d_scene/Spatial3DEntitySceneAdapter.hpp>

#include <lux/engine/core/serialization/Archive.hpp>
#include <lux/engine/core/serialization/NameTable.hpp>
#include <lux/engine/core/serialization/TaggedPropertyArchive.hpp>
#include <lux/engine/ecs/components/Transform3DComponent.hpp>
#include <lux/engine/ecs/render/components/PrimaryCameraTag.hpp>
#include <lux/engine/ecs/render/components/3d/Camera3DComponent.hpp>
#include <lux/engine/ecs/render/components/3d/ClassicMeshBatchComponent.hpp>
#include <lux/engine/ecs/render/components/3d/DirectionalLightComponent.hpp>
#include <lux/engine/ecs/render/components/3d/HeightFogComponent.hpp>
#include <lux/engine/ecs/render/components/3d/PointLightComponent.hpp>
#include <lux/engine/ecs/render/components/3d/SkyboxComponent.hpp>
#include <lux/engine/ecs/render/components/3d/VisualLodNodeComponent.hpp>
#include <lux/engine/ecs/navigation/components/NavigationRegion3DComponent.hpp>
#include <lux/engine/ecs/physics3d/components/Physics3DComponents.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/ecs/spatial3d/components/SpatialInterest3DComponent.hpp>
#include <lux/engine/ecs/terrain/components/TerrainTileComponent.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/meta/LuxObject.hpp>
#include <lux/engine/function/render/standard/content/ClassicMeshBatch.hpp>
#include <lux/engine/resource/asset/MeshSerDeser.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/resource/asset/codecs/StaticColliderBatch3DCodec.hpp>
#include <lux/engine/spatial3d/SceneCatalog.hpp>
#include <lux/engine/resource/asset/codecs/TerrainTileCodec.hpp>
#include <lux/engine/navigation/detour3d/NavigationDetour3D.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace
{
    [[nodiscard]] uuids::uuid uuid(const char* value)
    {
        return uuids::uuid::from_string(value).value();
    }

    [[nodiscard]] lux::rdesc::Mesh visualHlodSourceMesh()
    {
        constexpr std::uint32_t edge = 8u;
        lux::rdesc::Mesh mesh;
        mesh.vertices.resize((edge + 1u) * (edge + 1u));
        for (std::uint32_t z = 0u; z <= edge; ++z)
        for (std::uint32_t x = 0u; x <= edge; ++x)
        {
            auto& vertex = mesh.vertices[z * (edge + 1u) + x];
            vertex.position = {
                static_cast<float>(x), 0.0f, static_cast<float>(z)};
            vertex.normal = Eigen::Vector3f::UnitY();
            vertex.tangent = Eigen::Vector3f::UnitX();
            vertex.bitangent = Eigen::Vector3f::UnitZ();
            vertex.uv = {
                static_cast<float>(x) / static_cast<float>(edge),
                static_cast<float>(z) / static_cast<float>(edge)};
            vertex.bone = {};
        }
        for (std::uint32_t z = 0u; z < edge; ++z)
        for (std::uint32_t x = 0u; x < edge; ++x)
        {
            const auto i0 = z * (edge + 1u) + x;
            const auto i1 = i0 + 1u;
            const auto i2 = i0 + edge + 1u;
            const auto i3 = i2 + 1u;
            mesh.indices.insert(
                mesh.indices.end(), {i0, i2, i1, i1, i2, i3});
        }
        mesh.bounds = lux::math::AABB{
            {0.0f, 0.0f, 0.0f},
            {static_cast<float>(edge), 0.0f, static_cast<float>(edge)}};
        return mesh;
    }

    [[nodiscard]] lux::toolchain::Spatial3DMeshAssetCatalog
    meshAssetFixture()
    {
        const auto id = uuid("15000000-0000-4000-8000-000000000001");
        const auto encoded = lux::asset::MeshSerDeser::encodeData(
            id, visualHlodSourceMesh());
        assert(encoded);
        lux::toolchain::Spatial3DMeshAssetCatalog result;
        result.meshes.push_back({id, *encoded});
        return result;
    }

    class TaggedFields final
    {
    public:
        TaggedFields(
            std::vector<std::byte>& payload,
            lux::serialize::NameTable& names) noexcept
            : writer_(payload), names_(names)
        {}

        void floating(std::string_view name, float value)
        {
            const auto index = names_.intern(name);
            writer_.writePod(index);
            writer_.writePod(static_cast<std::uint8_t>(
                lux::serialize::EArchiveType::Float));
            writer_.writePod<std::uint32_t>(sizeof(value));
            writer_.writePod(value);
        }

        void finish()
        {
            writer_.writePod(lux::serialize::kEndOfObject);
        }

    private:
        lux::serialize::ArchiveWriter writer_;
        lux::serialize::NameTable& names_;
    };

    void appendComponent(
        std::vector<lux::toolchain::Spatial3DActorComponentSource>& components,
        lux::serialize::NameTable& names,
        std::string schema,
        bool abbreviated_point_light = false)
    {
        lux::toolchain::Spatial3DActorComponentSource record;
        record.schema_name = std::move(schema);
        record.schema_version = 1u;
        TaggedFields fields{record.tagged_payload, names};
        if (abbreviated_point_light)
            fields.floating("intensity", 9.0f);
        fields.finish();
        components.push_back(std::move(record));
    }

    [[nodiscard]] lux::toolchain::Spatial3DAuthoringSource sourceFixture()
    {
        lux::toolchain::Spatial3DAuthoringSource source;
        source.scene = lux::scene::ScenePackageId{
            uuid("10000000-0000-4000-8000-000000000001")};
        const auto space =
            uuid("12000000-0000-4000-8000-000000000001");
        source.spaces.push_back({
            space,
            lux::toolchain::ESpatial3DSourceTopology::PLANAR_XZ,
            256.0});

        lux::toolchain::Spatial3DActorSource actor;
        actor.id = lux::ecs::PersistentEntityId{
            uuid("13000000-0000-4000-8000-000000000001")};
        actor.space = space;
        actor.position = {10'000.25, 64.5, -20'000.75};
        lux::serialize::NameTable actor_names;
        appendComponent(
            actor.components,
            actor_names,
            "lux::ecs::Transform3DComponent");
        // Deliberately sparse legacy payload.  The adapter must apply current
        // defaults and emit the complete exact PointLight schema.
        appendComponent(
            actor.components,
            actor_names,
            "lux::ecs::PointLightComponent",
            true);
        // Selection owns the active camera's spatial-interest settings. An
        // authored/default component must be replaced, not appended as a
        // duplicate schema in the same EntityCookInput.
        appendComponent(
            actor.components,
            actor_names,
            "lux::ecs::SpatialInterest3DComponent");
        appendComponent(
            actor.components,
            actor_names,
            "lux::ecs::PrimaryCameraTag");
        appendComponent(
            actor.components,
            actor_names,
            "lux::ecs::SkyboxComponent");
        appendComponent(
            actor.components,
            actor_names,
            "lux::ecs::DirectionalLightComponent");
        appendComponent(
            actor.components,
            actor_names,
            "lux::ecs::HeightFogComponent");
        {
            lux::serialize::ArchiveWriter writer{
                actor.name_table};
            actor_names.serialize(writer);
        }
        source.actors.push_back(std::move(actor));
        auto spatial_actor = source.actors.front();
        spatial_actor.id = lux::ecs::PersistentEntityId{
            uuid("13000000-0000-4000-8000-000000000002")};
        spatial_actor.position = {-16.0, 8.0, 32.0};
        const auto primary_schema = lux::ecs::defaultComponentSchemaName(
            lux::ecs::typeToken<lux::ecs::PrimaryCameraTag>().name);
        std::erase_if(
            spatial_actor.components,
            [&primary_schema](const auto& component)
            {
                return lux::ecs::defaultComponentSchemaName(
                           component.schema_name) == primary_schema;
            });
        source.actors.push_back(std::move(spatial_actor));

        lux::toolchain::Spatial3DInstancePageSource instance_page;
        instance_page.space = space;
        instance_page.cell = {0, 0, 0};
        lux::toolchain::Spatial3DInstanceSource instance;
        instance.id = lux::ecs::PersistentEntityId{
            uuid("14000000-0000-4000-8000-000000000001")};
        instance.position = {32.0, 4.0, 48.0};
        instance.mesh =
            uuid("15000000-0000-4000-8000-000000000001");
        instance.material_instance =
            uuid("16000000-0000-4000-8000-000000000001");
        instance.stable_pick_id = 77u;
        instance_page.instances.push_back(std::move(instance));
        source.instance_pages.push_back(std::move(instance_page));
        auto second_instance_page = source.instance_pages.front();
        second_instance_page.cell = {1, 0, 0};
        second_instance_page.instances.front().id =
            lux::ecs::PersistentEntityId{
                uuid("14000000-0000-4000-8000-000000000002")};
        second_instance_page.instances.front().position =
            {288.0, 4.0, 48.0};
        second_instance_page.instances.front().stable_pick_id = 78u;
        source.instance_pages.push_back(std::move(second_instance_page));
        auto negative_instance_page = source.instance_pages.front();
        negative_instance_page.cell = {-1, 0, 0};
        negative_instance_page.instances.front().id =
            lux::ecs::PersistentEntityId{
                uuid("14000000-0000-4000-8000-000000000003")};
        negative_instance_page.instances.front().position =
            {-32.0, 4.0, 48.0};
        negative_instance_page.instances.front().stable_pick_id = 79u;
        source.instance_pages.push_back(std::move(negative_instance_page));

        lux::toolchain::Spatial3DTerrainPageSource terrain;
        terrain.terrain_set =
            uuid("17000000-0000-4000-8000-000000000001");
        terrain.space = space;
        terrain.cell = {0, 0, 0};
        terrain.height_min = 0.0f;
        terrain.height_max = 16.0f;
        terrain.sample_spacing = 1.0f;
        terrain.weight_layer_count = 1u;
        terrain.heights.assign(
            lux::terrain::kTerrainTileSampleCount, 4.0f);
        terrain.weight_planes[0].assign(
            lux::terrain::kTerrainTileWeightPlaneBytes, 0u);
        terrain.weight_planes[1].assign(
            lux::terrain::kTerrainTileWeightPlaneBytes, 0u);
        terrain.holes.assign(lux::terrain::kTerrainTileHoleBytes, 0u);
        source.terrain_pages.push_back(std::move(terrain));
        return source;
    }

    template <class Component>
    [[nodiscard]] std::string schemaName()
    {
        return lux::ecs::defaultComponentSchemaName(
            lux::ecs::typeToken<Component>().name);
    }

    void stage(
        lux::navigation::detour3d::NavigationRegion3DLease& lease)
    {
        using namespace lux::navigation::detour3d;
        while (lease.state() == ENavigationRegion3DLeaseState::STAGING)
        {
            const auto step = lease.advancePreparationOne();
            assert(step && step->work_items == 1u);
        }
        assert(lease.state() == ENavigationRegion3DLeaseState::READY);
    }

    void retire(
        lux::navigation::detour3d::NavigationRegion3DLease& lease)
    {
        using namespace lux::navigation::detour3d;
        if (lease.state() != ENavigationRegion3DLeaseState::RETIRING)
            assert(lease.beginRetirement());
        while (lease.state() != ENavigationRegion3DLeaseState::RETIRED)
        {
            const auto step = lease.advanceRetirementOne();
            assert(step && step->work_items == 1u);
        }
    }
}

int main()
{
    lux::meta::meta_module_init();
    lux::ecs::ComponentTypeCatalog components;
    const auto registered = lux::ecs::registerGeneratedComponents(components);
    assert(registered && *registered != 0u);

    const auto source = sourceFixture();
    const auto mesh_assets = meshAssetFixture();
    const auto first = lux::toolchain::adaptSpatial3DEntityScene(
        source, components, mesh_assets);
    const auto second = lux::toolchain::adaptSpatial3DEntityScene(
        source, components, mesh_assets);
    assert(first && second);
    assert(first->encoded_package == second->encoded_package);
    assert(first->generated_meshes.size() == 1u);
    assert(second->generated_meshes.size() == 1u);
    assert(first->generated_meshes.front().id ==
        second->generated_meshes.front().id);
    assert(first->generated_meshes.front().encoded_image ==
        second->generated_meshes.front().encoded_image);
    assert(first->generated_meshes.front().source_instance_count == 2u);
    assert(first->generated_meshes.front().output_vertex_count <
        first->generated_meshes.front().source_vertex_count);
    assert(first->generated_meshes.front().output_triangle_count <
        first->generated_meshes.front().source_triangle_count);

    auto changed_mesh_assets = mesh_assets;
    auto changed_source_mesh = visualHlodSourceMesh();
    changed_source_mesh.vertices.front().position.y() = 0.25f;
    const auto changed_source_image = lux::asset::MeshSerDeser::encodeData(
        changed_mesh_assets.meshes.front().id, changed_source_mesh);
    assert(changed_source_image);
    changed_mesh_assets.meshes.front().encoded_image = *changed_source_image;
    const auto changed = lux::toolchain::adaptSpatial3DEntityScene(
        source, components, changed_mesh_assets);
    assert(changed && changed->generated_meshes.size() == 1u);
    assert(changed->generated_meshes.front().id !=
        first->generated_meshes.front().id);

    auto mismatched_mesh_assets = mesh_assets;
    const auto mismatched_source_image = lux::asset::MeshSerDeser::encodeData(
        uuid("15000000-0000-4000-8000-000000000099"),
        visualHlodSourceMesh());
    assert(mismatched_source_image);
    mismatched_mesh_assets.meshes.front().encoded_image =
        *mismatched_source_image;
    const auto mismatched = lux::toolchain::adaptSpatial3DEntityScene(
        source, components, mismatched_mesh_assets);
    assert(!mismatched);
    assert(mismatched.error().code ==
        lux::toolchain::ESpatial3DEntitySceneAdapterError::
            CLASSIC_MESH_CONTENT_REJECTED);

    const auto generated_mesh = lux::asset::MeshSerDeser::decodeData(
        first->generated_meshes.front().encoded_image.data(),
        first->generated_meshes.front().encoded_image.size());
    assert(generated_mesh && *generated_mesh);
    assert((*generated_mesh)->vertices.size() ==
        first->generated_meshes.front().output_vertex_count);
    assert((*generated_mesh)->indices.size() / 3u ==
        first->generated_meshes.front().output_triangle_count);
    assert(first->sections.size() == 5u);
    assert(second->sections.size() == first->sections.size());
    for (std::size_t index = 0u; index < first->sections.size(); ++index)
    {
        assert(first->sections[index].encoded_image ==
            second->sections[index].encoded_image);
    }
    assert(first->package.id.value() == source.scene.value());
    assert(first->package.startup_sections.size() == 1u);
    assert(first->package.features.size() == 5u);
    for (const auto name : {
             "org.lux.builtin.animation3d",
             "org.lux.builtin.presentation3d",
             "org.lux.builtin.physics3d",
             "org.lux.builtin.navigation3d"})
    {
        assert(std::ranges::find(
                   first->package.features,
                   name,
                   [](const auto& feature)
                   {
                       return feature.id.name();
                   }) != first->package.features.end());
    }
    const auto spatial3d = std::ranges::find(
        first->package.features,
        lux::spatial3d::kPartitionedFeatureName,
        [](const auto& feature)
        {
            return feature.id.name();
        });
    assert(spatial3d != first->package.features.end());
    assert(spatial3d->config_schema_version ==
        lux::spatial3d::kSceneCatalogSchemaVersion);
    const auto catalog =
        lux::spatial3d::decodeSceneCatalog(
            spatial3d->config);
    assert(catalog);
    assert(catalog->bands.size() == 2u);
    assert(catalog->entries.size() == 4u);
    assert(std::ranges::any_of(
        catalog->entries,
        [](const auto& entry)
        {
            return entry.coordinate ==
                lux::spatial::GridCoord3i64{-1, 0, 0};
        }));

    const auto startup = std::ranges::find(
        first->sections,
        first->package.startup_sections.front(),
        [](const auto& section)
        {
            return section.record.id;
        });
    assert(startup != first->sections.end());
    assert(startup->record.demand_channels.empty());
    assert(startup->image.attachments.empty());
    assert(std::ranges::none_of(
        startup->image.schemas,
        [](const auto& schema)
        {
            return schema.id.name ==
                schemaName<lux::ecs::ClassicMeshBatchComponent>() ||
                schema.id.name ==
                    schemaName<lux::ecs::TerrainTileComponent>() ||
                schema.id.name ==
                    schemaName<lux::ecs::StaticColliderBatch3DComponent>();
        }));

    std::set<std::string, std::less<>> catalog_sections;
    for (const auto& entry : catalog->entries)
    {
        assert(entry.band < catalog->bands.size());
        catalog_sections.insert(uuids::to_string(entry.section.value()));
        const auto record = std::ranges::find_if(
            first->package.sections,
            [&entry](const lux::scene::SectionRecord& value)
            {
                return value.id.value() == entry.section.value();
            });
        assert(record != first->package.sections.end());
        assert(record->demand_channels.size() == 1u);
        assert(record->demand_channels.front().name() ==
            catalog->bands[entry.band].demand_channel.name());
        assert(catalog->bands[entry.band].source.name() ==
            "lux.spatial3d.source.12000000000040008000000000000001");
    }
    assert(catalog_sections.size() == first->sections.size() - 1u);
    assert(std::ranges::any_of(
        catalog->bands,
        [](const auto& band)
        {
            return band.level == 0u && band.cell_world_size == 256.0 &&
                band.active_distance_scale == 1.0 &&
                band.resident_distance_scale == 1.0;
        }));
    assert(std::ranges::any_of(
        catalog->bands,
        [](const auto& band)
        {
            return band.level == 1u && band.cell_world_size == 1024.0 &&
                band.active_distance_scale == 4.0 &&
                band.resident_distance_scale == 4.0;
        }));

    std::set<std::string, std::less<>> schemas;
    std::size_t blob_relocations = 0u;
    std::size_t persistent_relocations = 0u;
    bool saw_exact_actor_defaults = false;
    bool saw_sky_rotation = false;
    bool saw_classic = false;
    bool saw_terrain = false;
    bool saw_physics = false;
    bool saw_navigation = false;
    bool saw_generated_hlod_reference = false;
    std::size_t classic_attachment_count = 0u;
    for (const auto& section : first->sections)
    {
        assert(section.record.source ==
            lux::scene::SectionSource{
                lux::scene::StoredSectionSource{
                    "/Game/EntitySections/" + uuids::to_string(
                        section.record.id.value())}});
        assert(std::ranges::is_sorted(section.image.component_names));
        blob_relocations += section.image.blob_relocations.size();
        persistent_relocations +=
            section.image.persistent_reference_relocations.size();
        for (const auto& schema : section.image.schemas)
            schemas.emplace(schema.id.name);
        saw_exact_actor_defaults = saw_exact_actor_defaults ||
            (std::ranges::find(
                 section.image.component_names,
                 "attenuation_constant") !=
             section.image.component_names.end() &&
             std::ranges::find(section.image.component_names, "range") !=
                 section.image.component_names.end() &&
             std::ranges::find(
                 section.image.component_names, "shadow_bias") !=
                 section.image.component_names.end());
        saw_sky_rotation = saw_sky_rotation ||
            std::ranges::find(
                section.image.component_names, "rotation_radians") !=
                section.image.component_names.end();
        if (section.record.id !=
            first->package.startup_sections.front())
        {
            assert(catalog_sections.contains(
                uuids::to_string(section.record.id.value())));
            assert(section.record.demand_channels.size() == 1u);
        }
        for (const auto& attachment : section.image.attachments)
        {
            assert(attachment.reference.id ==
                lux::ecs::scene_format::makeContentBlobId(
                    attachment.reference.type,
                    attachment.reference.schema_version,
                    attachment.payload));
            if (attachment.reference.type.name() ==
                lux::classic_mesh::kClassicMeshBatchContentTypeName)
            {
                assert(attachment.reference.schema_version ==
                    lux::classic_mesh::kClassicMeshBatchSchemaVersion);
                const auto decoded =
                    lux::classic_mesh::decodeClassicMeshBatchBlob(
                        attachment.payload);
                assert(decoded);
                if (std::ranges::any_of(
                        decoded->instances,
                        [&first](const auto& instance)
                        {
                            return instance.mesh_asset ==
                                first->generated_meshes.front().id;
                        }))
                {
                    assert(decoded->instances.size() == 1u);
                    saw_generated_hlod_reference = true;
                }
                ++classic_attachment_count;
                saw_classic = true;
            }
            else if (attachment.reference.type.name() ==
                     lux::terrain::kTerrainTileContentTypeName)
            {
                assert(attachment.reference.schema_version ==
                    lux::terrain::kTerrainTileSchemaVersion);
                const auto decoded = lux::terrain::decodeTerrainTileBlob(
                    attachment.payload);
                assert(decoded && decoded->heights.size() ==
                    lux::terrain::kTerrainTileSampleCount);
                saw_terrain = true;
            }
            else if (attachment.reference.type.name() ==
                     lux::navigation::detour3d::
                         kNavigationRegion3DContentTypeName)
            {
                assert(attachment.reference.schema_version ==
                    lux::navigation::detour3d::
                        kNavigationRegion3DSchemaVersion);
                auto blob = lux::navigation::detour3d::
                    navigationRegion3DBlobFromBytes(
                        lux::cxx::SharedBytes<>::copyOf(
                            attachment.payload));
                assert(blob);
                auto prepared = lux::navigation::detour3d::
                    prepareNavigationRegion3D(std::move(*blob), 1u);
                assert(prepared && prepared->valid());
                saw_navigation = true;
            }
            else if (attachment.reference.type.name() ==
                     lux::physics3d::
                         kStaticColliderBatch3DContentTypeName)
            {
                assert(attachment.reference.schema_version ==
                    lux::physics3d::
                        kStaticColliderBatch3DSchemaVersion);
                const auto decoded = lux::physics3d::
                    decodeStaticColliderBatch3DBlob(attachment.payload);
                assert(decoded && decoded->heightfields.size() == 1u);
                assert(decoded->heightfields.front().samples.size() ==
                    lux::terrain::kTerrainTileSampleCount);
                saw_physics = true;
            }
        }
    }
    assert(saw_exact_actor_defaults && saw_sky_rotation);
    assert(blob_relocations == 7u);
    assert(persistent_relocations == 2u);
    // The source PointLight carried only `intensity`; these names prove the
    // catalog-backed default materialization was re-emitted as an exact
    // current schema rather than merely copying the abbreviated old bytes.
    assert(schemas.contains(schemaName<lux::ecs::Transform3DComponent>()));
    assert(schemas.contains(schemaName<lux::ecs::Camera3DComponent>()));
    assert(schemas.contains(schemaName<lux::ecs::PrimaryCameraTag>()));
    assert(schemas.contains(
        schemaName<lux::ecs::SpatialInterest3DComponent>()));
    assert(schemas.contains(schemaName<lux::ecs::PointLightComponent>()));
    assert(schemas.contains(schemaName<lux::ecs::SkyboxComponent>()));
    assert(schemas.contains(
        schemaName<lux::ecs::DirectionalLightComponent>()));
    assert(schemas.contains(schemaName<lux::ecs::HeightFogComponent>()));
    assert(schemas.contains(
        schemaName<lux::ecs::ClassicMeshBatchComponent>()));
    assert(schemas.contains(
        schemaName<lux::ecs::VisualLodNodeComponent>()));
    assert(schemas.contains(
        schemaName<lux::ecs::VisualLodParentComponent>()));
    assert(schemas.contains(schemaName<lux::ecs::TerrainTileComponent>()));
    assert(schemas.contains(
        schemaName<lux::ecs::StaticColliderBatch3DComponent>()));
    assert(schemas.contains(
        schemaName<lux::ecs::NavigationRegion3DComponent>()));

    assert(saw_classic && saw_terrain && saw_physics && saw_navigation &&
        saw_generated_hlod_reference);
    assert(classic_attachment_count == 4u);

    lux::toolchain::Spatial3DEntitySceneAdapterConfig custom_config;
    custom_config.section_content_prefix = "/Game/TestEntitySections/";
    const auto custom = lux::toolchain::adaptSpatial3DEntityScene(
        source, components, mesh_assets, custom_config);
    assert(custom);
    for (const auto& section : custom->sections)
    {
        assert(section.record.source ==
            lux::scene::SectionSource{
                lux::scene::StoredSectionSource{
                    "/Game/TestEntitySections/" + uuids::to_string(
                        section.record.id.value())}});
    }

    custom_config.section_content_prefix = "Game/EntitySections";
    const auto invalid_prefix =
        lux::toolchain::adaptSpatial3DEntityScene(
            source, components, mesh_assets, custom_config);
    assert(!invalid_prefix);
    assert(invalid_prefix.error().code ==
        lux::toolchain::ESpatial3DEntitySceneAdapterError::INVALID_ARGUMENT);

    const auto missing_mesh = lux::toolchain::adaptSpatial3DEntityScene(
        source,
        components,
        lux::toolchain::Spatial3DMeshAssetCatalog{});
    assert(!missing_mesh);
    assert(missing_mesh.error().code ==
        lux::toolchain::ESpatial3DEntitySceneAdapterError::
            CLASSIC_MESH_CONTENT_REJECTED);

    lux::toolchain::Spatial3DEntitySceneAdapterConfig bounded_config;
    bounded_config.visual_lod_max_merged_vertices = 4u;
    bounded_config.visual_lod_max_merged_indices = 6u;
    const auto over_budget = lux::toolchain::adaptSpatial3DEntityScene(
        source, components, mesh_assets, bounded_config);
    assert(!over_budget);
    assert(over_budget.error().code ==
        lux::toolchain::ESpatial3DEntitySceneAdapterError::
            CLASSIC_MESH_CONTENT_REJECTED);

    // Adjacent authored Terrain Cells produce matching semantic portals in
    // their independent LXNR attachments.  This exercises the actual cooker
    // output rather than a hand-authored backend fixture.
    auto portal_source = sourceFixture();
    portal_source.actors.clear();
    portal_source.instance_pages.clear();
    std::ranges::fill(
        portal_source.terrain_pages.front().heights, 4.0f);
    auto adjacent_terrain = portal_source.terrain_pages.front();
    adjacent_terrain.cell = {1, 0, 0};
    portal_source.terrain_pages.push_back(std::move(adjacent_terrain));
    const auto portal_bundle = lux::toolchain::adaptSpatial3DEntityScene(
        portal_source, components, mesh_assets);
    assert(portal_bundle);
    const auto portal_feature = std::ranges::find(
        portal_bundle->package.features,
        lux::spatial3d::kPartitionedFeatureName,
        [](const auto& feature)
        {
            return feature.id.name();
        });
    assert(portal_feature !=
           portal_bundle->package.features.end());
    const auto portal_catalog =
        lux::spatial3d::decodeSceneCatalog(
            portal_feature->config);
    assert(portal_catalog);
    struct CookedNavigationRegion final
    {
        lux::spatial::GridCoord3i64 coordinate;
        lux::navigation::NavigationRegionId id;
        lux::navigation::detour3d::NavigationRegion3DBlob blob;
    };
    std::vector<CookedNavigationRegion> cooked_regions;
    for (const auto& section : portal_bundle->sections)
    {
        const auto catalog_entry = std::ranges::find_if(
            portal_catalog->entries,
            [&section](const auto& entry)
            {
                return entry.section.value() == section.record.id.value();
            });
        if (catalog_entry == portal_catalog->entries.end())
            continue;
        for (const auto& attachment : section.image.attachments)
        {
            if (attachment.reference.type.name() !=
                lux::navigation::detour3d::
                    kNavigationRegion3DContentTypeName)
            {
                continue;
            }
            auto blob = lux::navigation::detour3d::
                navigationRegion3DBlobFromBytes(
                    lux::cxx::SharedBytes<>::copyOf(attachment.payload));
            assert(blob);
            const auto id = blob->region;
            cooked_regions.push_back({
                catalog_entry->coordinate, id, std::move(*blob)});
        }
    }
    std::ranges::sort(
        cooked_regions,
        {},
        [](const auto& value)
        {
            return value.coordinate.x;
        });
    assert(cooked_regions.size() == 2u);
    assert((cooked_regions[0].coordinate ==
            lux::spatial::GridCoord3i64{0, 0, 0}));
    assert((cooked_regions[1].coordinate ==
            lux::spatial::GridCoord3i64{1, 0, 0}));

    auto backend_result =
        lux::navigation::detour3d::Navigation3DBackend::create();
    assert(backend_result);
    auto backend = std::move(*backend_result);
    std::vector<std::unique_ptr<
        lux::navigation::detour3d::NavigationRegion3DLease>> leases;
    for (std::size_t index = 0u; index < cooked_regions.size(); ++index)
    {
        auto prepared = lux::navigation::detour3d::
            prepareNavigationRegion3D(
                std::move(cooked_regions[index].blob), index + 1u);
        assert(prepared);
        auto adopted = backend->adoptPrepared(std::move(*prepared));
        assert(adopted);
        leases.push_back(std::move(*adopted));
        stage(*leases.back());
        assert(leases.back()->publish());
    }
    lux::navigation::NavigationPathRequest stitched_request;
    stitched_request.start = {64.0, 4.0, 128.0};
    stitched_request.destination = {448.0, 4.0, 128.0};
    stitched_request.start_region = cooked_regions[0].id;
    stitched_request.destination_region = cooked_regions[1].id;
    const auto stitched = backend->query(stitched_request);
    assert(stitched.status ==
           lux::navigation::ENavigationPathStatus::COMPLETE);
    stitched_request.maximum_path_points = 2u;
    const auto truncated = backend->query(stitched_request);
    assert(truncated.status ==
           lux::navigation::ENavigationPathStatus::PARTIAL);
    stitched_request.maximum_path_points = 512u;
    assert(leases[1]->hide());
    const auto pending = backend->query(stitched_request);
    assert(pending.status ==
           lux::navigation::ENavigationPathStatus::PENDING);
    assert(pending.missing_regions ==
           std::vector<lux::navigation::NavigationRegionId>{
               cooked_regions[1].id});
    retire(*leases[1]);
    retire(*leases[0]);
    const auto navigation_closed = backend->snapshot();
    assert(navigation_closed.staged_regions == 0u);
    assert(navigation_closed.active_regions == 0u);
    assert(navigation_closed.retiring_regions == 0u);
    assert(navigation_closed.owned_bytes == 0u);

    auto malformed = source;
    malformed.actors.front().name_table.pop_back();
    const auto rejected = lux::toolchain::adaptSpatial3DEntityScene(
        malformed, components, mesh_assets);
    assert(!rejected);
    assert(rejected.error().code ==
        lux::toolchain::ESpatial3DEntitySceneAdapterError::
            INVALID_COMPONENT_PAYLOAD);
    return 0;
}
