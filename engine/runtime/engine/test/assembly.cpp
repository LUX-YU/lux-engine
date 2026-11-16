#include <lux/engine/EngineMetaFunctionRegistry.hpp>
#include <lux/engine/Init.hpp>
#include <lux/engine/SceneBuilder.hpp>
#include <lux/engine/SceneSystemRegistration.hpp>
#include <lux/engine/SimulationSystemRegistration.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationSystem.hpp>
#include <lux/engine/world/World.hpp>

#include <cassert>
#include <optional>
#include <vector>

namespace
{
    using namespace lux;
    using Result = engine::Engine::Result;

    const auto kInvalid = execution::toError(execution::EExecutionError::INVALID_ARGUMENT);
    constexpr error::Error kFactoryFailure{error::errorId("test.engine.assembly.factory")};
    constexpr error::Error kReleaseFailure{error::errorId("test.engine.assembly.release")};

    std::vector<int> destruction;
    std::vector<int> initialization;
    std::vector<int> retirement;
    bool reject_release = false;

    class SimulationProbe final : public simulation::SimulationSystem
    {
    public:
        explicit SimulationProbe(world::World& world) noexcept : world_(world) {}

        ~SimulationProbe() noexcept override
        {
            assert(world_.description().name == "assembly");
            destruction.push_back(0);
        }

        [[nodiscard]] simulation::SystemAccessSpec access() const noexcept override
        {
            return {};
        }

        void execute(const simulation::SimulationTime&) noexcept override {}

    private:
        world::World& world_;
    };

    class SceneProbe final : public scene::SceneSystem
    {
    public:
        SceneProbe(world::World& world, const ecs::ComponentSchemaSet& schemas, int id) noexcept
            : world_(world), schemas_(schemas), id_(id)
        {
        }

        ~SceneProbe() noexcept override
        {
            assert(world_.description().name == "assembly");
            assert(schemas_.find(cxx::make_basic_type_info<world::WorldObjectId>()));
            destruction.push_back(id_);
        }

        [[nodiscard]] Result execute() noexcept override
        {
            return {};
        }

    protected:
        [[nodiscard]] exec::task<Result> initialize() noexcept override
        {
            assert(schemas_.find(cxx::make_basic_type_info<world::WorldObjectId>()));
            initialization.push_back(id_);
            co_return Result{};
        }

        [[nodiscard]] exec::task<Result> releaseResources() noexcept override
        {
            retirement.push_back(id_);
            if (id_ == 1 && reject_release)
            {
                co_return cxx::unexpected(kReleaseFailure);
            }
            co_return Result{};
        }

    private:
        world::World& world_;
        const ecs::ComponentSchemaSet& schemas_;
        int id_;
    };

    template <class Sender> auto wait(execution::ExecutionRuntime& execution, Sender sender)
    {
        std::optional<typename execution::detail::ExecutionTypes<Sender>::Result> result;
        auto accepted = execution.spawn(stdexec::then(
            execution::asResult(stdexec::starts_on(execution.mainScheduler(), std::move(sender))),
            [&](auto completed) noexcept { result.emplace(std::move(completed)); }
        ));
        assert(accepted);
        assert(execution.waitUntil([&]() noexcept { return result.has_value(); }));
        return std::move(*result);
    }
}

