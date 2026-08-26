#include "WorldSectionFixtureBuilder.hpp"

#include <lux/engine/ecs/ComponentLoadSet.hpp>
#include <lux/engine/ecs/WorldSectionTransaction.hpp>
#include <lux/engine/ecs/WorldTaskResources.hpp>
#include <lux/engine/ecs/core/detail/EcsStateAccess.hpp>
#include <lux/engine/ecs/world_section/detail/WorldSectionTransactionAccess.hpp>
#include <lux/engine/meta/TypeStaticInfo.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <uuid.h>

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
#include <tuple>
#include <utility>
#include <vector>

#ifndef LUX_BENCHMARK_GIT_COMMIT
#define LUX_BENCHMARK_GIT_COMMIT "unknown"
#endif

#ifndef LUX_BENCHMARK_BUILD_TYPE
#define LUX_BENCHMARK_BUILD_TYPE "unknown"
#endif

struct BenchmarkFixed32 final
{
    std::array<std::uint64_t, 4U> words{};
};

namespace lux::meta
{
    template <>
    struct TypeStaticInfo<BenchmarkFixed32>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&BenchmarkFixed32::words>("words")
        );
    };
} // namespace lux::meta

namespace
{
    using Clock = std::chrono::steady_clock;
    using namespace lux::ecs;
    using namespace lux::ecs::world_section::test;

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
            result.group != "world-section")
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
        WorldChangeBatch batch;
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

    [[nodiscard]] WorldSectionId benchmarkSectionId()
    {
        return WorldSectionId{uuids::uuid::from_string(
            "30000000-0000-4000-8000-000000000001"
        ).value()};
    }

    struct WorldSectionState final
    {
        ComponentSchemaSet schemas;
        std::optional<ComponentLoadBinding> binding;
        ComponentLoadContribution contribution;
        ComponentLoadSet loads;
        std::optional<WorldSectionImage> image;
        std::unique_ptr<EcsState> world;
        WorldSectionInstance instance;

        explicit WorldSectionState(std::size_t section_rows)
        {
            auto built_schemas = ComponentSchemaSet::build({
                makeComponentSchema<BenchmarkFixed32>(
                    componentSchemaId("benchmark.Fixed32")
                ),
            });
            if (!built_schemas) throw std::runtime_error("schema build failed");
            schemas = std::move(*built_schemas);
            binding.emplace(bindComponentLoad<BenchmarkFixed32>(
                *schemas.find(componentSchemaId("benchmark.Fixed32"))
            ));
            contribution.bindings = std::span(&*binding, 1U);
            auto built_loads = ComponentLoadSet::build(
                schemas, std::span(&contribution, 1U)
            );
            if (!built_loads) throw std::runtime_error("load set build failed");
            loads = std::move(*built_loads);

            FixtureColumn column;
            column.schema_name = "benchmark.Fixed32";
            column.value_encoding = EWorldSectionValueEncoding::FIXED;
            column.fixed_stride = sizeof(BenchmarkFixed32);
            column.payload.resize(section_rows * sizeof(BenchmarkFixed32));
            auto opened = WorldSectionImage::open(
                buildFixture(
                    benchmarkSectionId(),
                    static_cast<std::uint32_t>(section_rows),
                    {std::move(column)}
                ),
                fixtureValidationBudget()
            );
            if (!opened) throw std::runtime_error("image build failed");
            image.emplace(std::move(*opened));

            world = std::make_unique<EcsState>(EcsStateConfig{{
                4096U, 64U * 1024U * 1024U,
            }});
            auto mutation = detail::WorldColdAccess::suppressingMutation(*world);
            for (std::size_t index{}; index < 1'000'000U; ++index)
                (void)mutation.create();
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
    else
    {
        evidence.measure(
            "world_section_load_commit_live_resident", parsed->size,
            [&]() { return std::make_unique<WorldSectionState>(parsed->size); },
            [](WorldSectionState& state)
            {
                auto& log = detail::WorldChangeAccess::log(*state.world);
                const auto epoch = log.epoch();
                const auto binds = log.streamBindCountForTest();
                const auto lookups = log.perRecordLookupCountForTest();
                detail::ComponentLoadTestStats::reset();
                auto transaction = beginWorldSectionTransaction(
                    *state.world,
                    WorldSectionLoadScratchBudget{256U * 1024U},
                    lux::serialization::SerializationLimits{}
                );
                if (!transaction || !transaction->load(
                        state.loads, *state.image, state.instance
                    ) || !transaction->commit())
                    throw std::runtime_error("section load failed");
                const auto membership =
                    detail::WorldSectionTransactionAccess::membershipStats(
                        *state.world
                    );
                Observation result;
                result.dispatch_calls = detail::ComponentLoadTestStats::load_calls;
                result.storage_lookups =
                    detail::ComponentLoadTestStats::storage_lookups;
                result.history_losses = log.epoch() - epoch;
                result.journal_stream_binds =
                    log.streamBindCountForTest() - binds;
                result.per_record_lookups =
                    log.perRecordLookupCountForTest() - lookups;
                result.membership_entry_capacity_bytes =
                    membership.entry_capacity_bytes;
                result.membership_node_capacity_bytes =
                    membership.node_capacity_bytes;
                result.active_tracked_entities =
                    membership.active_tracked_entities;
                result.active_memberships = membership.active_memberships;
                result.duplicate_comparisons = membership.duplicate_comparisons;
                result.retained_bytes = membership.entry_capacity_bytes +
                    membership.node_capacity_bytes;
                return result;
            },
            [](WorldSectionState& state)
            {
                if (!state.instance.active()) return;
                auto transaction = beginWorldSectionTransaction(
                    *state.world,
                    WorldSectionLoadScratchBudget{256U * 1024U},
                    lux::serialization::SerializationLimits{}
                );
                if (!transaction || !transaction->unload(state.instance) ||
                    !transaction->commit())
                    throw std::runtime_error("section unload failed");
            }
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
