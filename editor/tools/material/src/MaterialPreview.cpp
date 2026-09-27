#include <lux/engine/scene/RenderSceneState.hpp>
#include <lux/engine/scene/RenderAssets.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/editor/material/MaterialEditorImpl.hpp>
#include <lux/engine/editor/material/MaterialPreview.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/process/asset_loading/AssetReadOverlay.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/scene/TransformSystem.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/WorldStorageCodec.hpp>
#include <numbers>

namespace lux::editor::material
{
    namespace
    {
        namespace ecs = lux::simulation::ecs;
        template <class T> T previewIdentity(std::uint8_t tail)
        {
            std::array<std::uint8_t, 16>
                bytes{0xf3, 0x29, 0x45, 0x93, 0x22, 0x13, 0x41, 0xca, 0xbb, 0x33, 0xa6, 0x24, 0x71, 0x06, 0x23, tail};
            return T{uuids::uuid{bytes}};
        }
        template <class Error> auto failed(std::string domain, Error error)
        {
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, std::move(domain), 0, {}, std::move(error)}
            );
        }
        lux::cxx::SharedBytes<> own(std::vector<std::byte> bytes)
        {
            auto owner = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
            return lux::cxx::SharedBytes<>::fromOwner(owner, *owner);
        }

        EditorResult<lux::cxx::SharedBytes<>> sphereImage(lux::asset::AssetId id)
        {
            auto mesh = std::make_shared<lux::rdesc::Mesh>();
            constexpr std::uint32_t slices = 48, rings = 24;
            mesh->vertices.reserve((slices + 1) * (rings + 1));
            for (std::uint32_t y{}; y <= rings; ++y)
            {
                const float v = float(y) / rings, latitude = v * std::numbers::pi_v<float>;
                for (std::uint32_t x{}; x <= slices; ++x)
                {
                    const float u = float(x) / slices, longitude = u * 2 * std::numbers::pi_v<float>;
                    lux::rdesc::Vertex vertex{};
                    vertex.position = {
                        std::sin(latitude) * std::cos(longitude),
                        std::cos(latitude),
                        std::sin(latitude) * std::sin(longitude)
                    };
                    vertex.normal = vertex.position;
                    vertex.tangent = {-std::sin(longitude), 0, std::cos(longitude)};
                    vertex.bitangent = vertex.normal.cross(vertex.tangent);
                    vertex.uv = {u, v};
                    mesh->vertices.push_back(vertex);
                }
            }
            for (std::uint32_t y{}; y < rings; ++y)
                for (std::uint32_t x{}; x < slices; ++x)
                {
                    const auto a = y * (slices + 1) + x, b = a + slices + 1;
                    if (y != 0)
                        mesh->indices.insert(mesh->indices.end(), {a, a + 1, b});
                    if (y + 1 != rings)
                        mesh->indices.insert(mesh->indices.end(), {a + 1, b + 1, b});
                }
            auto asset = lux::asset::MeshAsset::create({id, lux::asset::MeshAsset::asset_type}, std::move(mesh));
            if (!asset)
                return failed("preview.mesh", asset.error());
            auto encoded = lux::asset::TAssetSerDeser<lux::asset::MeshAsset>::encode(
                **asset,
                lux::asset::AssetEncodeLimits{4U * 1024U * 1024U}
            );
            if (!encoded)
                return failed("preview.mesh.encode", encoded.error());
            return own(std::move(*encoded));
        }

        EditorResult<std::shared_ptr<const lux::world::WorldDescription>> previewWorld(lux::cxx::SharedBytes<>& volume)
        {
            using namespace lux::world;
            const auto bundle = previewIdentity<WorldBundleId>(1);
            const auto generation = previewIdentity<WorldBundleGeneration>(2);
            const std::array records{WorldPartitionRecord{previewIdentity<WorldPartitionId>(3), 0, 1}};
            const std::array extents{WorldPartitionExtent{0, 1, 1}};
            auto data = encodeWorldPartitionData({0}, {});
            auto table = encodeWorldPartitionTablePage({0}, records, extents);
            if (!data)
                return failed("preview.partition", data.error());
            if (!table)
                return failed("preview.table", table.error());
            const std::array chunks{
                WorldStorageChunkInput{EWorldStorageChunkKind::PARTITION_TABLE_PAGE, EWorldStorageCodec::NONE, *table},
                WorldStorageChunkInput{EWorldStorageChunkKind::WORLD_PARTITION_DATA, EWorldStorageCodec::NONE, *data}
            };
            auto encoded = encodeWorldStorageVolume(bundle, generation, 0, chunks);
            if (!encoded)
                return failed("preview.volume", encoded.error());
            volume = own(std::move(*encoded));
            WorldDescriptionBuilder world;
            auto built = world.setIdentity(bundle, generation, "Material preview");
            if (built)
                built = world.setPartitioner({worldPartitionerId("lux.spatial.builtin.single"), 1}, 1);
            if (built)
                built = world.addStorageVolume({"Preview.wvol", 1, 2, volume.size()});
            if (built)
                built = world.addPartitionTablePage({{0}, 1, {0, 0}});
            if (!built)
                return failed("preview.world", built.error());
            auto result = std::move(world).build();
            if (!result)
                return failed("preview.world", result.error());
            return std::make_shared<const WorldDescription>(std::move(*result));
        }
    }

    EditorResult<void> MaterialEditor::Impl::createPreview()
    {
        auto& renderer = editor_context_.renderRuntime();
        auto& resources = editor_context_.renderResources();
        const auto& registrations = editor_context_.sceneRegistrations();
        if (preview_)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "preview.exists"});
        auto& runtime = editor_context_.engine().sceneRuntime();
        auto preview = std::make_unique<Preview>(runtime);
        auto base = capturePreviewAssets();
        if (!base)
            return lux::cxx::unexpected(base.error());
        preview->successful = *base;
        preview->mesh = previewIdentity<lux::asset::AssetId>(4);
        auto mesh = sphereImage(preview->mesh);
        if (!mesh)
            return lux::cxx::unexpected(mesh.error());
        preview->mesh_image = std::move(*mesh);
        auto world = previewWorld(preview->world_volume);
        if (!world)
            return lux::cxx::unexpected(world.error());
        lux::simulation::SimulationDescriptionBuilder simulation;
        auto transform = lux::scene::makeTransformSystemConfiguration(64, {256, 65536});
        if (!transform)
            return failed("preview.transform", transform.error());
        auto rules = std::move(simulation).build();
        if (!rules)
            return failed("preview.simulation", rules.error());
        lux::scene::RenderSystemConfiguration configuration;
        for (const auto name :
             {"lux.render.view_camera.v1",
              "lux.render.material.v1",
              "lux.render.mesh_stack.v1",
              "lux.render.light.v1",
              "lux.render.forward_mesh.v1",
              "lux.render.shadow_map.v1"})
        {
            const auto& features = registrations.features;
            auto found = std::ranges::find(features, std::string_view{name}, [](const auto& entry) {
                return entry.factory.descriptor.canonical_name;
            });
            if (found == features.end())
                return failed("preview.feature.missing", std::string(name));
            std::vector<std::byte> bytes;
            auto encoded = found->configuration.portable.encode_default(bytes);
            if (!encoded)
                return failed("preview.feature.configuration", encoded.error());
            configuration.features.push_back(
                {found->factory.descriptor.type,
                 std::move(bytes),
                 std::string(found->configuration.schema),
                 found->configuration.schema_version}
            );
        }
        const auto registration = lux::scene::builtinRenderSystemRegistration();
        std::vector<std::byte> bytes;
        auto encoded = registration.configuration.encode(&configuration, bytes);
        if (!encoded)
            return failed("preview.render.configuration", encoded.error());
        lux::scene::SceneDescriptionBuilder description;
        const auto transform_registration = lux::scene::transformSystemRegistration();
        auto transform_added = description.addSystem(
            {1},
            "transform",
            transform_registration.type,
            transform_registration.description->version,
            transform_registration.description->configuration_schema_name,
            transform_registration.description->configuration_schema_version,
            *transform
        );
        if (!transform_added)
            return failed("preview.transform", transform_added.error());
        auto render_added = description.addSystem(
            preview_render_system_,
            "preview-render",
            registration.type,
            registration.description->version,
            registration.description->configuration_schema_name,
            registration.description->configuration_schema_version,
            bytes
        );
        if (!render_added)
            return failed("preview.description", render_added.error());
        auto scene = std::move(description).buildResolved();
        if (!scene)
            return failed("preview.description", scene.error());
        lux::scene::RenderFeatureSceneBindings bindings = registrations.render_bindings;
        std::array providers{
            lux::scene::makeSceneCapabilityProvider<lux::render::RenderRuntime>(
                "runtime",
                "lux.render.runtime",
                renderer
            ),
            lux::scene::makeSceneCapabilityProvider<lux::scene::RenderFeatureSceneBindings>(
                "bindings",
                "lux.render.scene_bindings",
                bindings
            ),
            lux::scene::makeSceneCapabilityProvider<lux::scene::RenderResources>(
                "resources",
                "lux.render.resources",
                resources
            ),
            lux::scene::makeSceneCapabilityProvider<lux::scene::RenderAssetInput>("assets", "lux.render.assets", *base)
        };
        auto instance =
            runtime.builder()
                .setDescription(std::make_shared<const lux::scene::SceneDescription>(std::move(*scene)))
                .setWorld(std::move(*world))
                .setSimulation(std::make_shared<const lux::simulation::SimulationDescription>(std::move(*rules)))
                .setRegistrations(
                    registrations.components,
                    *registrations.simulation_systems,
                    registrations.scene_systems
                )
                .setProviders(providers)
                .build();
        if (!instance)
            return failed("preview.create", instance.error());
        preview->scene = *instance;
        static_cast<void>(runtime.invalid(*instance));
        auto& registry = runtime.getSceneRegistry(*instance)->get();
        const auto sphere = registry.create(), camera = registry.create(), light = registry.create();
        registry.emplace<ecs::Transform3D>(sphere);
        ecs::Transform3D camera_pose;
        camera_pose.translation = {0, 0, 3.5};
        registry.emplace<ecs::Transform3D>(camera, camera_pose);
        registry.emplace<lux::scene::Camera>(camera, lux::scene::Camera{lux::scene::PerspectiveProjection{}, true});
        ecs::Transform3D light_pose;
        light_pose.rotation = Eigen::Quaterniond(
            Eigen::AngleAxisd(-0.6, Eigen::Vector3d::UnitX()) * Eigen::AngleAxisd(-0.5, Eigen::Vector3d::UnitY())
        );
        registry.emplace<ecs::Transform3D>(light, light_pose);
        lux::rdesc::LightDescription lamp;
        lamp.type = lux::rdesc::ELightType::DIRECTIONAL;
        lamp.intensity = 3.0F;
        lamp.cast_shadow = false;
        registry.emplace<ecs::Light3D>(light, lamp);
        preview->camera = camera;
        preview->sphere = sphere;
        const auto* render = lux::scene::RenderSceneState::find(registry, preview_render_system_);
        preview->receipt = resources.sceneReceipt(render->resource);
        preview_ = std::move(preview);
        return {};
    }

    EditorResult<lux::scene::RenderAssetInput> MaterialEditor::Impl::capturePreviewAssets()
    {
        auto reads = project().captureAssetReads();
        if (!reads)
            return lux::cxx::unexpected(reads.error());
        const auto identity = project().reference({});
        return lux::scene::RenderAssetInput{
            {identity.project_instance, 0},
            identity.catalog_revision,
            std::move(*reads)
        };
    }

    void MaterialEditor::Impl::updatePreview(
        lux::cxx::SharedBytes<> bytes,
        editing::StateId state,
        lux::scene::RenderAssetInput base
    )
    {
        if (!base)
            return;
        if (!preview_)
        {
            auto created = createPreview();
            if (!created)
            {
                preview_failure_ = created.error().domain + ": " + created.error().message;
                return;
            }
            preview_failure_.clear();
        }
        if (!preview_->scene)
            return;
        auto& p = *preview_;
        const auto observed = std::as_const(p.runtime).getSceneRegistry(*p.scene);
        if (!observed ||
            !observed->get().ctx().get<std::reference_wrapper<const lux::scene::SceneDriveSnapshot>>().get().result)
        {
            p.error = "Preview scene failed";
            return;
        }
        const auto borrowed = p.runtime.getSceneRegistry(*p.scene);
        if (!borrowed)
            return;
        auto& registry = borrowed->get();
        if (p.generation == UINT64_MAX)
        {
            p.error = "Preview resource generation exhausted";
            return;
        }
        auto port = process::asset_loading::makeAssetReadOverlay(
            {{source().id, {std::move(bytes)}}, {p.mesh, {p.mesh_image}}},
            base.reads
        );
        if (!port)
        {
            p.error = "Preview asset source failed";
            return;
        }
        const auto identity = project().reference({});
        lux::scene::RenderAssetInput candidate{
            {identity.project_instance, historyId().value},
            ++p.generation,
            std::move(*port)
        };
        auto& assets = *lux::scene::RenderAssets::find(registry, preview_render_system_);
        auto replaced = assets.replaceInput(candidate);
        if (!replaced)
        {
            p.error = "Preview resource replacement failed";
            return;
        }
        registry.emplace_or_replace<ecs::Mesh3D>(
            p.sphere,
            lux::rdesc::MeshVisualDescription{p.mesh, source().id, true, false, false}
        );
        p.pending = state;
        p.candidate = std::move(candidate);
        p.error.clear();
    }

    EditorResult<void> MaterialEditor::Impl::resetPreview()
    {
        if (!preview_ || !preview_->scene)
            return {};
        auto& preview = *preview_;
        const auto borrowed = preview.runtime.getSceneRegistry(*preview.scene);
        if (!borrowed)
            return failed("preview.reset", borrowed.error());
        auto& registry = borrowed->get();
        auto base = capturePreviewAssets();
        if (!base)
            return lux::cxx::unexpected(base.error());
        auto& assets = *lux::scene::RenderAssets::find(registry, preview_render_system_);
        auto changed = assets.replaceInput(*base);
        if (!changed)
            return failed("preview.reset", changed.error());
        preview.successful = std::move(*base);
        preview.candidate = {};
        preview.displayed = {};
        preview.pending = {};
        preview.error.clear();
        registry.remove<ecs::Mesh3D>(preview.sphere);
        return {};
    }

    void MaterialEditor::Impl::maintainPreview() noexcept
    {
        if (!preview_ || !preview_->scene)
            return;
        auto& p = *preview_;
        const auto observed = std::as_const(p.runtime).getSceneRegistry(*p.scene);
        if (!observed ||
            !observed->get().ctx().get<std::reference_wrapper<const lux::scene::SceneDriveSnapshot>>().get().result)
        {
            p.error = "Preview scene failed";
            return;
        }
        const auto borrowed = p.runtime.getSceneRegistry(*p.scene);
        if (!borrowed)
            return;
        auto& registry = borrowed->get();
        auto& assets = *lux::scene::RenderAssets::find(registry, preview_render_system_);
        const auto current = historyView();
        if (p.candidate && (!current || current->history.current != p.pending))
        {
            auto restored = assets.replaceInput(p.successful);
            if (!restored)
            {
                p.error = "Preview resource restoration failed";
                return;
            }
            p.candidate = {};
            if (!p.displayed.history.value)
                registry.remove<ecs::Mesh3D>(p.sphere);
        }
        if (p.candidate)
        {
            for (const auto& row : assets.statuses())
            {
                if (row.key.entity != p.sphere || row.key.source_version != p.candidate.version)
                    continue;
                if (row.state == lux::scene::ERenderAssetState::READY)
                {
                    p.successful = std::exchange(p.candidate, {});
                    p.displayed = p.pending;
                }
                else if (row.state == lux::scene::ERenderAssetState::FAILED ||
                         row.state == lux::scene::ERenderAssetState::CANCELLED)
                {
                    p.error = "Preview resources failed; retaining the last successful material";
                    static_cast<void>(assets.replaceInput(p.successful));
                    p.candidate = {};
                    if (!p.displayed.history.value)
                        registry.remove<ecs::Mesh3D>(p.sphere);
                }
                break;
            }
        }
    }

    lux::simulation::ecs::Entity MaterialEditor::Impl::previewCamera() const noexcept
    {
        return preview_ ? preview_->camera : lux::simulation::ecs::NullEntity;
    }
    lux::scene::SceneInstanceId MaterialEditor::Impl::previewInstance() const noexcept
    {
        return preview_ ? preview_->scene.value_or(lux::scene::SceneInstanceId{}) : lux::scene::SceneInstanceId{};
    }
    std::string MaterialEditor::Impl::previewStatus() const
    {
        if (!preview_failure_.empty())
            return preview_failure_;
        if (!preview_)
            return "Not compiled";
        if (!preview_->error.empty())
            return preview_->error;
        if (preview_->candidate)
            return "Preparing compiled material...";
        if (!preview_->displayed.history.value)
            return "Not compiled";
        const auto current = historyView();
        return current && current->history.current == preview_->displayed ? "Static preview"
                                                                          : "Recompile to update preview";
    }
    EditorResult<void> MaterialEditor::Impl::navigatePreview(
        const ecs::Transform3D& pose,
        const lux::scene::Camera& camera
    )
    {
        if (!preview_ || !preview_->scene)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "preview.camera"});
        auto& p = *preview_;
        const auto borrowed = p.runtime.getSceneRegistry(*p.scene);
        if (!borrowed)
            return failed("preview.camera", borrowed.error());
        auto& registry = borrowed->get();
        registry.patch<ecs::Transform3D>(p.camera, [&](auto& value) { value = pose; });
        registry.patch<lux::scene::Camera>(p.camera, [&](auto& value) { value = camera; });
        return {};
    }
}

namespace lux::editor::material
{

}
