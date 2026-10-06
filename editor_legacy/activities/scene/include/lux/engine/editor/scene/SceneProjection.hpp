#pragma once
#include <lux/engine/editor/scene/SceneSession.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/scene/RenderResources.hpp>
#include <lux/engine/scene/RenderFeatureSceneBinding.hpp>
#include <lux/engine/process/TaskScope.hpp>

namespace lux::services
{
    struct ServiceDescriptor;
}

namespace lux::editor::scene
{
    extern const services::ServiceDescriptor kScenePresentationHub;
    enum class EProjectionError : std::uint8_t
    {
        BUSY,
        CAPACITY,
        INVALID_SOURCE,
        WRONG_THREAD,
        INVALID_ENVIRONMENT
    };
    struct ProjectionFailure final
    {
        using VCause = std::
            variant<EProjectionError, SceneEditError, lux::scene::ScenePackageFailure, lux::scene::SceneRuntimeFailure>;
        VCause cause;
    };
    template <class T> using ProjectionResult = lux::cxx::expected<T, ProjectionFailure>;
    struct ProjectionEnvironment final
    {
        simulation::ecs::ComponentSchemaSet components;
        std::shared_ptr<const simulation::SimulationSystemRegistry> simulation_systems;
        std::vector<lux::scene::SceneSystemRegistration> scene_systems;
        std::vector<lux::scene::RenderFeatureSceneBinding> render_bindings;
        render::RenderRuntime* renderer{};
        lux::scene::RenderResources* resources{};
        lux::scene::RenderAssetInput assets;
        std::uint64_t version{1};
    };
    struct ProjectionVersion final
    {
        sessions::ContentStamp content;
        std::uint64_t configuration{}, environment{}, serial{};
        friend bool operator==(const ProjectionVersion&, const ProjectionVersion&) = default;
    };
    // Complete, frozen package assembly is shared by author projection consumers.
    [[nodiscard]] ProjectionResult<lux::scene::SceneInstanceLease>
    instantiateAuthorProjection(lux::scene::SceneRuntime&, const lux::scene::ScenePackage&, const ProjectionEnvironment&, std::shared_ptr<process::TaskScope>);
    class ScenePresentationHub;
    class SceneProjection final
    {
    public:
        ~SceneProjection();
        [[nodiscard]] lux::scene::SceneInstanceId instance() const noexcept;
        [[nodiscard]] ProjectionVersion version() const noexcept;
        // RESET_REQUIRED rebuilds from a complete immutable capture. No live source is stored.
        [[nodiscard]] ProjectionResult<void> update(const SceneSession&);
        [[nodiscard]] std::size_t rebuildCount() const noexcept;

    private:
        friend class ScenePresentationHub;
        struct Impl;
        explicit SceneProjection(std::unique_ptr<Impl>);
        std::unique_ptr<Impl> impl_;
    };
    // Records, including pending retirees, consume capacity. No unbounded weak cache or tombstones.
    class ScenePresentationHub final
    {
    public:
        ScenePresentationHub(lux::scene::SceneRuntime&, process::ExecutionRuntime&, std::size_t capacity = 16);
        ~ScenePresentationHub();
        ScenePresentationHub(const ScenePresentationHub&) = delete;
        ScenePresentationHub& operator=(const ScenePresentationHub&) = delete;
        ScenePresentationHub(ScenePresentationHub&&) = delete;
        ScenePresentationHub& operator=(ScenePresentationHub&&) = delete;
        [[nodiscard]] ProjectionResult<std::shared_ptr<SceneProjection>> acquire(
            const SceneSession&,
            ProjectionEnvironment,
            std::uint64_t configuration = 1
        );
        void collectReleased() noexcept;
        [[nodiscard]] std::size_t size() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
