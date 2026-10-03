#include <lux/engine/editor/material/MaterialPreviewRecipe.hpp>
#include <lux/engine/scene/RenderSceneState.hpp>
#include <lux/engine/scene/RenderAssets.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/process/asset_loading/AssetReadOverlay.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <random>

namespace lux::editor::material
{
    namespace ecs = lux::simulation::ecs;
    namespace
    {
        template<class E> auto failed(std::string domain, E error)
        {
            return cxx::unexpected(MaterialPreviewFailure{
                EMaterialPreviewError::PREPARATION, std::move(domain), std::move(error)
            });
        }
        auto busy()
        {
            return cxx::unexpected(MaterialPreviewFailure{EMaterialPreviewError::BUSY, "preview.busy"});
        }
        struct PreparedPreview final
        {
            PreviewAdoptionKey key;
            std::shared_ptr<const CompiledMaterial> compiled;
        };
    }
    struct MaterialPreview::Impl final
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
            std::optional<PreparedPreview> displayed, pending;
            std::string error;
        };
        lux::scene::SceneRuntime& runtime_;
        MaterialPreviewEnvironment environment_;
        lux::scene::RenderAssetInput base_;
        const uuids::uuid target_;
        PreviewAdoptionKey desired_;
        std::optional<PreparedPreview> prepared_;
        std::optional<VMaterialCompileFailure> compilation_failure_;
        std::optional<MaterialPreviewFailure> failure_;
        std::unique_ptr<Preview> preview_;
        lux::scene::InstanceRetirement retirement_;
        std::string preview_failure_;
        bool closing_{};
        inline static constexpr lux::system::SystemInstanceId preview_render_system_{2};
        Impl(lux::scene::SceneRuntime& runtime, MaterialPreviewEnvironment environment, uuids::uuid target)
            : runtime_(runtime), environment_(std::move(environment)), target_(target)
        { desired_.target = target; }
        MaterialPreviewResult<void> createPreview();
        void update();
    };
    MaterialPreviewResult<void> MaterialPreview::Impl::createPreview()
    {
        auto& renderer = *environment_.scene.renderer;
        auto& resources = *environment_.scene.resources;
        const auto& registrations = environment_.scene;
        if (preview_)
            return busy();
        auto& runtime = runtime_;
        auto preview = std::make_unique<Preview>(runtime);
        auto* base = &base_;
        preview->successful = *base;
        auto recipe = makeMaterialPreviewRecipe(environment_.features);
        if (!recipe)
            return cxx::unexpected(recipe.error());
        preview->mesh = recipe->mesh;
        preview->mesh_image = recipe->mesh_image;
        preview->world_volume = recipe->world_volume;
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
                .setDescription(recipe->scene)
                .setWorld(recipe->world)
                .setSimulation(recipe->simulation)
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
        registry.emplace<ecs::Transform3D>(camera, recipe->camera_pose);
        registry.emplace<lux::scene::Camera>(camera, recipe->camera);
        registry.emplace<ecs::Transform3D>(light, recipe->light_pose);
        registry.emplace<ecs::Light3D>(light, recipe->light);
        preview->camera = camera;
        preview->sphere = sphere;
        const auto* render = lux::scene::RenderSceneState::find(registry, preview_render_system_);
        preview->receipt = resources.sceneReceipt(render->resource);
        preview_ = std::move(preview);
        return {};
    }

    MaterialPreview::MaterialPreview(
        lux::scene::SceneRuntime& runtime,
        MaterialPreviewEnvironment environment
    )
    {
        std::mt19937 random{std::random_device{}()};
        const auto target = uuids::uuid_random_generator{random}();
        impl_ = std::make_unique<Impl>(runtime, std::move(environment), target);
    }
    MaterialPreview::~MaterialPreview() { static_cast<void>(close()); }
    MaterialPreviewResult<PreviewAdoptionKey> MaterialPreview::setDesired(MaterialCompileInputKey input) noexcept
    {
        if (impl_->closing_)
            return cxx::unexpected(MaterialPreviewFailure{EMaterialPreviewError::CLOSED, "preview.closed"});
        if (impl_->desired_.generation && impl_->desired_.input == input)
            return impl_->desired_;
        if (impl_->desired_.generation == UINT64_MAX)
            return cxx::unexpected(MaterialPreviewFailure{EMaterialPreviewError::CAPACITY, "preview.generation"});
        impl_->desired_ = {impl_->target_, impl_->desired_.generation + 1, input, 1, impl_->environment_.scene.version};
        impl_->compilation_failure_.reset();
        impl_->failure_.reset();
        return impl_->desired_;
    }
    MaterialPreviewResult<void> MaterialPreview::receive(PreviewAdoptionKey key,
        MaterialCompileResult<std::shared_ptr<const CompiledMaterial>> result, lux::scene::RenderAssetInput assets)
    {
        if (impl_->closing_ || key != impl_->desired_)
            return {}; // Settle accepted stale work without adopting or relabelling it.
        if (!result)
        {
            impl_->compilation_failure_ = std::move(result.error());
            impl_->preview_failure_ = "Compilation failed; previous preview is stale";
            return {};
        }
        if (!*result || (*result)->key() != key.input)
            return cxx::unexpected(MaterialPreviewFailure{EMaterialPreviewError::INVALID_INPUT, "preview.source"});
        impl_->prepared_ = PreparedPreview{key, std::move(*result)};
        impl_->base_ = std::move(assets);
        impl_->compilation_failure_.reset();
        impl_->preview_failure_.clear();
        return {};
    }
    void MaterialPreview::Impl::update()
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
                failure_ = created.error();
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
                failure_ = MaterialPreviewFailure{EMaterialPreviewError::CAPACITY, "preview.resources.generation"};
                return;
            }
            auto port = process::asset_loading::makeAssetReadOverlay(
                {{prepared_->compiled->artifact()->id(), {prepared_->compiled->bytes()}}, {p.mesh, {p.mesh_image}}},
                base_.reads
            );
            if (!port)
            {
                p.error = "Preview asset source failed";
                failure_ = MaterialPreviewFailure{EMaterialPreviewError::PREPARATION, "preview.assets", port.error()};
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
                lux::rdesc::MeshVisualDescription{p.mesh, prepared_->compiled->artifact()->id(), true, false, false}
            );
            p.pending = std::exchange(prepared_, {});
            p.candidate = std::move(candidate);
            p.error.clear();
            failure_.reset();
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
                    failure_ = MaterialPreviewFailure{EMaterialPreviewError::PREPARATION, "preview.resources", row};
                    p.candidate = {};
                    p.pending.reset();
                    if (!p.displayed)
                        registry.remove<ecs::Mesh3D>(p.sphere);
                }
                break;
            }
    }
    void MaterialPreview::update() noexcept
    {
        impl_->update();
    }
    MaterialPreviewResult<void> MaterialPreview::reset(lux::scene::RenderAssetInput base)
    {
        if (impl_->preview_ && impl_->preview_->scene)
        {
            auto& p = *impl_->preview_;
            const auto registry = impl_->runtime_.borrowInstance(p.scene->id());
            if (!registry)
                return busy();
            auto& assets = *lux::scene::RenderAssets::find(registry->get(), Impl::preview_render_system_);
            if (!assets.replaceInput(base))
                return busy();
            registry->get().remove<ecs::Mesh3D>(p.sphere);
            p.successful = base;
            p.candidate = {};
            p.displayed.reset();
            p.pending.reset();
            p.error.clear();
        }
        impl_->base_ = std::move(base);
        impl_->prepared_.reset();
        impl_->desired_.input = {};
        impl_->preview_failure_.clear();
        impl_->compilation_failure_.reset();
        impl_->failure_.reset();
        return {};
    }
    MaterialPreviewResult<void> MaterialPreview::navigate(
        const ecs::Transform3D& pose,
        const lux::scene::Camera& camera
    )
    {
        auto registry = impl_->runtime_.borrowInstance(instance());
        if (!registry)
            return busy();
        registry->get().patch<ecs::Transform3D>(impl_->preview_->camera, [&](auto& value) { value = pose; });
        registry->get().patch<lux::scene::Camera>(impl_->preview_->camera, [&](auto& value) { value = camera; });
        return {};
    }
    MaterialPreviewStatus MaterialPreview::status() const
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
        result.compilation_failure = impl_->compilation_failure_;
        result.failure = impl_->failure_;
        result.stale = result.accepted && *result.accepted != result.desired;
        return result;
    }
    simulation::ecs::Entity MaterialPreview::camera() const noexcept
    {
        return impl_->preview_ ? impl_->preview_->camera : simulation::ecs::NullEntity;
    }
    lux::scene::SceneInstanceId MaterialPreview::instance() const noexcept
    {
        return impl_->preview_ && impl_->preview_->scene ? impl_->preview_->scene->id() : lux::scene::SceneInstanceId{};
    }
    lux::scene::InstanceRetirement MaterialPreview::close() noexcept
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
