#include <lux/engine/editor/scene/SceneProjection.hpp>
#include <lux/engine/process/world_loading/WorldMemoryStorageSource.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <algorithm>
#include <thread>

namespace lux::editor::scene
{
    namespace
    {
        template <class T> auto rejected(T error)
        {
            return lux::cxx::unexpected(ProjectionFailure{std::move(error)});
        }
        struct Admission final
        {
            bool& active;
            explicit Admission(bool& value) : active(value)
            {
                active = true;
            }
            ~Admission()
            {
                active = false;
            }
        };
    }
    ProjectionResult<lux::scene::SceneInstanceLease> instantiateAuthorProjection(
        lux::scene::SceneRuntime& runtime,
        const lux::scene::ScenePackage& package,
        const ProjectionEnvironment& environment,
        std::shared_ptr<process::TaskScope> tasks
    )
    {
        const bool is_invalid_package = !package.world || !package.scene || !package.simulation;
        const bool is_invalid_environment = !environment.simulation_systems || !tasks || !environment.version;
        if (is_invalid_package || is_invalid_environment)
            return rejected(EProjectionError::INVALID_ENVIRONMENT);
        auto storage = process::world_loading::makeWorldMemoryStorageSource(
            std::shared_ptr<const world::WorldDescription>(package.world, &package.world->data()),
            package.volumes
        );
        if (!storage)
            return rejected(EProjectionError::INVALID_SOURCE);
        lux::scene::WorldLoadingServices loading{std::move(*storage), *tasks};
        for (std::uint32_t ordinal{}; ordinal < package.world->data().partitionCount(); ++ordinal)
            loading.bootstrap.push_back({ordinal});
        loading.initial_partitions = package.partitions;
        auto bindings = lux::scene::RenderFeatureSceneBindings(environment.render_bindings);
        auto assets = environment.assets;
        std::vector<lux::scene::SceneCapabilityProvider> providers;
        providers.push_back(lux::scene::makeSceneCapabilityProvider<lux::scene::WorldLoadingServices>(
            "world-storage",
            "lux.world.loading",
            loading
        ));
        if (environment.renderer && environment.resources)
        {
            providers.push_back(lux::scene::makeSceneCapabilityProvider<render::RenderRuntime>(
                "main-window",
                "lux.render.runtime",
                *environment.renderer
            ));
            providers.push_back(lux::scene::makeSceneCapabilityProvider<lux::scene::RenderResources>(
                "resources",
                "lux.render.resources",
                *environment.resources
            ));
            providers.push_back(lux::scene::makeSceneCapabilityProvider<lux::scene::RenderFeatureSceneBindings>(
                "render-bindings",
                "lux.render.scene_bindings",
                bindings
            ));
            if (assets)
                providers.push_back(lux::scene::makeSceneCapabilityProvider<lux::scene::RenderAssetInput>(
                    "assets",
                    "lux.render.assets",
                    assets
                ));
        }
        auto created =
            runtime.builder()
                .setDescription(
                    std::shared_ptr<const lux::scene::SceneDescription>(package.scene, &package.scene->data())
                )
                .setWorld(std::shared_ptr<const world::WorldDescription>(package.world, &package.world->data()))
                .setSimulation(std::shared_ptr<const simulation::SimulationDescription>(
                    package.simulation,
                    &package.simulation->data()
                ))
                .setRegistrations(environment.components, *environment.simulation_systems, environment.scene_systems)
                .setProviders(providers)
                .build();
        if (!created)
            return rejected(created.error());
        auto registry = runtime.borrowInstance(created->id());
        if (!registry)
            return rejected(registry.error());
        // Task-scope ownership extends through real Runtime retirement, including abandoned projections.
        registry->get().ctx().emplace<std::shared_ptr<process::TaskScope>>(std::move(tasks));
        if (!registry->get().ctx().contains<lux::scene::WorldResidency>())
            return rejected(EProjectionError::INVALID_ENVIRONMENT);
        const auto paused = runtime.pauseSimulation(created->id());
        if (!paused)
            return rejected(paused.error());
        return std::move(*created);
    }
    struct SceneProjection::Impl final
    {
        lux::scene::SceneRuntime& runtime;
        std::shared_ptr<process::TaskScope> tasks;
        ProjectionEnvironment environment;
        ProjectionVersion version;
        SceneChangeCursor cursor;
        lux::scene::SceneInstanceLease instance;
        lux::scene::InstanceRetirement retiring;
        const std::thread::id owner{std::this_thread::get_id()};
        bool updating{}, closing{};
        std::size_t rebuilds{};
        Impl(
            lux::scene::SceneRuntime& host,
            std::shared_ptr<process::TaskScope> work,
            ProjectionEnvironment env,
            std::uint64_t configuration
        )
            : runtime(host), tasks(std::move(work)), environment(std::move(env))
        {
            version.configuration = configuration;
            version.environment = environment.version;
        }
        ProjectionResult<void> replace(SceneSnapshot capture)
        {
            auto package = buildSceneSnapshotPackage(capture);
            if (!package)
                return rejected(package.error());
            auto created = instantiateAuthorProjection(runtime, *package, environment, tasks);
            if (!created)
                return rejected(created.error());
            retiring = instance.retire();
            instance = std::move(*created);
            version.content = capture.content();
            cursor = capture.cursor();
            ++version.serial;
            ++rebuilds;
            return {};
        }
    };
    SceneProjection::SceneProjection(std::unique_ptr<Impl> value) : impl_(std::move(value)) {}
    SceneProjection::~SceneProjection() = default;
    lux::scene::SceneInstanceId SceneProjection::instance() const noexcept
    {
        return impl_->instance.id();
    }
    ProjectionVersion SceneProjection::version() const noexcept
    {
        return impl_->version;
    }
    std::size_t SceneProjection::rebuildCount() const noexcept
    {
        return impl_->rebuilds;
    }
    ProjectionResult<void> SceneProjection::update(const SceneSession& session)
    {
        auto& state = *impl_;
        if (state.owner != std::this_thread::get_id())
            return rejected(EProjectionError::WRONG_THREAD);
        if (state.updating || state.closing || !state.retiring.complete())
            return rejected(EProjectionError::BUSY);
        Admission admission(state.updating);
        auto changes = session.changesSince(state.cursor);
        if (!changes)
            return rejected(changes.error());
        const bool is_different_owner =
            changes->cursor.session != state.cursor.session || changes->cursor.history != state.cursor.history;
        if (is_different_owner)
            return rejected(EProjectionError::INVALID_SOURCE);
        if (changes->status == ESceneChanges::DELTA && changes->cursor == state.cursor)
            return {};
        auto frozen = session.capture();
        if (!frozen)
            return rejected(frozen.error());
        return state.replace(std::move(*frozen));
    }
    struct ScenePresentationHub::Impl final
    {
        lux::scene::SceneRuntime& runtime;
        std::shared_ptr<process::TaskScope> tasks;
        const std::size_t capacity;
        const std::thread::id owner{std::this_thread::get_id()};
        bool acquiring{};
        std::vector<std::shared_ptr<SceneProjection>> records;
        Impl(lux::scene::SceneRuntime& host, process::ExecutionRuntime& workers, std::size_t limit)
            : runtime(host), tasks(std::make_shared<process::TaskScope>(workers)), capacity(limit)
        {
            records.reserve(capacity);
        }
    };
    ScenePresentationHub::ScenePresentationHub(
        lux::scene::SceneRuntime& runtime,
        process::ExecutionRuntime& execution,
        std::size_t capacity
    )
        : impl_(std::make_unique<Impl>(runtime, execution, capacity))
    {}
    ScenePresentationHub::~ScenePresentationHub() = default;
    std::size_t ScenePresentationHub::size() const noexcept
    {
        return impl_->records.size();
    }
    void ScenePresentationHub::collectReleased() noexcept
    {
        if (impl_->owner != std::this_thread::get_id() || impl_->acquiring)
            return;
        Admission admission(impl_->acquiring);
        std::erase_if(impl_->records, [](auto& record) {
            if (record.use_count() != 1)
                return false;
            auto& state = *record->impl_;
            if (!state.retiring.complete())
                return false;
            if (!state.closing)
            {
                state.retiring = state.instance.retire();
                state.closing = true;
            }
            return state.retiring.complete();
        });
    }
    ProjectionResult<std::shared_ptr<SceneProjection>> ScenePresentationHub::acquire(
        const SceneSession& session,
        ProjectionEnvironment environment,
        std::uint64_t configuration
    )
    {
        if (impl_->owner != std::this_thread::get_id())
            return rejected(EProjectionError::WRONG_THREAD);
        if (impl_->acquiring)
            return rejected(EProjectionError::BUSY);
        collectReleased();
        Admission admission(impl_->acquiring);
        auto changes = session.changesSince({});
        if (!changes)
            return rejected(changes.error());
        for (const auto& record : impl_->records)
        {
            const auto& state = *record->impl_;
            const bool is_same_source =
                state.cursor.session == changes->cursor.session && state.cursor.history == changes->cursor.history;
            const bool is_same_configuration =
                state.version.configuration == configuration && state.version.environment == environment.version;
            if (!state.closing && is_same_source && is_same_configuration)
                return record;
        }
        if (impl_->records.size() >= impl_->capacity)
            return rejected(EProjectionError::CAPACITY);
        auto frozen = session.capture();
        if (!frozen)
            return rejected(frozen.error());
        auto state = std::make_unique<SceneProjection::Impl>(
            impl_->runtime,
            impl_->tasks,
            std::move(environment),
            configuration
        );
        auto built = state->replace(std::move(*frozen));
        if (!built)
            return lux::cxx::unexpected(built.error());
        auto record = std::shared_ptr<SceneProjection>(new SceneProjection(std::move(state)));
        impl_->records.push_back(record);
        return record;
    }
}
