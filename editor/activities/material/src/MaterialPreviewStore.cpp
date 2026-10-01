#include <lux/engine/editor/material/MaterialPreviewStore.hpp>
#include <lux/engine/scene/RenderSceneState.hpp>
#include <lux/engine/scene/RenderAssets.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
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
#include <atomic>
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
                VMaterialCompileFailure{MaterialPreviewFailure{std::move(domain), std::move(error)}}
            );
        }
        lux::cxx::SharedBytes<> own(std::vector<std::byte> bytes)
        {
            auto owner = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
            return lux::cxx::SharedBytes<>::fromOwner(owner, *owner);
        }

        MaterialCompileResult<lux::cxx::SharedBytes<>> sphereImage(lux::asset::AssetId id)
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

        MaterialCompileResult<std::shared_ptr<const lux::world::WorldDescription>> previewWorld(
            lux::cxx::SharedBytes<>& volume
        )
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

    struct MaterialPreviewStore::Impl final
    {
        struct Preview final
        {
            explicit Preview(lux::scene::SceneRuntime& host) : runtime(host) {}
            lux::scene::SceneRuntime& runtime;
            std::optional<lux::scene::SceneInstanceLease> scene;
            simulation::ecs::Entity camera{simulation::ecs::NullEntity}, sphere{simulation::ecs::NullEntity};
            lux::scene::RenderSceneReceipt receipt;
            asset::AssetId mesh;
            lux::cxx::SharedBytes<> mesh_image, world_volume;
            lux::scene::RenderAssetInput successful, candidate;
            std::uint64_t generation{};
            std::shared_ptr<const CompiledMaterial> displayed, pending;
            std::string error;
        };
        lux::scene::SceneRuntime& runtime_;
        MaterialPreviewEnvironment environment_;
        lux::scene::RenderAssetInput base_;
        const std::uint64_t target_;
        MaterialCompileKey desired_;
        std::shared_ptr<const CompiledMaterial> prepared_;
        std::unique_ptr<Preview> preview_;
        lux::scene::InstanceRetirement retirement_;
        std::string preview_failure_;
        bool closing_{};
        inline static constexpr lux::system::SystemInstanceId preview_render_system_{2};
        Impl(lux::scene::SceneRuntime& runtime, MaterialPreviewEnvironment environment, std::uint64_t target)
            : runtime_(runtime), environment_(std::move(environment)), target_(target)
        {}
        MaterialCompileResult<void> createPreview();
        void update();
    };
    MaterialCompileResult<void> MaterialPreviewStore::Impl::createPreview()
    {
        auto& renderer = *environment_.scene.renderer;
        auto& resources = *environment_.scene.resources;
        const auto& registrations = environment_.scene;
        if (preview_)
            return lux::cxx::unexpected(VMaterialCompileFailure{EMaterialCompileRequestError::BUSY});
        auto& runtime = runtime_;
        auto preview = std::make_unique<Preview>(runtime);
        auto* base = &base_;
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
            const auto& features = environment_.features;
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
        preview->scene = std::move(*instance);
        static_cast<void>(runtime.pauseSimulation(preview->scene->id()));
        auto& registry = runtime.borrowInstance(preview->scene->id())->get();
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

    MaterialPreviewStore::MaterialPreviewStore(
        lux::scene::SceneRuntime& runtime,
        MaterialPreviewEnvironment environment
    )
    {
        static std::atomic_uint64_t next{1};
        const auto target = next.fetch_add(1, std::memory_order_relaxed);
        if (target == 0 || target == UINT64_MAX)
            std::terminate();
        impl_ = std::make_unique<Impl>(runtime, std::move(environment), target);
    }
    MaterialPreviewStore::~MaterialPreviewStore()
    {
        static_cast<void>(close());
    }
    std::uint64_t MaterialPreviewStore::target() const noexcept
    {
        return impl_->target_;
    }
    void MaterialPreviewStore::setDesired(MaterialCompileKey key) noexcept
    {
        if (!impl_->closing_ && key.target == target())
            impl_->desired_ = key;
    }
    MaterialCompileResult<void> MaterialPreviewStore::receive(
        const MaterialCompileOperation& operation,
        lux::scene::RenderAssetInput assets
    )
    {
        if (!operation.ready())
            return lux::cxx::unexpected(VMaterialCompileFailure{EMaterialCompileRequestError::BUSY});
        if (impl_->closing_ || operation.key() != impl_->desired_)
            return {}; // Settled stale work, never labelled as the desired effect.
        auto result = operation.result();
        if (!result)
        {
            impl_->preview_failure_ = "Compilation failed; previous preview is stale";
            return lux::cxx::unexpected(result.error());
        }
        impl_->prepared_ = std::move(*result);
        impl_->base_ = std::move(assets);
        impl_->preview_failure_.clear();
        return {};
    }
    void MaterialPreviewStore::Impl::update()
    {
        if (prepared_ && prepared_->key != desired_)
            prepared_.reset();
        if (closing_ || !environment_.scene.renderer || !environment_.scene.resources)
            return;
        if (prepared_ && !preview_)
        {
            auto created = createPreview();
            if (!created)
            {
                preview_failure_ = "Preview creation failed";
                return;
            }
        }
        if (!preview_ || !preview_->scene)
            return;
        auto& p = *preview_;
        const auto borrowed = runtime_.borrowInstance(p.scene->id());
        if (!borrowed)
            return; // Prepared bytes remain owned during a busy stable/publication pass.
        auto& registry = borrowed->get();
        auto& assets = *lux::scene::RenderAssets::find(registry, preview_render_system_);
        if (p.candidate && p.pending->key != desired_)
        {
            if (!assets.replaceInput(p.successful))
                return;
            p.candidate = {};
            p.pending.reset();
            if (!p.displayed)
                registry.remove<ecs::Mesh3D>(p.sphere);
        }
        if (prepared_ && !p.candidate)
        {
            if (p.generation == UINT64_MAX)
            {
                p.error = "Preview generation exhausted";
                return;
            }
            auto port = process::asset_loading::makeAssetReadOverlay(
                {{prepared_->artifact->id(), {prepared_->bytes}}, {p.mesh, {p.mesh_image}}},
                base_.reads
            );
            if (!port)
            {
                p.error = "Preview asset source failed";
                return;
            }
            // The existing Runtime allocates instance identities across all callers; no per-DLL
            // preview counter is used as a shared RenderResources cache namespace.
            const auto instance = p.scene->id();
            lux::scene::RenderAssetInput candidate{
                {instance.domain, (std::uint64_t{instance.slot} << 32) | instance.generation},
                ++p.generation,
                std::move(*port)
            };
            if (!assets.replaceInput(candidate))
                return;
            registry.emplace_or_replace<ecs::Mesh3D>(
                p.sphere,
                lux::rdesc::MeshVisualDescription{p.mesh, prepared_->artifact->id(), true, false, false}
            );
            p.pending = std::exchange(prepared_, {});
            p.candidate = std::move(candidate);
            p.error.clear();
        }
        if (p.candidate)
            for (const auto& row : assets.statuses())
            {
                if (row.key.entity != p.sphere || row.key.source_version != p.candidate.version)
                    continue;
                if (row.state == lux::scene::ERenderAssetState::READY)
                {
                    p.successful = std::exchange(p.candidate, {});
                    p.displayed = std::exchange(p.pending, {});
                }
                else if (row.state == lux::scene::ERenderAssetState::FAILED ||
                         row.state == lux::scene::ERenderAssetState::CANCELLED)
                {
                    if (!assets.replaceInput(p.successful))
                        return;
                    p.error = "Preview resources failed; retaining the last successful material";
                    p.candidate = {};
                    p.pending.reset();
                    if (!p.displayed)
                        registry.remove<ecs::Mesh3D>(p.sphere);
                }
                break;
            }
    }
    void MaterialPreviewStore::update() noexcept
    {
        impl_->update();
    }
    MaterialCompileResult<void> MaterialPreviewStore::reset(lux::scene::RenderAssetInput base)
    {
        if (impl_->preview_ && impl_->preview_->scene)
        {
            auto& p = *impl_->preview_;
            const auto registry = impl_->runtime_.borrowInstance(p.scene->id());
            if (!registry)
                return lux::cxx::unexpected(VMaterialCompileFailure{EMaterialCompileRequestError::BUSY});
            auto& assets = *lux::scene::RenderAssets::find(registry->get(), Impl::preview_render_system_);
            if (!assets.replaceInput(base))
                return lux::cxx::unexpected(VMaterialCompileFailure{EMaterialCompileRequestError::BUSY});
            registry->get().remove<ecs::Mesh3D>(p.sphere);
            p.successful = base;
            p.candidate = {};
            p.displayed.reset();
            p.pending.reset();
            p.error.clear();
        }
        impl_->base_ = std::move(base);
        impl_->prepared_.reset();
        impl_->desired_ = {};
        impl_->preview_failure_.clear();
        return {};
    }
    MaterialCompileResult<void> MaterialPreviewStore::navigate(
        const ecs::Transform3D& pose,
        const lux::scene::Camera& camera
    )
    {
        auto registry = impl_->runtime_.borrowInstance(instance());
        if (!registry)
            return lux::cxx::unexpected(VMaterialCompileFailure{EMaterialCompileRequestError::BUSY});
        registry->get().patch<ecs::Transform3D>(impl_->preview_->camera, [&](auto& value) { value = pose; });
        registry->get().patch<lux::scene::Camera>(impl_->preview_->camera, [&](auto& value) { value = camera; });
        return {};
    }
    MaterialPreviewStatus MaterialPreviewStore::status() const
    {
        MaterialPreviewStatus result{impl_->desired_};
        if (impl_->prepared_)
            result.prepared = impl_->prepared_->key;
        if (impl_->preview_)
        {
            const auto& p = *impl_->preview_;
            if (p.pending)
                result.prepared = p.pending->key;
            if (p.displayed)
                result.accepted = p.displayed->key;
            result.diagnostic = p.error;
        }
        if (!impl_->preview_failure_.empty())
            result.diagnostic = impl_->preview_failure_;
        result.stale = result.accepted && *result.accepted != result.desired;
        return result;
    }
    simulation::ecs::Entity MaterialPreviewStore::camera() const noexcept
    {
        return impl_->preview_ ? impl_->preview_->camera : simulation::ecs::NullEntity;
    }
    lux::scene::SceneInstanceId MaterialPreviewStore::instance() const noexcept
    {
        return impl_->preview_ && impl_->preview_->scene ? impl_->preview_->scene->id() : lux::scene::SceneInstanceId{};
    }
    lux::scene::InstanceRetirement MaterialPreviewStore::close() noexcept
    {
        impl_->closing_ = true;
        impl_->prepared_.reset();
        if (impl_->preview_ && impl_->preview_->scene)
        {
            impl_->retirement_ = impl_->preview_->scene->retire();
            impl_->preview_->scene.reset();
        }
        return impl_->retirement_;
    }
}