int main()
{
    using namespace lux;
    constexpr std::size_t budget = 4 * 1024 * 1024;
    const execution::ExecutionRuntimeConfig execution_config{
        .cpu_concurrency = 2, .timer = {16}, .blocking = execution::BlockingSchedulerConfig{1, 32}
    };
#if defined(LUX_ENGINE_WITH_RENDER)
    render::RenderServerConfig render_config;
    render_config.channel_name = "lux.test.engine.assembly";
    auto context = engine::init(execution_config, {budget, budget, 32}, std::move(render_config), {});
#else
    auto context = engine::init(execution_config, {budget, budget, 32});
#endif
    assert(context);
    engine::Engine engine{**context};
    engine::EngineMetaFunctionRegistry metadata;
    for (const auto& schema : world::worldComponentSchemas())
    {
        assert(metadata.addComponent(schema));
    }
    assert(metadata.addSimulationSystem({
        .name = "test.simulation",
        .create = +[](engine::EngineContext&, world::World& world, std::span<const std::byte>) noexcept
            -> cxx::expected<std::unique_ptr<simulation::SimulationSystem>, error::Error>
        {
            return std::unique_ptr<simulation::SimulationSystem>{std::make_unique<SimulationProbe>(world)};
        }
    }));
    const auto codec = serialization::makePortableValueCodec<std::uint32_t>();
    assert(metadata.addSceneSystem({
        .name = "test.scene",
        .configuration_version = 1,
        .configuration = codec,
        .create = +[](engine::EngineContext&, world::World& world, simulation::Simulation& simulation,
                      const ecs::ComponentSchemaSet& schemas, std::span<const std::byte> bytes) noexcept
            -> cxx::expected<std::unique_ptr<scene::SceneSystem>, error::Error>
        {
            assert(simulation.groups().size() == 1 && simulation.groups().front().systems().size() == 1);
            const auto codec = serialization::makePortableValueCodec<std::uint32_t>();
            std::uint32_t id{};
            if (!codec.decode(bytes, &id))
            {
                return cxx::unexpected(kInvalid);
            }
            if (id != 1 && id != 2)
            {
                return cxx::unexpected(kFactoryFailure);
            }
            return std::unique_ptr<scene::SceneSystem>{std::make_unique<SceneProbe>(world, schemas, id)};
        }
    }));
    assert(!metadata.addSceneSystem({.name = "test.scene"}));

    scene::SceneDescription description;
    description.world.name = "assembly";
    description.simulation.groups.push_back({{}, {{"test.simulation", "simulation", 0, {}}}});
    const auto configured = [&](std::uint32_t id)
    {
        scene::SceneSystemDescription result{"test.scene", "scene", 1, {}};
        assert(codec.encode(&id, result.configuration));
        return result;
    };
    description.systems.push_back(configured(1));
    description.systems.push_back(configured(2));

    engine::Engine::SceneBuilder builder{engine, metadata};
    auto invalid = description;
    invalid.simulation.groups[0].systems[0].configuration = {std::byte{1}};
    auto rejected = builder.build(invalid);
    assert(!rejected && rejected.error() == kInvalid && destruction.empty());

    invalid = description;
    invalid.systems.back().type = "test.unknown";
    rejected = builder.build(invalid);
    assert(!rejected && rejected.error() == kInvalid && (destruction == std::vector{1, 0}));
    destruction.clear();

    invalid = description;
    ++invalid.systems.back().configuration_version;
    rejected = builder.build(invalid);
    assert(!rejected && rejected.error() == kInvalid && (destruction == std::vector{1, 0}));
    destruction.clear();

    invalid = description;
    invalid.systems.push_back(configured(3));
    rejected = builder.build(invalid);
    assert(!rejected && rejected.error() == kFactoryFailure && (destruction == std::vector{2, 1, 0}));
    assert(initialization.empty() && retirement.empty());
    destruction.clear();

    auto scene = builder.build(description);
    assert(scene && (*scene)->systems().size() == 2 && initialization.empty());
    auto id = engine.addScene(*scene, true);
    assert(id && !*scene);
    auto& execution = (*context)->execution();
    assert(wait(execution, engine.whenSceneInitialized(*id)));
    assert((initialization == std::vector{1, 2}));

    reject_release = true;
    const auto failed_release = wait(execution, engine.removeScene(*id));
    assert(!failed_release && (destruction == std::vector{2}) && (retirement == std::vector{2, 1}));
    reject_release = false;
    assert(wait(execution, engine.removeScene(*id)));
    assert((destruction == std::vector{2, 1, 0}) && (retirement == std::vector{2, 1, 1}));
}
