#include <lux/engine/editor/scene/ProjectSceneEnvironment.hpp>
#include <lux/engine/editor/scene/SceneProjection.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>

namespace lux::editor::scene
{
    namespace
    {
        using SimulationSystems = std::shared_ptr<const simulation::SimulationSystemRegistry>;
        using SceneSystems = std::vector<lux::scene::SceneSystemRegistration>;
        using RenderBindings = std::vector<lux::scene::RenderFeatureSceneBinding>;

        class ProjectSceneEnvironment final
        {
        public:
            ProjectSceneEnvironment(ProjectStorage& project, ProjectionEnvironment environment)
                : project_(project), environment_(std::move(environment))
            {
            }
            ProjectSceneEnvironment(const ProjectSceneEnvironment&) = delete;
            ProjectSceneEnvironment& operator=(const ProjectSceneEnvironment&) = delete;
            ProjectSceneEnvironment(ProjectSceneEnvironment&&) = delete;
            ProjectSceneEnvironment& operator=(ProjectSceneEnvironment&&) = delete;

            ProjectionEnvironment& environment() noexcept
            {
                return environment_;
            }
            services::ServiceResult<void> refresh() noexcept
            {
                const auto revision = project_.catalogRevision();
                if (environment_.assets.version == revision)
                {
                    return {};
                }
                auto reads = project_.captureAssetReads();
                if (!reads)
                {
                    const auto& error = reads.error();
                    return cxx::unexpected(services::ServiceFailure{
                        error.code == EEditorError::BUSY ? services::EServiceError::BUSY
                                                         : services::EServiceError::FACTORY_FAILURE,
                        error.message,
                        error.domain,
                        static_cast<std::uint64_t>(error.code)
                    });
                }
                // Publish one complete version. Existing projections/Run preparations retain their
                // frozen input; failure above leaves the last accepted environment untouched.
                auto next = environment_.assets;
                next.reads = std::move(*reads);
                next.version = revision;
                std::swap(environment_.assets, next);
                environment_.version = revision;
                return {};
            }

        private:
            ProjectStorage& project_;
            ProjectionEnvironment environment_;
        };
        constexpr services::ServiceContract contracts[]{
            {services::ServiceNameView{"lux.editor.scene.projection.environment"},
             1,
             cxx::typeToken<ProjectionEnvironment>(),
             [](void* value) noexcept -> void*
             { return &static_cast<ProjectSceneEnvironment*>(value)->environment(); }}
        };
        constexpr services::ServiceDependency dependencies[]{
            {services::ServiceNameView{"lux.editor.project.storage"},
             1, cxx::typeToken<ProjectStorage>(), services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.simulation.components"},
             1, cxx::typeToken<simulation::ecs::ComponentSchemaSet>(), services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.simulation.systems"},
             1, cxx::typeToken<SimulationSystems>(), services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.scene.systems"},
             1, cxx::typeToken<SceneSystems>(), services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.render.scene.bindings"},
             1, cxx::typeToken<RenderBindings>(), services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.render.runtime"},
             1, cxx::typeToken<render::RenderRuntime>(), services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT, {}, {}, true},
            {services::ServiceNameView{"lux.render.resources"},
             1, cxx::typeToken<lux::scene::RenderResources>(), services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT, {}, {}, true}
        };
        services::ServiceResult<std::unique_ptr<ProjectSceneEnvironment>>
        createEnvironment(services::ServiceResolver& resolver, const services::ServiceConfiguration&) noexcept
        {
            auto project = resolver.require<ProjectStorage>(0);
            if (!project)
            {
                return cxx::unexpected(std::move(project.error()));
            }
            auto components = resolver.require<simulation::ecs::ComponentSchemaSet>(1);
            if (!components)
            {
                return cxx::unexpected(std::move(components.error()));
            }
            auto simulations = resolver.require<SimulationSystems>(2);
            if (!simulations)
            {
                return cxx::unexpected(std::move(simulations.error()));
            }
            auto systems = resolver.require<SceneSystems>(3);
            if (!systems)
            {
                return cxx::unexpected(std::move(systems.error()));
            }
            auto bindings = resolver.require<RenderBindings>(4);
            if (!bindings)
            {
                return cxx::unexpected(std::move(bindings.error()));
            }
            auto renderer = resolver.require<render::RenderRuntime>(5);
            const bool has_renderer_failure = !renderer && renderer.error().code != services::EServiceError::NOT_FOUND;
            if (has_renderer_failure)
            {
                return cxx::unexpected(std::move(renderer.error()));
            }
            auto resources = resolver.require<lux::scene::RenderResources>(6);
            const bool has_resources_failure = !resources && resources.error().code != services::EServiceError::NOT_FOUND;
            if (has_resources_failure)
            {
                return cxx::unexpected(std::move(resources.error()));
            }
            auto candidate = std::make_unique<ProjectSceneEnvironment>(
                project->get(),
                ProjectionEnvironment{
                    components->get(), simulations->get(), systems->get(), bindings->get(),
                    renderer ? &renderer->get() : nullptr, resources ? &resources->get() : nullptr,
                    {{project->get().catalogModel().reference({}).project_instance, 0}, 0, {}, {}}, 0
                }
            );
            if (auto refreshed = candidate->refresh(); !refreshed)
            {
                return cxx::unexpected(std::move(refreshed.error()));
            }
            return candidate;
        }
    } // namespace
    constinit const services::ServiceDescriptor kProjectSceneEnvironment = []
    {
        auto descriptor = services::ServiceDescriptor::forType<ProjectSceneEnvironment, createEnvironment>(
            services::ServiceNameView{"lux.editor.scene.project-environment"}, contracts, dependencies
        );
        descriptor.retention = services::EServiceRetention::SCOPED;
        descriptor.affinity = services::EServiceAffinity::OWNER;
        descriptor.maintain = [](void* allocation) noexcept
        { return static_cast<ProjectSceneEnvironment*>(allocation)->refresh(); };
        return descriptor;
    }();
} // namespace lux::editor::scene
