#include <lux/engine/simulation/EventPoint.hpp>
#include <lux/engine/simulation/HookPoint.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationStepInfo.hpp>
#include <lux/engine/simulation/ecs/EcsCommandBuffer.hpp>
#include <lux/engine/simulation/ecs/EcsSnapshot.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/systems/ScriptSystem.hpp>
#include <lux/engine/function/script/abi/lux_script_abi.h>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <entt/signal/sigh.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef LUX_BENCHMARK_GIT_COMMIT
#define LUX_BENCHMARK_GIT_COMMIT "unknown"
#endif
#ifndef LUX_BENCHMARK_BUILD_TYPE
#define LUX_BENCHMARK_BUILD_TYPE "unknown"
#endif

namespace
{
    using Clock = std::chrono::steady_clock;
    using namespace lux::simulation;
    using namespace lux::simulation::ecs;
    using namespace lux::simulation::script;

    std::atomic_size_t g_allocation_count{};
    std::atomic_bool g_count_allocations{};

    struct Options final
    {
        std::string group{"task-graph"};
        std::string mode{"diagnostic"};
        std::size_t size{1024U};
        std::filesystem::path output{"ecs_l1_benchmark.csv"};
        std::size_t warmups{1U};
        std::size_t samples{3U};
    };

    [[nodiscard]] std::optional<Options> parseOptions(int argc, char** argv)
    {
        Options result;
        for (int index{1}; index < argc; ++index)
        {
            if (index + 1 >= argc)
                return std::nullopt;
            const std::string_view key{argv[index++]};
            const std::string_view value{argv[index]};
            if (key == "--group")
                result.group = value;
            else if (key == "--mode")
                result.mode = value;
            else if (key == "--output")
                result.output = value;
            else if (key == "--size")
            {
                const auto parsed = std::from_chars(
                    value.data(),
                    value.data() + value.size(),
                    result.size
                );
                if (parsed.ec != std::errc{} ||
                    parsed.ptr != value.data() + value.size() ||
                    result.size == 0U ||
                    result.size > std::numeric_limits<std::uint32_t>::max())
                    return std::nullopt;
            }
            else
                return std::nullopt;
        }
        if (result.mode == "performance")
        {
            result.warmups = 5U;
            result.samples = 30U;
        }
        constexpr std::array groups{
            std::string_view{"task-graph"},
            std::string_view{"world"},
            std::string_view{"command-buffer"},
            std::string_view{"reactive-dirty"},
            std::string_view{"cpp-method-prepared"},
            std::string_view{"hook-global-multi"},
            std::string_view{"hook-entity-multi"},
            std::string_view{"global-event"},
            std::string_view{"entity-targeted-event-sparse"},
            std::string_view{"owned-worker-event-buffer"},
            std::string_view{"script-detach"},
            std::string_view{"script-dirty"},
            std::string_view{"script-prepare-scaling"},
            std::string_view{"ecs-snapshot"}};
        if (std::find(groups.begin(), groups.end(), result.group) ==
            groups.end())
            return std::nullopt;
        return result;
    }

    struct Observation final
    {
        std::size_t updates{}, notifications{}, callbacks{};
        std::size_t asset_resolution_delta{}, target_resolution_delta{};
        std::size_t entities_examined{}, target_range_lookups{};
        std::size_t handlers_visited{}, target_ranges_built{};
        std::size_t dispatch_handlers_built{}, instance_creates{};
        std::size_t method_prepares{}, frame_builds{};
        std::size_t bindings_removed{}, dirty_marks{};
    };

    struct Sample final
    {
        std::size_t index{};
        std::uint64_t nanoseconds{};
        std::size_t allocations{};
        Observation observation;
    };

