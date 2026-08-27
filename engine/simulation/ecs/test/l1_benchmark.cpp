#include <lux/engine/simulation/EventPoint.hpp>
#include <lux/engine/simulation/HookPoint.hpp>
#include <lux/engine/simulation/ecs/EcsCommandBuffer.hpp>
#include <lux/engine/simulation/ecs/EcsSnapshot.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
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
#include <unordered_map>
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
                  "frame_builds\n";
        for (const auto& sample : samples)
        {
            const auto& value = sample.observation;
            output << "9," << LUX_BENCHMARK_GIT_COMMIT << ','
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
                   << ',' << value.frame_builds << '\n';
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
        explicit HookState(std::size_t count)
        {
            counters.resize(count);
            if (hook.prepare(count, count) != EEndpointMutationError::NONE)
                throw std::runtime_error("hook prepare failed");
            for (auto& counter : counters)
            {
                if (!hook.connect(&counter, [](void* value) noexcept
                    {
                        ++*static_cast<std::size_t*>(value);
                    }))
                    throw std::runtime_error("hook connect failed");
            }
            if (hook.flushMutations() != EEndpointMutationError::NONE)
                throw std::runtime_error("hook flush failed");
        }
        HookPoint<void()> hook;
        std::vector<std::size_t> counters;
    };

    struct BroadcastState final
    {
        explicit BroadcastState(std::size_t count)
        {
            if (event.prepare(1U, count, 1U, 1U) !=
                EEndpointMutationError::NONE ||
                !event.connect(&callbacks, [](void* value, const auto&) noexcept
                {
                    ++*static_cast<std::size_t*>(value);
                }) || event.flushMutations() != EEndpointMutationError::NONE)
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
        explicit SparseState(std::size_t scene_size)
        {
            const auto scripted = std::min<std::size_t>(scene_size, 10000U);
            const auto stride = std::max<std::size_t>(1U, scene_size / scripted);
            for (std::size_t index{}; index < scene_size; ++index)
            {
                const auto entity = registry.create();
                if (index % stride == 0U && handlers.size() < scripted)
                {
                    ranges.emplace(entityBits(entity), handlers.size());
                    handlers.push_back(0U);
                    entities.push_back(entity);
                }
            }
        }
        Registry registry;
        std::unordered_map<std::uint64_t, std::size_t> ranges;
        std::vector<std::size_t> handlers;
        std::vector<Entity> entities;
    };

    struct WorkerEventState final
    {
        explicit WorkerEventState(std::size_t count)
        {
            if (event.prepare(4U, (count + 3U) / 4U, 1U, 1U) !=
                EEndpointMutationError::NONE ||
                !event.connect(&callbacks, [](void* value, const auto&) noexcept
                {
                    ++*static_cast<std::size_t*>(value);
                }) || event.flushMutations() != EEndpointMutationError::NONE)
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
            executor = std::make_unique<lux::task::TaskExecutor>(
                lux::task::TaskExecutorConfig{4U, graph.taskCount()});
        }
        EventPoint<SimulationBroadcastRoute, std::size_t> event;
        std::size_t callbacks{};
        std::atomic_bool failed{};
        lux::task::TaskGraph graph;
        std::unique_ptr<lux::task::TaskExecutor> executor;
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
            executor = std::make_unique<lux::task::TaskExecutor>(
                lux::task::TaskExecutorConfig{0U, graph.taskCount()});
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
        metric = "hook_mixed_scope";
        samples = measure(*options,
            [&] { return std::make_unique<HookState>(options->size); },
            [](HookState& state)
            {
                const auto calls = state.hook.dispatch();
                return Observation{
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
                constexpr std::size_t occurrences{100000U};
                std::size_t callbacks{};
                for (std::size_t index{}; index < occurrences; ++index)
                {
                    const auto entity =
                        state.entities[index % state.entities.size()];
                    const auto found = state.ranges.find(entityBits(entity));
                    if (found != state.ranges.end())
                    {
                        ++state.handlers[found->second];
                        ++callbacks;
                    }
                }
                return Observation{
                    .notifications = occurrences,
                    .callbacks = callbacks,
                    .entities_examined = occurrences,
                    .target_range_lookups = occurrences,
                    .handlers_visited = callbacks,
                    .target_ranges_built = state.ranges.size(),
                    .dispatch_handlers_built = state.handlers.size(),
                    .frame_builds = occurrences};
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
