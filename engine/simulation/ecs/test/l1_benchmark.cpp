#include <lux/engine/simulation/ecs/EcsCommandBuffer.hpp>
#include <lux/engine/simulation/ecs/EcsSnapshot.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
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
            result.group != "typed-event" &&
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
                      "notifications,callbacks,reflection_lookups,string_lookups\n";
            for (const auto& sample : samples)
            {
                output << "5," << LUX_BENCHMARK_GIT_COMMIT << ','
                       << LUX_BENCHMARK_BUILD_TYPE << ',' << options.group
                       << ',' << metric << ',' << options.size << ','
                       << sample.index << ',' << sample.nanoseconds << ','
                       << sample.allocations << ','
                       << sample.observation.updates << ','
                       << sample.observation.notifications << ','
                       << sample.observation.callbacks << ','
                       << sample.observation.reflection_lookups << ','
                       << sample.observation.string_lookups << '\n';
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

    struct TypedEvent final
    {
        std::uint32_t producer{};
        std::uint32_t local{};
    };

    struct TypedEventState final
    {
        explicit TypedEventState(std::size_t count)
        {
            const std::size_t per_producer = (count + 3U) / 4U;
            for (auto& values : producers)
                values.reserve(per_producer);
        }

        std::array<std::vector<TypedEvent>, 4U> producers;
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
    else if (options->group == "typed-event")
    {
        metric = "typed_event_dispatch";
        samples = measure(
            *options,
            [&] { return std::make_unique<TypedEventState>(options->size); },
            [&](TypedEventState& state)
            {
                for (auto& values : state.producers)
                    values.clear();
                for (std::size_t index{}; index < options->size; ++index)
                {
                    const auto producer = static_cast<std::uint32_t>(index % 4U);
                    auto& values = state.producers[producer];
                    values.push_back(TypedEvent{
                        producer,
                        static_cast<std::uint32_t>(values.size())});
                }
                std::size_t callbacks{};
                for (std::uint32_t producer{};
                     producer < state.producers.size(); ++producer)
                {
                    std::uint32_t expected{};
                    for (const auto event : state.producers[producer])
                    {
                        if (event.producer != producer ||
                            event.local != expected++)
                        {
                            throw std::runtime_error("event order mismatch");
                        }
                        ++callbacks;
                    }
                }
                if (callbacks != options->size)
                    throw std::runtime_error("event callback mismatch");
                return Observation{
                    .notifications = options->size,
                    .callbacks = callbacks};
            },
            [](TypedEventState&) noexcept {}
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
