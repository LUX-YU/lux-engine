#include <lux/engine/simulation/ecs/EcsCommandBuffer.hpp>
#include <lux/engine/simulation/ecs/EcsSnapshot.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ScriptBindingSession.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <entt/signal/sigh.hpp>

#include <atomic>
#include <array>
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
    using namespace lux::simulation::ecs;

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
        for (int index = 1; index < argc; ++index)
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
                {
                    return std::nullopt;
                }
            }
            else
                return std::nullopt;
        }
        if (result.mode == "performance")
        {
            result.warmups = 5U;
            result.samples = 30U;
        }
        if (result.group != "task-graph" && result.group != "world" &&
            result.group != "command-buffer" &&
            result.group != "reactive-dirty" &&
            result.group != "bound-call-native" &&
            result.group != "hook-multi" &&
            result.group != "global-event" &&
            result.group != "entity-targeted-event" &&
            result.group != "ecs-snapshot")
        {
            return std::nullopt;
        }
        return result;
    }

    struct Observation final
    {
        std::size_t updates{};
        std::size_t notifications{};
        std::size_t callbacks{};
        std::size_t reflection_lookups{};
        std::size_t string_lookups{};
        std::size_t asset_lookups{};
        std::size_t scene_scans{};
    };

    struct Sample final
    {
        std::size_t index{};
        std::uint64_t nanoseconds{};
        std::size_t allocations{};
        Observation observation;
    };

    template <class Setup, class Operation, class Teardown>
    [[nodiscard]] std::vector<Sample> measure(
        const Options& options,
        Setup&& setup,
        Operation&& operation,
        Teardown&& teardown
    )
    {
        for (std::size_t index{}; index < options.warmups; ++index)
        {
            auto state = setup();
            (void)operation(*state);
            teardown(*state);
        }

        std::vector<Sample> result;
        result.reserve(options.samples);
        for (std::size_t index{}; index < options.samples; ++index)
        {
            auto state = setup();
            g_allocation_count.store(0U, std::memory_order_relaxed);
            g_count_allocations.store(true, std::memory_order_release);
            const auto begin = Clock::now();
            const Observation observation = operation(*state);
            const auto end = Clock::now();
            g_count_allocations.store(false, std::memory_order_release);
            teardown(*state);
            result.push_back(Sample{
                index,
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        end - begin
                    ).count()
                ),
                g_allocation_count.load(std::memory_order_relaxed),
                observation});
        }
        return result;
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
        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output)
                throw std::runtime_error("cannot open benchmark output");
            output << "benchmark_schema_version,git_commit,build_type,group,"
                      "metric,size,sample,nanoseconds,allocations,updates,"
                      "notifications,callbacks,reflection_lookups,string_lookups,"
                      "asset_lookups,scene_scans\n";
            for (const auto& sample : samples)
            {
                output << "6," << LUX_BENCHMARK_GIT_COMMIT << ','
                       << LUX_BENCHMARK_BUILD_TYPE << ',' << options.group
                       << ',' << metric << ',' << options.size << ','
                       << sample.index << ',' << sample.nanoseconds << ','
                       << sample.allocations << ','
                       << sample.observation.updates << ','
                       << sample.observation.notifications << ','
                       << sample.observation.callbacks << ','
                       << sample.observation.reflection_lookups << ','
                       << sample.observation.string_lookups << ','
                       << sample.observation.asset_lookups << ','
                       << sample.observation.scene_scans << '\n';
            }
        }
        std::error_code error;
        std::filesystem::remove(options.output, error);
        std::filesystem::rename(temporary, options.output);
    }

    struct Position final
    {
        std::uint32_t value{};
    };

    struct WorldState final
    {
        explicit WorldState(std::size_t count)
        {
            entities.reserve(count);
            registry.storage<Position>().reserve(count);
            for (std::size_t index{}; index < count; ++index)
            {
                const Entity entity = registry.create();
                registry.emplace<Position>(
                    entity,
                    Position{static_cast<std::uint32_t>(index)}
                );
                entities.push_back(entity);
            }
        }

        Registry registry;
        std::vector<Entity> entities;
    };

    struct ReactiveDirtyState final
    {
        explicit ReactiveDirtyState(std::size_t count)
        {
            entities.reserve(count);
            dirty.reserve(count);
            registry.storage<Position>().reserve(count);
            for (std::size_t index{}; index < count; ++index)
            {
                const Entity entity = registry.create();
                registry.emplace<Position>(entity);
                entities.push_back(entity);
            }
            connection = registry.on_update<Position>().connect<
                &ReactiveDirtyState::onUpdate>(*this);
        }

        void onUpdate(Registry&, Entity entity) noexcept
        {
            dirty.push_back(entity);
        }

        Registry registry;
        std::vector<Entity> entities;
        std::vector<Entity> dirty;
        entt::scoped_connection connection;
    };

    struct CommandState final
    {
        explicit CommandState(std::size_t count)
        {
            entities.reserve(count);
            registry.storage<Position>().reserve(count);
            for (std::size_t index{}; index < count; ++index)
            {
                const Entity entity = registry.create();
                registry.emplace<Position>(entity);
                entities.push_back(entity);
            }
            const EcsCommandProducerCapacity capacity{count, 0U};
            if (!commands.prepare(std::span{&capacity, 1U}))
                throw std::runtime_error("command prepare failed");
        }

        Registry registry;
        std::vector<Entity> entities;
        EcsCommandBuffer commands;
    };

    inline constexpr std::array kBindingHooks{
        lux::simulation::makeSystemHookPoint<void()>("update")};
    inline constexpr std::array kBindingEvents{
        lux::simulation::makeSystemEvent<void>(
            "global-event",
            kBindingHooks[0],
            lux::simulation::ESystemEventTarget::GLOBAL,
            {},
            0U
        ),
        lux::simulation::makeSystemEvent<void>(
            "entity-event",
            kBindingHooks[0],
            lux::simulation::ESystemEventTarget::ENTITY_TARGETED,
            {},
            0U
        )};
    inline constexpr lux::simulation::SystemDescription kBindingSystem{
        .canonical_name = "lux.benchmark.binding",
        .version = 1U,
        .hooks = kBindingHooks,
        .events = kBindingEvents};

    struct BindingBenchmarkState final
    {
        BindingBenchmarkState(std::size_t count, std::string_view kind)
            : kind(kind)
        {
            std::array<std::uint8_t, 16U> id_bytes{};
            id_bytes[0] = 0xB6U;
            asset_id = lux::asset::AssetId{id_bytes};
            asset.description.schema_version = lux::rdesc::Script::kSchemaVersion;
            asset.description.module_name = "l1.benchmark.binding";
            asset.description.body = lux::rdesc::CppBehaviorScript{"benchmark"};
            for (std::uint64_t symbol = 1U; symbol <= 4U; ++symbol)
            {
                asset.description.exports.push_back(lux::rdesc::ScriptFunction{
                    "hook-" + std::to_string(symbol), symbol, {}, {}});
            }
            asset.description.exports.push_back(
                lux::rdesc::ScriptFunction{"global", 5U, {}, {}}
            );
            asset.description.exports.push_back(
                lux::rdesc::ScriptFunction{"entity", 6U, {}, {}}
            );

            lux::simulation::SimulationDescriptionBuilder builder;
            if (!builder.addSystem("benchmark", kBindingSystem))
                throw std::runtime_error("binding system build failed");
            std::vector<lux::rdesc::ScriptBindingDescription> global_bindings;
            if (kind == "bound-call-native" || kind == "hook-multi")
            {
                const auto handler_count = kind == "hook-multi" ? 4U : 1U;
                for (std::uint64_t symbol = 1U; symbol <= handler_count; ++symbol)
                {
                    global_bindings.push_back({
                        symbol,
                        lux::rdesc::EScriptBindingKind::HOOK,
                        "lux.benchmark.binding",
                        "benchmark",
                        "update"});
                }
            }
            else if (kind == "global-event")
            {
                global_bindings.push_back({
                    5U,
                    lux::rdesc::EScriptBindingKind::EVENT,
                    "lux.benchmark.binding",
                    "benchmark",
                    "global-event"});
            }
            if (!global_bindings.empty() &&
                !builder.addGlobalScriptMount(
                    lux::simulation::ScriptMountDescription{
                        asset_id,
                        lux::simulation::EScriptBindingSetMode::EXPLICIT,
                        std::move(global_bindings)}
                ))
            {
                throw std::runtime_error("binding mount build failed");
            }
            auto description = std::move(builder).build();
            if (!description)
                throw std::runtime_error("binding description build failed");

            if (kind == "entity-targeted-event")
            {
                entities.reserve(count);
                registry.storage<lux::simulation::ScriptMountFacts>().reserve(count);
                for (std::size_t index{}; index < count; ++index)
                {
                    const auto entity = registry.create();
                    registry.emplace<lux::simulation::ScriptMountFacts>(
                        entity,
                        lux::simulation::ScriptMountFacts{{
                            lux::simulation::ScriptMountDescription{
                                asset_id,
                                lux::simulation::EScriptBindingSetMode::EXPLICIT,
                                {{
                                    6U,
                                    lux::rdesc::EScriptBindingKind::EVENT,
                                    "lux.benchmark.binding",
                                    "benchmark",
                                    "entity-event"}}}}}
                    );
                    entities.push_back(entity);
                }
            }

            const lux::simulation::ScriptBackendDescriptor backend{
                lux::rdesc::Script::Kind::CPP_BEHAVIOR,
                this,
                &BindingBenchmarkState::prepareCall,
                nullptr};
            auto created = lux::simulation::ScriptBindingSession::create(
                std::move(*description),
                registry,
                lux::simulation::ScriptBindingCapacities{
                    std::max<std::size_t>(count + 4U, 8U),
                    std::max<std::size_t>(count + 1U, 2U),
                    4U,
                    std::max<std::size_t>((count + 3U) / 4U, 1U),
                    8U},
                lux::simulation::ScriptAssetResolver{
                    this,
                    &BindingBenchmarkState::resolveAsset},
                std::span{&backend, 1U}
            );
            if (!created)
                throw std::runtime_error("binding session create failed");
            session = std::make_unique<lux::simulation::ScriptBindingSession>(
                std::move(*created)
            );
            if (!session->prepare())
                throw std::runtime_error("binding session prepare failed");
            hook = session->hookSlot("benchmark", "update");
            global_event = session->eventSlot("benchmark", "global-event");
            entity_event = session->eventSlot("benchmark", "entity-event");
        }

        static bool resolveAsset(
            void* opaque,
            const lux::asset::AssetId& id,
            lux::simulation::ResolvedScriptAsset& result
        ) noexcept
        {
            auto& self = *static_cast<BindingBenchmarkState*>(opaque);
            if (id != self.asset_id)
                return false;
            result.asset = std::addressof(self.asset);
            return true;
        }

        static lux::simulation::EScriptBackendPrepareResult prepareCall(
            void* opaque,
            const lux::simulation::ScriptPrepareContext&,
            const lux::asset::ScriptAssetContent&,
            const lux::rdesc::ScriptFunction&,
            lux::script::BoundScriptCall& result
        ) noexcept
        {
            result = lux::script::BoundScriptCall{
                &BindingBenchmarkState::invoke,
                opaque};
            return lux::simulation::EScriptBackendPrepareResult::SUCCESS;
        }

        static int invoke(lux_script_call_frame* frame) noexcept
        {
            ++static_cast<BindingBenchmarkState*>(frame->user_context)->callbacks;
            return 0;
        }

        std::string kind;
        Registry registry;
        std::vector<Entity> entities;
        lux::asset::AssetId asset_id;
        lux::asset::ScriptAssetContent asset;
        std::unique_ptr<lux::simulation::ScriptBindingSession> session;
        lux::simulation::ScriptHookSlot hook;
        lux::simulation::ScriptEventSlot global_event;
        lux::simulation::ScriptEventSlot entity_event;
        std::size_t callbacks{};
    };

    struct TaskGraphState final
    {
        explicit TaskGraphState(std::size_t count)
        {
            lux::task::TaskGraphBuilder builder;
            for (std::size_t index{}; index < count; ++index)
            {
                if (!builder.add([this]() noexcept { ++executed; }))
                    throw std::runtime_error("task add failed");
            }
            auto built = std::move(builder).build();
            if (!built)
                throw std::runtime_error("task graph build failed");
            graph = std::move(*built);
            executor = std::make_unique<lux::task::TaskExecutor>(
                lux::task::TaskExecutorConfig{0U, graph.taskCount()}
            );
        }

        std::atomic_size_t executed{};
        lux::task::TaskGraph graph;
        std::unique_ptr<lux::task::TaskExecutor> executor;
    };

    struct SnapshotState final
    {
        explicit SnapshotState(std::size_t count)
        {
            std::vector<ComponentSchema> schemas_values;
            const auto transform_schemas = transformComponentSchemas();
            schemas_values.insert(
                schemas_values.end(),
                transform_schemas.begin(),
                transform_schemas.end()
            );
            auto built_schemas = ComponentSchemaSet::build(
                std::move(schemas_values)
            );
            if (!built_schemas)
                throw std::runtime_error("schema build failed");
            schemas = std::move(*built_schemas);
            const std::array contributions{
                transformComponentSnapshotContribution()};
            auto built_components = ComponentSnapshotSet::build(
                schemas,
                contributions
            );
            if (!built_components)
                throw std::runtime_error("snapshot component build failed");
            components = std::move(*built_components);
            registry.storage<Transform3D>().reserve(count);
            for (std::size_t index{}; index < count; ++index)
                registry.emplace<Transform3D>(registry.create());
        }

        Registry registry;
        ComponentSchemaSet schemas;
        ComponentSnapshotSet components;
    };
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
        samples = measure(
            *options,
            [&] { return std::make_unique<WorldState>(options->size); },
            [](WorldState& state)
            {
                for (const Entity entity : state.entities)
                {
                    state.registry.patch<Position>(
                        entity,
                        [](Position& value) noexcept { ++value.value; }
                    );
                }
                return Observation{.updates = state.entities.size()};
            },
            [](WorldState&) noexcept {}
        );
    }
    else if (options->group == "reactive-dirty")
    {
        metric = "reactive_dirty_patch";
        samples = measure(
            *options,
            [&]
            {
                return std::make_unique<ReactiveDirtyState>(options->size);
            },
            [](ReactiveDirtyState& state)
            {
                state.dirty.clear();
                for (const Entity entity : state.entities)
                {
                    state.registry.patch<Position>(
                        entity,
                        [](Position& value) noexcept { ++value.value; }
                    );
                }
                if (state.dirty.size() != state.entities.size())
                    throw std::runtime_error("dirty notification mismatch");
                return Observation{
                    .updates = state.entities.size(),
                    .notifications = state.dirty.size()};
            },
            [](ReactiveDirtyState&) noexcept {}
        );
    }
    else if (options->group == "command-buffer")
    {
        metric = "command_buffer_record";
        samples = measure(
            *options,
            [&] { return std::make_unique<CommandState>(options->size); },
            [](CommandState& state)
            {
                {
                    auto begun = state.commands.begin(0U);
                    if (!begun)
                        throw std::runtime_error("command begin failed");
                    auto writer = std::move(*begun);
                    for (const Entity entity : state.entities)
                    {
                        if (!writer.remove<Position>(entity))
                            throw std::runtime_error("command record failed");
                    }
                }
                return Observation{.updates = state.entities.size()};
            },
            [](CommandState& state) noexcept
            {
                state.commands.discardPending();
            }
        );
    }
    else if (options->group == "bound-call-native" ||
             options->group == "hook-multi" ||
             options->group == "global-event" ||
             options->group == "entity-targeted-event")
    {
        metric = options->group == "bound-call-native"
            ? "bound_call_native"
            : options->group == "hook-multi"
                ? "hook_multi"
                : options->group == "global-event"
                    ? "global_event"
                    : "entity_targeted_event";
        samples = measure(
            *options,
            [&]
            {
                return std::make_unique<BindingBenchmarkState>(
                    options->size,
                    options->group
                );
            },
            [&](BindingBenchmarkState& state)
            {
                state.callbacks = 0U;
                state.session->beginUpdate();
                lux_script_call_frame frame{};
                if (options->group == "global-event")
                {
                    for (std::size_t index{}; index < options->size; ++index)
                    {
                        if (!state.session->writer(index % 4U).emit(
                                state.global_event,
                                frame
                            ))
                        {
                            throw std::runtime_error("global event emit failed");
                        }
                    }
                }
                else if (options->group == "entity-targeted-event")
                {
                    for (std::size_t index{}; index < options->size; ++index)
                    {
                        if (!state.session->writer(index % 4U).emit(
                                state.entity_event,
                                state.entities[index],
                                frame
                            ))
                        {
                            throw std::runtime_error("entity event emit failed");
                        }
                    }
                }
                const auto iterations =
                    options->group == "global-event" ||
                    options->group == "entity-targeted-event"
                    ? 1U
                    : options->size;
                std::size_t dispatch_calls{};
                for (std::size_t index{}; index < iterations; ++index)
                    dispatch_calls += state.session->dispatchHook(
                        state.hook,
                        frame
                    ).calls;
                const auto expected_callbacks = options->group == "hook-multi"
                    ? options->size * 4U
                    : options->size;
                if (state.callbacks != expected_callbacks ||
                    dispatch_calls != expected_callbacks)
                {
                    throw std::runtime_error("binding callback mismatch");
                }
                return Observation{
                    .updates = options->size,
                    .notifications =
                        options->group == "global-event" ||
                        options->group == "entity-targeted-event"
                        ? options->size
                        : 0U,
                    .callbacks = state.callbacks,
                    .reflection_lookups =
                        state.session->hotPathNameLookupCount(),
                    .string_lookups =
                        state.session->hotPathNameLookupCount(),
                    .asset_lookups =
                        state.session->hotPathAssetLookupCount(),
                    .scene_scans =
                        state.session->hotPathSceneScanCount()};
            },
            [](BindingBenchmarkState&) noexcept {}
        );
    }
    else if (options->group == "task-graph")
    {
        metric = "task_graph_execute";
        samples = measure(
            *options,
            [&] { return std::make_unique<TaskGraphState>(options->size); },
            [](TaskGraphState& state)
            {
                state.executed.store(0U, std::memory_order_relaxed);
                if (!state.executor->execute(state.graph))
                    throw std::runtime_error("task graph execute failed");
                const auto count = state.executed.load(std::memory_order_relaxed);
                if (count != state.graph.taskCount())
                    throw std::runtime_error("task callback mismatch");
                return Observation{.callbacks = count};
            },
            [](TaskGraphState&) noexcept {}
        );
    }
    else
    {
        metric = "ecs_snapshot_capture";
        samples = measure(
            *options,
            [&] { return std::make_unique<SnapshotState>(options->size); },
            [](SnapshotState& state)
            {
                const auto snapshot = EcsSnapshot::capture(
                    state.registry,
                    state.components
                );
                if (!snapshot)
                    throw std::runtime_error("snapshot capture failed");
                return Observation{
                    .updates = state.registry.storage<Entity>().free_list()};
            },
            [](SnapshotState&) noexcept {}
        );
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