    template <class Setup, class Operation>
    [[nodiscard]] std::vector<Sample> measure(
        const Options& options,
        Setup&& setup,
        Operation&& operation
    )
    {
        for (std::size_t index{}; index < options.warmups; ++index)
        {
            auto state = setup();
            static_cast<void>(operation(*state));
        }
        std::vector<Sample> samples;
        samples.reserve(options.samples);
        for (std::size_t index{}; index < options.samples; ++index)
        {
            auto state = setup();
            g_allocation_count.store(0U, std::memory_order_relaxed);
            g_count_allocations.store(true, std::memory_order_release);
            const auto begin = Clock::now();
            const auto observation = operation(*state);
            const auto end = Clock::now();
            g_count_allocations.store(false, std::memory_order_release);
            samples.push_back({
                index,
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        end - begin).count()),
                g_allocation_count.load(std::memory_order_relaxed),
                observation});
        }
        return samples;
    }

    void writeCsv(
        const Options& options,
        std::string_view metric,
        const std::vector<Sample>& samples
    )
    {
        if (!options.output.parent_path().empty())
            std::filesystem::create_directories(options.output.parent_path());
        auto temporary = options.output;
        temporary += ".tmp";
        std::ofstream output(temporary, std::ios::trunc);
        if (!output)
            throw std::runtime_error("cannot open benchmark output");
        output << "benchmark_schema_version,git_commit,build_type,group,"
                  "metric,size,sample,nanoseconds,allocations,updates,"
                  "notifications,callbacks,asset_resolution_delta,"
                  "target_resolution_delta,entities_examined,"
                  "target_range_lookups,handlers_visited,target_ranges_built,"
                  "dispatch_handlers_built,instance_creates,method_prepares,"
                  "frame_builds,bindings_removed,dirty_marks\n";
        for (const auto& sample : samples)
        {
            const auto& value = sample.observation;
            output << "11," << LUX_BENCHMARK_GIT_COMMIT << ','
                   << LUX_BENCHMARK_BUILD_TYPE << ',' << options.group << ','
                   << metric << ',' << options.size << ',' << sample.index
                   << ',' << sample.nanoseconds << ',' << sample.allocations
                   << ',' << value.updates << ',' << value.notifications
                   << ',' << value.callbacks << ','
                   << value.asset_resolution_delta << ','
                   << value.target_resolution_delta << ','
                   << value.entities_examined << ','
                   << value.target_range_lookups << ','
                   << value.handlers_visited << ','
                   << value.target_ranges_built << ','
                   << value.dispatch_handlers_built << ','
                   << value.instance_creates << ',' << value.method_prepares
                   << ',' << value.frame_builds << ','
                   << value.bindings_removed << ',' << value.dirty_marks
                   << '\n';
        }
        output.close();
        std::error_code error;
        std::filesystem::remove(options.output, error);
        std::filesystem::rename(temporary, options.output);
    }

    struct Position final { std::uint32_t value{}; };

    struct WorldState
    {
        explicit WorldState(std::size_t count)
        {
            entities.reserve(count);
            registry.storage<Position>().reserve(count);
            for (std::size_t index{}; index < count; ++index)
            {
                const auto entity = registry.create();
                registry.emplace<Position>(entity);
                entities.push_back(entity);
            }
        }
        Registry registry;
        std::vector<Entity> entities;
    };

    struct ReactiveState final : WorldState
    {
        explicit ReactiveState(std::size_t count) : WorldState(count)
        {
            dirty.reserve(count);
            connection = registry.on_update<Position>()
                .connect<&ReactiveState::updated>(*this);
        }
        void updated(Registry&, Entity value) noexcept { dirty.push_back(value); }
        std::vector<Entity> dirty;
        entt::scoped_connection connection;
    };

    struct CommandState final : WorldState
    {
        explicit CommandState(std::size_t count) : WorldState(count)
        {
            const EcsCommandProducerCapacity capacity{count, 0U};
            if (!commands.prepare(std::span{&capacity, 1U}))
                throw std::runtime_error("command prepare failed");
        }
        EcsCommandBuffer commands;
    };

    struct HookState final
    {
        static void increment(void* value) noexcept
        {
            ++*static_cast<std::size_t*>(value);
        }

        explicit HookState(std::size_t count)
        {
            counters.resize(count);
            tokens.reserve(count);
            if (hook.prepare(count) != EEndpointMutationError::NONE)
                throw std::runtime_error("hook prepare failed");
            for (auto& counter : counters)
            {
                const auto connected = hook.connect(&counter, &increment);
                if (!connected)
                    throw std::runtime_error("hook connect failed");
                tokens.push_back(connected.token);
            }
        }
        HookPoint<void()> hook;
        std::vector<std::size_t> counters;
        std::vector<EndpointConnectionToken> tokens;
    };

    struct BroadcastState final
    {
        explicit BroadcastState(std::size_t count)
        {
            if (event.prepare(1U, count, 1U) !=
                EEndpointMutationError::NONE ||
                !event.connect(&callbacks, [](void* value, const auto&) noexcept
                {
                    ++*static_cast<std::size_t*>(value);
                }))
                throw std::runtime_error("event prepare failed");
            auto writer = event.begin(0U);
            for (std::size_t index{}; index < count; ++index)
            {
                if (!writer.record(index))
                    throw std::runtime_error("event record failed");
            }
        }
        EventPoint<SimulationBroadcastRoute, std::size_t> event;
        std::size_t callbacks{};
    };

    struct SparseState final
    {
        static void increment(
            void* opaque,
            const Entity&,
            const std::size_t&
        ) noexcept
        {
            ++*static_cast<std::size_t*>(opaque);
        }

        explicit SparseState(std::size_t subscriber_count)
            : occurrence_count((std::min)(
                  subscriber_count,
                  std::size_t{100000U}
              ))
        {
            entities.reserve(subscriber_count);
            if (event.prepare(
                    1U,
                    occurrence_count,
                    subscriber_count + 1U
                ) != EEndpointMutationError::NONE)
            {
                throw std::runtime_error("targeted event prepare failed");
            }
            if (!event.connectAll(&callbacks, &increment))
                throw std::runtime_error("connect-all failed");
            for (std::size_t index{}; index < subscriber_count; ++index)
            {
                const auto entity = registry.create();
                entities.push_back(entity);
                if (!event.connect(entity, &callbacks, &increment))
                    throw std::runtime_error("target connect failed");
            }
            auto writer = event.begin(0U);
            for (std::size_t index{}; index < occurrence_count; ++index)
            {
                if (!writer.record(
                        entities[index % entities.size()],
                        index
                    ))
                {
                    throw std::runtime_error("target record failed");
                }
            }
        }
        Registry registry;
        std::vector<Entity> entities;
        EventPoint<EntityTargetedRoute<Entity>, std::size_t> event;
        std::size_t occurrence_count{};
        std::size_t callbacks{};
    };

    struct DetachState final
    {
        static void ignore(void*) noexcept
        {
        }

        explicit DetachState(std::size_t binding_count)
        {
            if (binding_count < target_tokens.size() ||
                hook.prepare(binding_count) != EEndpointMutationError::NONE)
            {
                throw std::runtime_error("detach prepare failed");
            }
            const auto target_begin = binding_count - target_tokens.size();
            for (std::size_t index{}; index < binding_count; ++index)
            {
                const auto connected = hook.connect(nullptr, &ignore);
                if (!connected)
                    throw std::runtime_error("detach connect failed");
                if (index >= target_begin)
                    target_tokens[index - target_begin] = connected.token;
            }
        }

        HookPoint<void()> hook;
        std::array<EndpointConnectionToken, 4U> target_tokens;
    };

    struct ScriptDirtyMarker final
    {
    };

    struct ScriptDirtyState final
    {
        explicit ScriptDirtyState(std::size_t count)
        {
            entities.reserve(count);
            registry.storage<ScriptDirtyMarker>().reserve(count);
            for (std::size_t index{}; index < count; ++index)
                entities.push_back(registry.create());
            for (const auto entity : entities)
                registry.emplace<ScriptDirtyMarker>(entity);
            registry.clear<ScriptDirtyMarker>();
        }

        Registry registry;
        std::vector<Entity> entities;
    };

    struct WorkerEventState final
    {
        explicit WorkerEventState(std::size_t count)
        {
            if (event.prepare(4U, (count + 3U) / 4U, 1U) !=
                EEndpointMutationError::NONE ||
                !event.connect(&callbacks, [](void* value, const auto&) noexcept
                {
                    ++*static_cast<std::size_t*>(value);
                }))
                throw std::runtime_error("worker event prepare failed");
            lux::task::TaskGraphBuilder builder;
            for (std::size_t producer{}; producer < 4U; ++producer)
            {
                const auto first = count * producer / 4U;
                const auto last = count * (producer + 1U) / 4U;
                if (!builder.add([this, producer, first, last]() noexcept
                    {
                        auto writer = event.begin(producer);
                        for (std::size_t value{first}; value < last; ++value)
                        {
                            if (!writer.record(value))
                                failed.store(true, std::memory_order_relaxed);
                        }
                    }))
                    throw std::runtime_error("worker task add failed");
            }
            auto built = std::move(builder).build();
            if (!built)
                throw std::runtime_error("worker graph build failed");
            graph = std::move(*built);
            auto created = lux::task::TaskExecutor::create(
                lux::task::TaskExecutorConfig{4U, graph.taskCount()}
            );
            if (!created)
            {
                throw std::runtime_error("worker executor creation failed");
            }
            executor = std::make_unique<lux::task::TaskExecutor>(std::move(*created));
        }
        EventPoint<SimulationBroadcastRoute, std::size_t> event;
        std::size_t callbacks{};
        std::atomic_bool failed{};
        lux::task::TaskGraph graph;
        std::unique_ptr<lux::task::TaskExecutor> executor;
    };

    inline constexpr SystemInstanceId kPrepareSystem{0x7101U};
    inline constexpr HookPointId kPrepareHook{0x7102U};
    inline constexpr lux::script::ScriptSymbolId kPrepareSymbol{0x7103U};

    [[nodiscard]] lux::asset::AssetId prepareAssetId() noexcept
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.front() = 0x71U;
        return lux::asset::AssetId{bytes};
    }

    [[nodiscard]] SimulationDescription makePrepareSimulation()
    {
        constexpr std::array hooks{
            makeHookPointSpec<void(const SimulationStepInfo&)>(kPrepareHook, "prepare")
        };
        const SystemDescription system{
            .canonical_name = "lux.benchmark.ScriptPrepare",
            .version = 1U,
            .hooks = hooks,
            .events = {}
        };
        SimulationDescriptionBuilder builder;
        auto added = builder.addSystem(kPrepareSystem, "prepare", system);
        if (!added)
            throw std::runtime_error("prepare benchmark system rejected");
        auto built = std::move(builder).build();
        if (!built)
            throw std::runtime_error("prepare benchmark simulation rejected");
        return std::move(*built);
    }

    [[nodiscard]] lux::script::ScriptArtifact makePrepareArtifact()
    {
        const auto argument = lux::rdesc::makeScriptValueType<SimulationStepInfo>(
            lux::semantic::EValuePass::CONST_REF
        );
        lux::rdesc::Script description;
        description.module_name = "lux.benchmark.script.prepare";
        description.exports = {{"prepare", kPrepareSymbol, {argument}, {}}};
        description.body = lux::rdesc::CppStaticScript{"benchmark"};
        auto artifact = lux::script::ScriptArtifact::create(std::move(description), {});
        if (!artifact)
            throw std::runtime_error("prepare benchmark artifact rejected");
        return std::move(*artifact);
    }

    struct PrepareBackendState final
    {
        std::size_t instance_creates{};
        std::size_t instance_destroys{};
        std::size_t method_prepares{};
        std::size_t method_releases{};
    };

    int prepareInvoke(lux_script_call_frame*) noexcept
    {
        return 0;
    }

    EScriptBackendResult createPrepareInstance(
        void* context,
        const ScriptInstanceCreateContext&,
        const lux::script::ScriptArtifact&,
        ScriptBackendInstance& output
    ) noexcept
    {
        auto* state = static_cast<PrepareBackendState*>(context);
        auto* instance = new (std::nothrow) std::byte{};
        if (!instance)
            return EScriptBackendResult::ALLOCATION_FAILURE;
        ++state->instance_creates;
        output.value = instance;
        return EScriptBackendResult::SUCCESS;
    }

    EScriptBackendResult prepareBenchmarkMethod(
        void* context,
        ScriptBackendInstance instance,
        const lux::rdesc::ScriptFunction&,
        lux::script::BoundScriptCall& output
    ) noexcept
    {
        ++static_cast<PrepareBackendState*>(context)->method_prepares;
        output = {&prepareInvoke, instance.value};
        return EScriptBackendResult::SUCCESS;
    }

    void releaseBenchmarkMethod(void* context, ScriptBackendInstance, lux::script::BoundScriptCall) noexcept
    {
        ++static_cast<PrepareBackendState*>(context)->method_releases;
    }

    void destroyPrepareInstance(void* context, ScriptBackendInstance instance) noexcept
    {
        ++static_cast<PrepareBackendState*>(context)->instance_destroys;
        delete static_cast<std::byte*>(instance.value);
    }

    struct PrepareScalingState final
    {
        explicit PrepareScalingState(std::size_t count)
            : simulation(makePrepareSimulation()), artifact(makePrepareArtifact()), asset_id(prepareAssetId())
        {
            ScriptSystemDescriptionBuilder builder;
            for (std::size_t index{}; index < count; ++index)
            {
                ScriptMountDescription mount{
                    ScriptMountId{index + 1U},
                    asset_id,
                    SimulationScriptMount{},
                    true,
                    {{kPrepareSymbol, HookScriptTarget{kPrepareSystem, kPrepareHook}}}
                };
                if (!builder.addMount(std::move(mount)))
                    throw std::runtime_error("prepare benchmark mount rejected");
            }
            auto built = std::move(builder).build(simulation);
            if (!built)
                throw std::runtime_error("prepare benchmark description rejected");
            description = std::move(*built);

            if (hook.prepare(1U) != EEndpointMutationError::NONE)
                throw std::runtime_error("prepare benchmark hook rejected");
            bridge = std::make_unique<ScriptHookEndpoint<void(const SimulationStepInfo&)>>(
                kPrepareSystem,
                kPrepareHook,
                hook
            );
            hook_descriptor = bridge->descriptor();
            backend_descriptor = {
                lux::rdesc::Script::Kind::CPP_STATIC,
                &backend,
                &createPrepareInstance,
                &prepareBenchmarkMethod,
                &releaseBenchmarkMethod,
                &destroyPrepareInstance
            };
            auto created = ScriptSystem::create(
                simulation,
                description,
                registry,
                1U,
                {this, &resolveArtifact},
                {},
                std::span{&backend_descriptor, 1U},
                std::span{&hook_descriptor, 1U},
                {}
            );
            if (!created)
                throw std::runtime_error("prepare benchmark ScriptSystem create failed");
            system = std::make_unique<ScriptSystem>(std::move(*created));
        }

        static bool resolveArtifact(
            void* context,
            const lux::asset::AssetId& asset,
            ResolvedScriptArtifact& output
        ) noexcept
        {
            auto* state = static_cast<PrepareScalingState*>(context);
            if (asset != state->asset_id)
                return false;
            ++state->asset_resolutions;
            output = {
                &state->artifact,
                state,
                [](void* value) noexcept
                {
                    ++static_cast<PrepareScalingState*>(value)->asset_releases;
                }
            };
            return true;
        }

        SimulationDescription simulation;
        lux::script::ScriptArtifact artifact;
        lux::asset::AssetId asset_id;
        ScriptSystemDescription description;
        ecs::Registry registry;
        HookPoint<void(const SimulationStepInfo&)> hook;
        std::unique_ptr<ScriptHookEndpoint<void(const SimulationStepInfo&)>> bridge;
        ScriptHookEndpointDescriptor hook_descriptor;
        PrepareBackendState backend;
        ScriptBackendDescriptor backend_descriptor;
        std::unique_ptr<ScriptSystem> system;
        std::size_t asset_resolutions{};
        std::size_t asset_releases{};
    };

    struct TaskState final
    {
        explicit TaskState(std::size_t count)
        {
            lux::task::TaskGraphBuilder builder;
            for (std::size_t index{}; index < count; ++index)
            {
                if (!builder.add([this]() noexcept { ++calls; }))
                    throw std::runtime_error("task add failed");
            }
            auto built = std::move(builder).build();
            if (!built)
                throw std::runtime_error("task graph build failed");
            graph = std::move(*built);
            auto created = lux::task::TaskExecutor::create(
                lux::task::TaskExecutorConfig{0U, graph.taskCount()}
            );
            if (!created)
            {
                throw std::runtime_error("task executor creation failed");
            }
            executor = std::make_unique<lux::task::TaskExecutor>(std::move(*created));
        }
        std::atomic_size_t calls{};
        lux::task::TaskGraph graph;
        std::unique_ptr<lux::task::TaskExecutor> executor;
    };

    struct SnapshotState final
    {
        explicit SnapshotState(std::size_t count)
        {
            const auto source = transformComponentSchemas();
            auto schemas_value = ComponentSchemaSet::build(
                std::vector<ComponentSchema>(source.begin(), source.end()));
            if (!schemas_value)
                throw std::runtime_error("schema build failed");
            schemas = std::move(*schemas_value);
            const std::array contributions{
                transformComponentSnapshotContribution()};
            auto component_value = ComponentSnapshotSet::build(
                schemas,
                contributions);
            if (!component_value)
                throw std::runtime_error("component build failed");
            components = std::move(*component_value);
            registry.storage<Transform3D>().reserve(count);
            for (std::size_t index{}; index < count; ++index)
                registry.emplace<Transform3D>(registry.create());
        }
        Registry registry;
        ComponentSchemaSet schemas;
        ComponentSnapshotSet components;
    };

    int preparedInvoke(lux_script_call_frame* frame)
    {
        ++*static_cast<std::size_t*>(frame->user_context);
        return 0;
    }
}

