#include <lux/engine/simulation/ecs/EcsTaskResources.hpp>
#include <lux/engine/simulation/ecs/EcsSnapshot.hpp>
#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>
#include <lux/engine/simulation/ecs/core/detail/EcsStateAccess.hpp>
#include <lux/engine/simulation/SimulationExecution.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <algorithm>
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
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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

#define LUX_STRINGIFY_IMPL(value) #value
#define LUX_STRINGIFY(value) LUX_STRINGIFY_IMPL(value)

    [[nodiscard]] constexpr std::string_view compilerName() noexcept
    {
#if defined(_MSC_VER)
        return "MSVC";
#elif defined(__clang__)
        return "Clang";
#elif defined(__GNUC__)
        return "GCC";
#else
        return "unknown";
#endif
    }

    [[nodiscard]] constexpr std::string_view compilerVersion() noexcept
    {
#if defined(_MSC_FULL_VER)
        return LUX_STRINGIFY(_MSC_FULL_VER);
#elif defined(__clang_version__)
        return __clang_version__;
#elif defined(__VERSION__)
        return __VERSION__;
#else
        return "unknown";
#endif
    }

    [[nodiscard]] constexpr std::string_view platformName() noexcept
    {
#if defined(_WIN32)
        return "Windows";
#elif defined(__ANDROID__)
        return "Android";
#elif defined(__linux__)
        return "Linux";
#else
        return "unknown";
#endif
    }

    [[nodiscard]] constexpr std::string_view architectureName() noexcept
    {
#if defined(_M_X64) || defined(__x86_64__)
        return "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
        return "arm64";
#else
        return "unknown";
#endif
    }

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
            if (key == "--group") result.group = value;
            else if (key == "--mode") result.mode = value;
            else if (key == "--output") result.output = value;
            else if (key == "--size")
            {
                const auto parsed = std::from_chars(
                    value.data(), value.data() + value.size(), result.size
                );
                if (parsed.ec != std::errc{} ||
                    parsed.ptr != value.data() + value.size() ||
                    result.size == 0U ||
                    result.size > std::numeric_limits<std::uint32_t>::max())
                    return std::nullopt;
            }
            else return std::nullopt;
        }
        if (result.group != "task-graph" &&
            result.group != "world-change-batch" &&
            result.group != "simulation-step" &&
            result.group != "command-batch" &&
            result.group != "hierarchy-delta" &&
            result.group != "journal-readers" &&
            result.group != "ecs-snapshot")
            return std::nullopt;
        if (result.mode == "qualification")
        {
            result.warmups = 5U;
            result.samples = 30U;
        }
        else if (result.mode != "diagnostic") return std::nullopt;
        return result;
    }

    struct Observation final
    {
        std::size_t retained_bytes{};
        std::size_t dispatch_calls{};
        std::size_t storage_lookups{};
        std::size_t visited_nodes{};
        std::size_t history_losses{};
        std::size_t lane_binds{};
        std::size_t journal_stream_binds{};
        std::size_t record_appends{};
        std::size_t per_record_lookups{};
        std::size_t membership_entry_capacity_bytes{};
        std::size_t membership_node_capacity_bytes{};
        std::size_t active_tracked_entities{};
        std::size_t active_memberships{};
        std::size_t duplicate_comparisons{};
    };

    struct Sample final
    {
        std::string group;
        std::string metric;
        std::size_t size{};
        std::size_t index{};
        std::uint64_t nanoseconds{};
        std::size_t allocations{};
        Observation observation;
    };

    void writeRow(
        std::ostream& output,
        std::string_view kind,
        std::string_view group,
        std::string_view metric,
        std::size_t size,
        std::string_view sample,
        std::uint64_t nanoseconds,
        std::size_t allocations,
        const Observation& observation
    )
    {
        output << "4," << LUX_BENCHMARK_GIT_COMMIT << ','
               << compilerName() << ',' << compilerVersion() << ','
               << LUX_BENCHMARK_BUILD_TYPE << ',' << platformName() << ','
               << architectureName() << ',' << kind << ',' << group << ','
               << metric << ',' << size << ',' << sample << ','
               << nanoseconds << ',' << allocations << ','
               << observation.retained_bytes << ','
               << observation.dispatch_calls << ','
               << observation.storage_lookups << ','
               << observation.visited_nodes << ','
               << observation.history_losses << ','
               << observation.lane_binds << ','
               << observation.journal_stream_binds << ','
               << observation.record_appends << ','
               << observation.per_record_lookups << ','
               << observation.membership_entry_capacity_bytes << ','
               << observation.membership_node_capacity_bytes << ','
               << observation.active_tracked_entities << ','
               << observation.active_memberships << ','
               << observation.duplicate_comparisons << '\n';
    }

    void writeCsv(
        const std::filesystem::path& path,
        const std::vector<Sample>& samples
    )
    {
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path());
        auto temporary = path;
        temporary += ".tmp";
        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output) throw std::runtime_error("cannot open output");
            output << "benchmark_schema_version,git_commit,compiler,"
                      "compiler_version,build_type,platform,architecture,kind,"
                      "group,metric,size,sample,nanoseconds,allocations,"
                      "retained_bytes,dispatch_calls,storage_lookups,"
                      "visited_nodes,history_losses,lane_binds,"
                      "journal_stream_binds,record_appends,per_record_lookups,"
                      "membership_entry_capacity_bytes,"
                      "membership_node_capacity_bytes,active_tracked_entities,"
                      "active_memberships,duplicate_comparisons\n";
            for (const auto& value : samples)
            {
                writeRow(
                    output, "raw", value.group, value.metric, value.size,
                    std::to_string(value.index), value.nanoseconds,
                    value.allocations, value.observation
                );
            }
            std::size_t begin{};
            while (begin < samples.size())
            {
                std::size_t end = begin + 1U;
                while (end < samples.size() &&
                       samples[end].group == samples[begin].group &&
                       samples[end].metric == samples[begin].metric &&
                       samples[end].size == samples[begin].size)
                    ++end;
                std::vector<std::uint64_t> times;
                std::vector<std::size_t> allocations;
                for (std::size_t index = begin; index < end; ++index)
                {
                    times.push_back(samples[index].nanoseconds);
                    allocations.push_back(samples[index].allocations);
                }
                std::sort(times.begin(), times.end());
                std::sort(allocations.begin(), allocations.end());
                const std::size_t median = times.size() / 2U;
                const std::size_t p95 =
                    (times.size() * 95U + 99U) / 100U - 1U;
                writeRow(
                    output, "summary", samples[begin].group,
                    samples[begin].metric, samples[begin].size, "median",
                    times[median], allocations[median],
                    samples[end - 1U].observation
                );
                writeRow(
                    output, "summary", samples[begin].group,
                    samples[begin].metric, samples[begin].size, "p95",
                    times[p95], allocations[p95],
                    samples[end - 1U].observation
                );
                begin = end;
            }
            output.flush();
            if (!output) throw std::runtime_error("cannot flush output");
        }
        std::error_code error;
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
        if (error) throw std::runtime_error("cannot publish output");
    }

    class Evidence final
    {
      public:
        explicit Evidence(Options options) : options_(std::move(options)) {}

        template <class Setup, class Operation, class Teardown>
        void measure(
            std::string metric,
            std::size_t size,
            Setup&& setup,
            Operation&& operation,
            Teardown&& teardown
        )
        {
            for (std::size_t index{}; index < options_.warmups; ++index)
            {
                auto state = setup();
                (void)operation(*state);
                teardown(*state);
            }
            for (std::size_t index{}; index < options_.samples; ++index)
            {
                auto state = setup();
                g_allocation_count.store(0U, std::memory_order_relaxed);
                g_count_allocations.store(true, std::memory_order_release);
                const auto begin = Clock::now();
                const Observation observation = operation(*state);
                const auto end = Clock::now();
                g_count_allocations.store(false, std::memory_order_release);
                teardown(*state);
                samples_.push_back(Sample{
                    options_.group, std::move(metric), size, index,
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            end - begin
                        ).count()
                    ),
                    g_allocation_count.load(std::memory_order_relaxed),
                    observation
                });
                metric = samples_.back().metric;
                writeCsv(options_.output, samples_);
            }
        }

      private:
        Options options_;
        std::vector<Sample> samples_;
    };

    struct TaskState final
    {
        std::atomic_size_t calls{};
        lux::task::TaskGraph graph;
        lux::task::TaskExecutor executor;

        explicit TaskState(std::size_t count)
            : executor(lux::task::TaskExecutorConfig{4U, count})
        {
            lux::task::TaskGraphBuilder builder;
            for (std::size_t index{}; index < count; ++index)
                if (!builder.add([this]() noexcept
                    { calls.fetch_add(1U, std::memory_order_relaxed); }))
                    throw std::bad_alloc{};
            auto built = std::move(builder).build();
            if (!built) throw std::bad_alloc{};
            graph = std::move(*built);
            if (!executor.reserve(count)) throw std::bad_alloc{};
        }
    };

    struct ChangeBatchState final
    {
        EcsChangeBatch batch;
        std::vector<std::uint64_t> storages;
        std::size_t rows{};

        ChangeBatchState(std::size_t lane_count, std::size_t row_count)
            : storages(lane_count), rows(row_count)
        {
            for (std::size_t index{}; index < lane_count; ++index)
                storages[index] = index + 1U;
            if (!batch.prepare(storages, rows)) throw std::bad_alloc{};
        }
    };

    struct BenchmarkPosition final
    {
        std::uint32_t value{};
    };

    struct IncrementPosition final
    {
        Entity entity{NullEntity};

        void apply(SimulationEcsMutation& mutation) noexcept
        {
            mutation.update<BenchmarkPosition>(
                entity,
                [](BenchmarkPosition& position) noexcept
                {
                    ++position.value;
                }
            );
        }
    };

    struct StepState final
    {
        EcsState state;
        EcsChangeJournal journal{EcsChangeHistoryBudget{4096U, 65536U}};
        EcsCommandBatch commands;
        lux::task::TaskGraph graph;
        lux::task::TaskExecutor executor{
            lux::task::TaskExecutorConfig{1U, 1U}};
        Entity entity{NullEntity};

        StepState()
        {
            auto mutation = state.mutate();
            if (!mutation)
                throw std::runtime_error("mutation failed");
            entity = mutation->create();
            mutation->emplace<BenchmarkPosition>(entity, 0U);
            *mutation = {};

            auto observed = beginSimulationEcsMutation(state, journal);
            if (!observed)
                throw std::runtime_error("observed mutation failed");
            observed->update<BenchmarkPosition>(
                entity,
                [](BenchmarkPosition&) noexcept {}
            );
            *observed = {};

            constexpr std::array capacities{
                EcsCommandProducerCapacity{1U, 64U}
            };
            if (!commands.prepare(capacities))
                throw std::bad_alloc{};

            lux::task::TaskGraphBuilder builder;
            auto task = builder.add([this]() noexcept
            {
                auto scope = commands.begin(0U);
                if (!scope ||
                    scope->commands().push(IncrementPosition{entity}) !=
                        ECommandResult::ACCEPTED)
                {
                    std::abort();
                }
            });
            if (!task)
                throw std::bad_alloc{};
            auto built = std::move(builder).build();
            if (!built || !executor.reserve(1U))
                throw std::bad_alloc{};
            graph = std::move(*built);
        }
    };

    struct NoopCommand final
    {
        std::uint64_t value{};

        void apply(SimulationEcsMutation&) noexcept
        {
        }
    };

    struct CommandState final
    {
        EcsCommandBatch commands;
        std::size_t count{};

        explicit CommandState(std::size_t value) : count(value)
        {
            const std::array capacities{
                EcsCommandProducerCapacity{
                    count,
                    count * (sizeof(NoopCommand) + alignof(NoopCommand) - 1U)
                }
            };
            if (!commands.prepare(capacities))
                throw std::bad_alloc{};
        }
    };

    struct HierarchyState final
    {
        HierarchyIndex hierarchy;
        HierarchyMutationBatch mutations;
        HierarchyDeltaBatch deltas;
        std::size_t count{};

        explicit HierarchyState(std::size_t value) : count(value)
        {
            if (!mutations.prepare(count) || !deltas.prepare(count))
                throw std::bad_alloc{};
            for (std::size_t index{}; index < count; ++index)
            {
                const Entity child = static_cast<Entity>(
                    static_cast<std::uint32_t>(index + 2U)
                );
                const Entity parent = static_cast<Entity>(
                    static_cast<std::uint32_t>(index + 1U)
                );
                if (!mutations.append(HierarchyMutation{
                        EHierarchyMutationKind::SET_PARENT,
                        child,
                        parent
                    }))
                {
                    throw std::bad_alloc{};
                }
            }
            if (!hierarchy.rebuild(mutations.values(), deltas))
                throw std::runtime_error("hierarchy rebuild failed");
            mutations.reset();
            deltas.reset();
        }
    };

    struct JournalReaderState final
    {
        EcsChangeJournal journal{EcsChangeHistoryBudget{4096U, 65536U}};
        std::vector<ChangeCursor<BenchmarkPosition>> cursors;
        std::vector<std::size_t> counts;

        explicit JournalReaderState(std::size_t readers)
            : cursors(readers), counts(readers)
        {
            for (auto& cursor : cursors)
                (void)journal.read(cursor);
            EcsChangeBatch batch;
            const std::uint64_t storage =
                entt::type_hash<BenchmarkPosition>::value();
            if (!batch.prepare(std::span{&storage, 1U}, 1U))
                throw std::bad_alloc{};
            auto stream = batch.binder()(storage);
            if (!stream ||
                !stream(static_cast<Entity>(1U), EComponentChangeKind::MODIFIED) ||
                !batch.publish(journal))
            {
                throw std::runtime_error("journal setup failed");
            }
        }
    };

    struct SnapshotState final
    {
        EcsState state;
        ComponentSnapshotSet components;
        std::size_t count{};

        explicit SnapshotState(std::size_t value) : count(value)
        {
            const auto schema = makeComponentSchema<BenchmarkPosition>(
                componentSchemaId("benchmark.position")
            );
            auto schemas = ComponentSchemaSet::build({schema});
            if (!schemas)
                throw std::runtime_error("schema build failed");
            const std::array bindings{
                bindComponentSnapshot<BenchmarkPosition>(schema)
            };
            const ComponentSnapshotContribution contribution{
                {},
                bindings
            };
            auto built = ComponentSnapshotSet::build(
                *schemas,
                std::span(&contribution, 1U)
            );
            if (!built)
                throw std::runtime_error("snapshot binding failed");
            components = *built;

            auto mutation = state.mutate();
            if (!mutation)
                throw std::runtime_error("snapshot state mutation failed");
            mutation->reserve<BenchmarkPosition>(count);
            for (std::size_t index{}; index < count; ++index)
            {
                const Entity entity = mutation->create();
                mutation->emplace<BenchmarkPosition>(
                    entity,
                    static_cast<std::uint32_t>(index)
                );
            }
        }
    };

} // namespace