int main(int argc, char** argv)
{
    const auto options = parseOptions(argc, argv);
    if (!options)
        return 2;
    std::string_view metric;
    std::vector<Sample> samples;
    if (options->group == "world")
    {
        metric = "world_patch";
        samples = measure(*options,
            [&] { return std::make_unique<WorldState>(options->size); },
            [](WorldState& state)
            {
                for (const auto entity : state.entities)
                    state.registry.patch<Position>(entity);
                return Observation{.updates = state.entities.size()};
            });
    }
    else if (options->group == "reactive-dirty")
    {
        metric = "reactive_dirty_patch";
        samples = measure(*options,
            [&] { return std::make_unique<ReactiveState>(options->size); },
            [](ReactiveState& state)
            {
                for (const auto entity : state.entities)
                    state.registry.patch<Position>(entity);
                return Observation{
                    .updates = state.entities.size(),
                    .notifications = state.dirty.size()};
            });
    }
    else if (options->group == "command-buffer")
    {
        metric = "command_buffer_record";
        samples = measure(*options,
            [&] { return std::make_unique<CommandState>(options->size); },
            [](CommandState& state)
            {
                auto begun = state.commands.begin(0U);
                if (!begun)
                    throw std::runtime_error("command begin failed");
                auto writer = std::move(*begun);
                for (const auto entity : state.entities)
                    if (!writer.remove<Position>(entity))
                        throw std::runtime_error("command record failed");
                return Observation{.updates = state.entities.size()};
            });
    }
    else if (options->group == "cpp-method-prepared")
    {
        metric = "cpp_method_prepared";
        samples = measure(*options,
            [] { return std::make_unique<std::size_t>(0U); },
            [&](std::size_t& calls)
            {
                lux_script_call_frame frame{};
                frame.user_context = &calls;
                for (std::size_t index{}; index < options->size; ++index)
                    preparedInvoke(&frame);
                return Observation{
                    .callbacks = calls,
                    .handlers_visited = calls,
                    .method_prepares = 1U,
                    .frame_builds = options->size};
            });
    }
    else if (options->group == "hook-global-multi" ||
            options->group == "hook-entity-multi")
    {
        metric = "hook_dense_dispatch";
        samples = measure(*options,
            [&] { return std::make_unique<HookState>(options->size); },
            [](HookState& state)
            {
                const auto mutation_count = (std::min)(
                    state.tokens.size(),
                    std::size_t{1024U}
                );
                std::uint64_t random{0x9E3779B97F4A7C15ULL};
                for (std::size_t mutation{}; mutation < mutation_count;
                     ++mutation)
                {
                    random = random * 6364136223846793005ULL + 1U;
                    const auto index = static_cast<std::size_t>(
                        random % state.tokens.size()
                    );
                    if (state.hook.disconnect(state.tokens[index]) !=
                        EEndpointMutationError::NONE)
                    {
                        throw std::runtime_error("hook disconnect failed");
                    }
                    const auto connected = state.hook.connect(
                        std::addressof(state.counters[index]),
                        &HookState::increment
                    );
                    if (!connected)
                        throw std::runtime_error("hook reconnect failed");
                    state.tokens[index] = connected.token;
                }
                const auto calls = state.hook.dispatch();
                return Observation{
                    .updates = mutation_count * 2U,
                    .callbacks = calls,
                    .handlers_visited = calls,
                    .dispatch_handlers_built = state.counters.size(),
                    .frame_builds = 1U};
            });
    }
    else if (options->group == "global-event")
    {
        metric = "global_event";
        samples = measure(*options,
            [&] { return std::make_unique<BroadcastState>(options->size); },
            [](BroadcastState& state)
            {
                const auto calls = state.event.drain();
                return Observation{
                    .notifications = calls,
                    .callbacks = state.callbacks,
                    .handlers_visited = calls,
                    .frame_builds = calls};
            });
    }
    else if (options->group == "entity-targeted-event-sparse")
    {
        metric = "entity_targeted_event_sparse";
        samples = measure(*options,
            [&] { return std::make_unique<SparseState>(options->size); },
            [](SparseState& state)
            {
                const auto calls = state.event.drain();
                return Observation{
                    .notifications = state.occurrence_count,
                    .callbacks = state.callbacks,
                    .entities_examined = state.occurrence_count,
                    .target_range_lookups = state.occurrence_count,
                    .handlers_visited = calls,
                    .target_ranges_built = state.event.targetBucketCount(),
                    .dispatch_handlers_built = state.event.handlerCount(),
                    .frame_builds = state.occurrence_count};
            });
    }
    else if (options->group == "owned-worker-event-buffer")
    {
        metric = "owned_worker_event_buffer";
        samples = measure(*options,
            [&] { return std::make_unique<WorkerEventState>(options->size); },
            [&](WorkerEventState& state)
            {
                if (!state.executor->execute(state.graph) || state.failed.load())
                    throw std::runtime_error("worker production failed");
                const auto calls = state.event.drain();
                return Observation{
                    .updates = options->size,
                    .notifications = calls,
                    .callbacks = state.callbacks,
                    .frame_builds = calls};
            });
    }
    else if (options->group == "task-graph")
    {
        metric = "task_graph_execute";
        samples = measure(*options,
            [&] { return std::make_unique<TaskState>(options->size); },
            [](TaskState& state)
            {
                if (!state.executor->execute(state.graph))
                    throw std::runtime_error("task execution failed");
                return Observation{.callbacks = state.calls.load()};
            });
    }
    else if (options->group == "script-detach")
    {
        metric = "script_mount_detach";
        samples = measure(*options,
            [&] { return std::make_unique<DetachState>(options->size); },
            [](DetachState& state)
            {
                const auto before = state.hook.handlerCount();
                for (const auto token : state.target_tokens)
                {
                    if (state.hook.disconnect(token) !=
                        EEndpointMutationError::NONE)
                    {
                        throw std::runtime_error("detach failed");
                    }
                }
                const auto removed = before - state.hook.handlerCount();
                return Observation{
                    .updates = removed,
                    .bindings_removed = removed};
            });
    }
    else if (options->group == "script-dirty")
    {
        metric = "script_dirty_distinct";
        samples = measure(*options,
            [&] { return std::make_unique<ScriptDirtyState>(options->size); },
            [](ScriptDirtyState& state)
            {
                for (const auto entity : state.entities)
                    state.registry.emplace<ScriptDirtyMarker>(entity);
                const auto count =
                    state.registry.storage<ScriptDirtyMarker>().size();
                return Observation{
                    .updates = state.entities.size(),
                    .notifications = count,
                    .dirty_marks = count};
            });
    }
    else if (options->group == "script-prepare-scaling")
    {
        metric = "script_prepare_linear";
        samples = measure(*options,
            [&] { return std::make_unique<PrepareScalingState>(options->size); },
            [&](PrepareScalingState& state)
            {
                if (!state.system->prepare())
                    throw std::runtime_error("script prepare failed");
                const auto active_instances = state.system->activeInstanceCount();
                const auto lane_connections = state.hook.handlerCount();
                if (!state.system->shutdown())
                    throw std::runtime_error("script prepare shutdown failed");
                return Observation{
                    .updates = options->size,
                    .callbacks = active_instances,
                    .asset_resolution_delta = state.asset_resolutions,
                    .target_resolution_delta = lane_connections,
                    .dispatch_handlers_built = lane_connections,
                    .instance_creates = state.backend.instance_creates,
                    .method_prepares = state.backend.method_prepares
                };
            });
    }
    else
    {
        metric = "ecs_snapshot_capture";
        samples = measure(*options,
            [&] { return std::make_unique<SnapshotState>(options->size); },
            [](SnapshotState& state)
            {
                const auto snapshot = EcsSnapshot::capture(
                    state.registry,
                    state.components);
                if (!snapshot)
                    throw std::runtime_error("snapshot capture failed");
                return Observation{
                    .updates = state.registry.storage<Transform3D>().size()};
            });
    }
    writeCsv(*options, metric, samples);
    return 0;
}

void* operator new(std::size_t size)
{
    if (g_count_allocations.load(std::memory_order_relaxed))
        g_allocation_count.fetch_add(1U, std::memory_order_relaxed);
    if (void* result = std::malloc(size == 0U ? 1U : size))
        return result;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept
{
    ::operator delete(value);
}