int main(int argc, char** argv)
{
    const auto parsed = parseOptions(argc, argv);
    if (!parsed) return 2;
    Evidence evidence(*parsed);

    if (parsed->group == "task-graph")
    {
        evidence.measure(
            "task_graph_execute_none", parsed->size,
            [&]() { return std::make_unique<TaskState>(parsed->size); },
            [](TaskState& state)
            {
                const std::size_t before = state.calls.load();
                if (!state.executor.execute(state.graph))
                    throw std::runtime_error("task execution failed");
                Observation result;
                result.dispatch_calls = state.calls.load() - before;
                return result;
            },
            [](TaskState&) noexcept {}
        );
    }
    else if (parsed->group == "world-change-batch")
    {
        for (const std::size_t lanes : {1U, 4U, 16U, 32U})
        {
            evidence.measure(
                "world_change_batch_record_" + std::to_string(lanes),
                parsed->size,
                [&, lanes]()
                { return std::make_unique<ChangeBatchState>(lanes, parsed->size); },
                [](ChangeBatchState& state)
                {
                    for (const std::uint64_t storage : state.storages)
                    {
                        auto stream = state.batch.binder()(storage);
                        if (!stream) throw std::runtime_error("bind failed");
                        for (std::size_t row{}; row < state.rows; ++row)
                            if (!stream(
                                    static_cast<Entity>(
                                        static_cast<std::uint32_t>(row)
                                    ),
                                    EComponentChangeKind::MODIFIED
                                ))
                                throw std::runtime_error("append failed");
                    }
                    const auto stats = state.batch.stats();
                    Observation result;
                    result.retained_bytes =
                        stats.retained_capacity * sizeof(Entity);
                    result.lane_binds = stats.lane_binds;
                    result.record_appends = stats.record_appends;
                    result.per_record_lookups = stats.per_record_lookups;
                    return result;
                },
                [](ChangeBatchState& state) noexcept { state.batch.reset(); }
            );
        }
    }
    else if (parsed->group == "simulation-step")
    {
        evidence.measure(
            "simulation_step_execute", parsed->size,
            []() { return std::make_unique<StepState>(); },
            [](StepState& state)
            {
                if (!lux::simulation::executeSimulationStep(
                        state.executor,
                        state.graph,
                        state.state,
                        state.journal,
                        state.commands
                    ))
                {
                    throw std::runtime_error("simulation step failed");
                }
                Observation result;
                result.dispatch_calls = 1U;
                return result;
            },
            [](StepState&) noexcept {}
        );
    }
    else if (parsed->group == "command-batch")
    {
        evidence.measure(
            "command_batch_record", parsed->size,
            [&]() { return std::make_unique<CommandState>(parsed->size); },
            [](CommandState& state)
            {
                auto scope = state.commands.begin(0U);
                if (!scope)
                    throw std::runtime_error("command begin failed");
                for (std::size_t index{}; index < state.count; ++index)
                {
                    if (scope->commands().push(NoopCommand{index}) !=
                        ECommandResult::ACCEPTED)
                    {
                        throw std::runtime_error("command push failed");
                    }
                }
                Observation result;
                result.dispatch_calls = state.count;
                return result;
            },
            [](CommandState& state) noexcept
            {
                state.commands.discardPending();
            }
        );
    }
    else if (parsed->group == "hierarchy-delta")
    {
        evidence.measure(
            "hierarchy_delta_apply", parsed->size,
            [&]() { return std::make_unique<HierarchyState>(parsed->size); },
            [](HierarchyState& state)
            {
                const Entity root = static_cast<Entity>(1U);
                for (std::size_t index{}; index < state.count; ++index)
                {
                    const Entity child = static_cast<Entity>(
                        static_cast<std::uint32_t>(index + 2U)
                    );
                    if (!state.mutations.append(HierarchyMutation{
                            EHierarchyMutationKind::SET_PARENT,
                            child,
                            root
                        }))
                    {
                        throw std::runtime_error("hierarchy append failed");
                    }
                }
                if (!state.hierarchy.apply(
                        state.mutations.values(),
                        state.deltas
                    ))
                {
                    throw std::runtime_error("hierarchy apply failed");
                }
                Observation result;
                result.dispatch_calls = state.deltas.values().size();
                return result;
            },
            [](HierarchyState&) noexcept {}
        );
    }
    else if (parsed->group == "journal-readers")
    {
        evidence.measure(
            "journal_concurrent_readers", parsed->size,
            [&]() { return std::make_unique<JournalReaderState>(parsed->size); },
            [](JournalReaderState& state)
            {
                std::vector<std::thread> readers;
                readers.reserve(state.cursors.size());
                for (std::size_t index{}; index < state.cursors.size(); ++index)
                {
                    readers.emplace_back([&state, index]() noexcept
                    {
                        const auto changes = state.journal.read(
                            state.cursors[index]
                        );
                        state.counts[index] = changes.size();
                    });
                }
                for (auto& reader : readers)
                    reader.join();
                Observation result;
                result.dispatch_calls = state.cursors.size();
                return result;
            },
            [](JournalReaderState&) noexcept {}
        );
    }
    else if (parsed->group == "ecs-snapshot")
    {
        evidence.measure(
            "ecs_snapshot_capture", parsed->size,
            [&]() { return std::make_unique<SnapshotState>(parsed->size); },
            [](SnapshotState& state)
            {
                auto snapshot = EcsSnapshot::capture(
                    state.state,
                    state.components
                );
                if (!snapshot)
                    throw std::runtime_error("snapshot capture failed");
                Observation result;
                result.dispatch_calls = state.count;
                return result;
            },
            [](SnapshotState&) noexcept {}
        );
    }
    return 0;
}

void* operator new(std::size_t size)
{
    if (g_count_allocations.load(std::memory_order_relaxed))
        g_allocation_count.fetch_add(1U, std::memory_order_relaxed);
    if (void* result = std::malloc(size == 0U ? 1U : size)) return result;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept
{ ::operator delete(value); }
