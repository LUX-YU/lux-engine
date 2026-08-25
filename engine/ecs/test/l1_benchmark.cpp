#include "WorldSectionFixtureBuilder.hpp"

#include <lux/engine/ecs/ComponentLoadSet.hpp>
#include <lux/engine/ecs/HierarchyIndex.hpp>
#include <lux/engine/ecs/HierarchySystem.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/TransformSchema.hpp>
#include <lux/engine/ecs/TransformSystem.hpp>
#include <lux/engine/ecs/WorldSectionLoader.hpp>
#include <lux/engine/ecs/WorldSnapshot.hpp>
#include <lux/engine/ecs/SystemExecution.hpp>
#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/SystemRelations.hpp>
#include <lux/engine/ecs/SystemTaskGraphCompiler.hpp>
#include <lux/engine/ecs/core/detail/WorldAccess.hpp>
#include <lux/engine/ecs/core/detail/WorldChangeLog.hpp>
#include <lux/engine/ecs/hierarchy/detail/HierarchyIndexTestAccess.hpp>
#include <lux/engine/ecs/system/detail/SystemTestRig.hpp>
#include <lux/engine/ecs/transform/detail/TransformSystemTestAccess.hpp>
#include <lux/engine/ecs/world_section/detail/ComponentLoadSerialization.hpp>
#include <lux/engine/ecs/world_section/detail/WorldSectionTransactionAccess.hpp>
#include <lux/engine/meta/TypeStaticInfo.hpp>
#include <lux/engine/serialization/Serialization.hpp>
#include <lux/engine/serialization/external_support/Eigen.hpp>
#include <lux/engine/task/TaskGraph.hpp>

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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

struct BenchmarkPosition final { std::uint64_t value{}; };

template <std::size_t Index>
struct BenchmarkWrite final
{
    std::uint64_t value{};
};

template <std::size_t Index>
struct BenchmarkFixed32 final
{
    std::array<std::uint64_t, 4> words{};
};

struct BenchmarkTag final {};

struct BenchmarkFixed16 final
{
    std::array<std::uint64_t, 2> words{};
};

struct BenchmarkFixed64 final
{
    std::array<std::uint64_t, 8> words{};
};

struct BenchmarkDynamicPayload final
{
    std::uint64_t sequence{};
    std::string label;
};

template <std::size_t Count>
struct BenchmarkEntityReferences final
{
    std::array<lux::ecs::Entity, Count> entities{};
};

namespace lux::meta
{
    template <>
    struct TypeStaticInfo<BenchmarkPosition>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&BenchmarkPosition::value>("value")
        );
    };

    template <std::size_t Index>
    struct TypeStaticInfo<BenchmarkFixed32<Index>>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&BenchmarkFixed32<Index>::words>("words")
        );
    };

    template <>
    struct TypeStaticInfo<BenchmarkTag>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::tuple{};
    };

    template <>
    struct TypeStaticInfo<BenchmarkFixed16>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&BenchmarkFixed16::words>("words")
        );
    };

    template <>
    struct TypeStaticInfo<BenchmarkFixed64>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&BenchmarkFixed64::words>("words")
        );
    };

    template <>
    struct TypeStaticInfo<BenchmarkDynamicPayload>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&BenchmarkDynamicPayload::sequence>("sequence"),
            typeStaticField<&BenchmarkDynamicPayload::label>("label")
        );
    };

    template <std::size_t Count>
    struct TypeStaticInfo<BenchmarkEntityReferences<Count>>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&BenchmarkEntityReferences<Count>::entities>(
                "entities"
            )
        );
    };
} // namespace lux::meta

namespace
{
    using Clock = std::chrono::steady_clock;
    std::atomic_size_t allocation_count{};
    std::atomic_bool count_allocations{};
    volatile std::uint64_t checksum{};

    enum class EGroup : std::uint8_t
    {
        QUERY,
        HIERARCHY,
        TRANSFORM,
        SNAPSHOT,
        WORLD_SECTION,
        TASK_GRAPH_BUILD,
        TASK_GRAPH_EXECUTE,
        SYSTEM_COMPILE,
        SYSTEM_EXECUTE,
        WORLD_CHANGE_LOG,
        SYSTEM_WRITE_QUERY,
    };

    enum class EMode : std::uint8_t { DIAGNOSTIC, QUALIFICATION };

    struct Options final
    {
        EGroup group{EGroup::QUERY};
        EMode mode{EMode::DIAGNOSTIC};
        std::size_t size{};
        std::filesystem::path output;
        std::size_t warmups{1U};
        std::size_t samples{3U};
    };

    struct Observation final
    {
        std::uint64_t measured_nanoseconds{};
        std::size_t retained_bytes{};
        std::size_t dispatch_calls{};
        std::size_t storage_lookups{};
        std::size_t visited_nodes{};
        std::size_t history_losses{};
    };

    struct Sample final
    {
        std::string metric;
        std::size_t size{};
        std::size_t index{};
        std::uint64_t nanoseconds{};
        std::size_t allocations{};
        Observation observation;
    };

    [[nodiscard]] std::optional<Options> parseOptions(int argc, char** argv)
    {
        Options result;
        std::string_view group;
        std::string_view mode;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument(argv[index]);
            if (index + 1 >= argc)
                return std::nullopt;
            const std::string_view value(argv[++index]);
            if (argument == "--group")
                group = value;
            else if (argument == "--mode")
                mode = value;
            else if (argument == "--output")
                result.output = value;
            else if (argument == "--size")
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
        if (group == "query") result.group = EGroup::QUERY;
        else if (group == "hierarchy") result.group = EGroup::HIERARCHY;
        else if (group == "transform") result.group = EGroup::TRANSFORM;
        else if (group == "snapshot") result.group = EGroup::SNAPSHOT;
        else if (group == "world-section") result.group = EGroup::WORLD_SECTION;
        else if (group == "task-graph-build")
            result.group = EGroup::TASK_GRAPH_BUILD;
        else if (group == "task-graph-execute")
            result.group = EGroup::TASK_GRAPH_EXECUTE;
        else if (group == "system-compile")
            result.group = EGroup::SYSTEM_COMPILE;
        else if (group == "system-execute")
            result.group = EGroup::SYSTEM_EXECUTE;
        else if (group == "world-change-log")
            result.group = EGroup::WORLD_CHANGE_LOG;
        else if (group == "system-write-query")
            result.group = EGroup::SYSTEM_WRITE_QUERY;
        else return std::nullopt;
        if (mode == "qualification")
        {
            result.mode = EMode::QUALIFICATION;
            result.warmups = 5U;
            result.samples = 30U;
        }
        else if (mode != "diagnostic")
            return std::nullopt;
        if (result.size == 0U || result.output.empty())
            return std::nullopt;
        return result;
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
            if (!output)
                throw std::runtime_error("cannot open output");
            output << "kind,metric,size,sample,nanoseconds,allocations,"
                      "retained_bytes,dispatch_calls,storage_lookups,"
                      "visited_nodes,history_losses\n";
            for (const auto& value : samples)
            {
                output << "raw," << value.metric << ',' << value.size << ','
                       << value.index << ',' << value.nanoseconds << ','
                       << value.allocations << ','
                       << value.observation.retained_bytes << ','
                       << value.observation.dispatch_calls << ','
                       << value.observation.storage_lookups << ','
                       << value.observation.visited_nodes << ','
                       << value.observation.history_losses << '\n';
            }
            std::size_t begin{};
            while (begin < samples.size())
            {
                std::size_t end = begin + 1U;
                while (end < samples.size() &&
                       samples[end].metric == samples[begin].metric &&
                       samples[end].size == samples[begin].size)
                {
                    ++end;
                }
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
                for (const auto [label, index] : {
                         std::pair{std::string_view{"median"}, median},
                         std::pair{std::string_view{"p95"}, p95}})
                {
                    output << "summary," << samples[begin].metric << ','
                           << samples[begin].size << ',' << label << ','
                           << times[index] << ',' << allocations[index] << ','
                           << samples[end - 1U].observation.retained_bytes << ','
                           << samples[end - 1U].observation.dispatch_calls << ','
                           << samples[end - 1U].observation.storage_lookups << ','
                           << samples[end - 1U].observation.visited_nodes << ','
                           << samples[end - 1U].observation.history_losses << '\n';
                }
                begin = end;
            }
            output.flush();
            if (!output)
                throw std::runtime_error("cannot flush output");
        }
        std::error_code error;
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
        if (error)
            throw std::runtime_error("cannot publish output");
    }

    class Evidence final
    {
      public:
        explicit Evidence(Options options) : options_(std::move(options)) {}

        template <class Fn>
        void measure(std::string metric, std::size_t size, Fn&& function)
        {
            for (std::size_t index{}; index < options_.warmups; ++index)
                (void)function();
            for (std::size_t index{}; index < options_.samples; ++index)
            {
                allocation_count.store(0U, std::memory_order_relaxed);
                count_allocations.store(true, std::memory_order_relaxed);
                const auto begin = Clock::now();
                const Observation observation = function();
                const auto end = Clock::now();
                count_allocations.store(false, std::memory_order_relaxed);
                const auto elapsed = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        end - begin
                    ).count()
                );
                samples_.push_back(Sample{
                    metric,
                    size,
                    index,
                    observation.measured_nanoseconds == 0U
                        ? elapsed
                        : observation.measured_nanoseconds,
                    allocation_count.load(std::memory_order_relaxed),
                    observation
                });
            }
            writeCsv(options_.output, samples_);
        }

      private:
        Options options_;
        std::vector<Sample> samples_;
    };

    std::unique_ptr<lux::ecs::World> positionWorld(std::size_t count)
    {
        auto world = std::make_unique<lux::ecs::World>();
        auto result = world->mutate();
        auto edit = std::move(*result);
        edit.reserve<BenchmarkPosition>(count);
        for (std::size_t index{}; index < count; ++index)
            edit.emplace<BenchmarkPosition>(edit.create(), index);
        return world;
    }

    class PositionWriteSystem final
        : public lux::ecs::StaticSystemAccess<
            lux::ecs::Write<BenchmarkPosition>
        >
    {
      public:
        void update(lux::ecs::SystemContext& frame) noexcept
        {
            for (auto [entity, position] :
                 frame.query<lux::ecs::Write<BenchmarkPosition>>())
            {
                position.value += lux::ecs::entityBits(entity) & 1U;
            }
        }
    };

    class NoopSystem final : public lux::ecs::StaticSystemAccess<>
    {
      public:
        explicit NoopSystem(std::uint64_t& value) noexcept : value_(&value) {}
        void update(lux::ecs::SystemContext&) noexcept { ++*value_; }

      private:
        std::uint64_t* value_{};
    };

    class DeclaredWriterSystem final
        : public lux::ecs::StaticSystemAccess<
            lux::ecs::Write<BenchmarkPosition>
        >
    {
    public:
        void update(lux::ecs::SystemContext&) noexcept {}
    };

    class OwnerNoopSystem final : public lux::ecs::StaticSystemAccess<>
    {
    public:
        using lux_thread_affine = std::true_type;

        explicit OwnerNoopSystem(std::uint64_t& value) noexcept
            : value_(&value)
        {
        }

        [[nodiscard]] bool isOnAffinityThread() const noexcept
        {
            return true;
        }

        void update(lux::ecs::SystemContext&) noexcept
        {
            ++*value_;
        }

    private:
        std::uint64_t* value_{};
    };

    struct TaskCounter final
    {
        std::uint64_t value{};
    };

    void countTask(void* target, void*) noexcept
    {
        ++static_cast<TaskCounter*>(target)->value;
    }

    enum class EGraphShape : std::uint8_t
    {
        NONE,
        CHAIN,
        DIAMOND
    };

    [[nodiscard]] lux::task::TaskGraph makeTaskGraph(
        std::size_t count,
        EGraphShape shape,
        TaskCounter& counter
    )
    {
        lux::task::TaskGraphBuilder builder;
        std::vector<lux::task::TaskId> ids;
        ids.reserve(count);
        for (std::size_t index{}; index < count; ++index)
        {
            const auto task = builder.addTask(
                {std::addressof(counter), &countTask},
                (index & 3U) == 0U
                    ? lux::task::ETaskAffinity::OWNER_THREAD
                    : lux::task::ETaskAffinity::WORKER
            );
            if (!task)
                std::abort();
            ids.push_back(*task);
        }

        if (shape == EGraphShape::CHAIN)
        {
            for (std::size_t index = 1U; index < ids.size(); ++index)
                if (!builder.addDependency(ids[index - 1U], ids[index]))
                    std::abort();
        }
        else if (shape == EGraphShape::DIAMOND && ids.size() >= 4U)
        {
            for (std::size_t index = 1U; index + 1U < ids.size(); ++index)
            {
                if (!builder.addDependency(ids.front(), ids[index]) ||
                    !builder.addDependency(ids[index], ids.back()))
                {
                    std::abort();
                }
            }
        }

        auto graph = std::move(builder).build();
        if (!graph)
            std::abort();
        return std::move(*graph);
    }

    void benchmarkTaskGraphBuild(Evidence& evidence, std::size_t count)
    {
        TaskCounter counter;
        for (const auto [name, shape] : {
                 std::pair{std::string_view{"none"}, EGraphShape::NONE},
                 std::pair{std::string_view{"chain"}, EGraphShape::CHAIN},
                 std::pair{std::string_view{"diamond"}, EGraphShape::DIAMOND}})
        {
            evidence.measure(
                std::string{"task_graph_build_"} + std::string{name},
                count,
                [&]
                {
                    auto graph = makeTaskGraph(count, shape, counter);
                    return Observation{
                        .dispatch_calls = graph.taskCount()
                    };
                }
            );
        }
    }

    void benchmarkTaskGraphExecute(Evidence& evidence, std::size_t count)
    {
        TaskCounter counter;
        for (const auto [name, shape] : {
                 std::pair{std::string_view{"none"}, EGraphShape::NONE},
                 std::pair{std::string_view{"chain"}, EGraphShape::CHAIN},
                 std::pair{std::string_view{"diamond"}, EGraphShape::DIAMOND}})
        {
            auto graph = makeTaskGraph(count, shape, counter);
            lux::task::TaskExecutionScratch scratch;
            if (!scratch.prepare(graph))
                std::abort();
            evidence.measure(
                std::string{"task_graph_execute_"} + std::string{name},
                count,
                [&]
                {
                    lux::task::executeTaskGraph(
                        lux::task::referenceTaskExecutionBackend(),
                        graph,
                        nullptr,
                        scratch
                    );
                    return Observation{
                        .dispatch_calls = graph.taskCount()
                    };
                }
            );
        }
        checksum = checksum + counter.value;
    }

    struct SystemCompileFixture final
    {
        explicit SystemCompileFixture(
            std::size_t count,
            EGraphShape shape,
            bool conflict
        )
            : relations(systems)
        {
            ids.reserve(count);
            for (std::size_t index{}; index < count; ++index)
            {
                const auto id = conflict
                    ? systems.emplace<DeclaredWriterSystem>()
                    : systems.emplace<NoopSystem>(updates);
                if (!id)
                    std::abort();
                ids.push_back(*id);
            }
            if (shape == EGraphShape::CHAIN)
            {
                for (std::size_t index = 1U; index < ids.size(); ++index)
                    if (!relations.before(ids[index - 1U], ids[index]))
                        std::abort();
            }
            else if (shape == EGraphShape::DIAMOND && ids.size() >= 4U)
            {
                for (std::size_t index = 1U; index + 1U < ids.size(); ++index)
                {
                    if (!relations.before(ids.front(), ids[index]) ||
                        !relations.before(ids[index], ids.back()))
                    {
                        std::abort();
                    }
                }
            }
        }

        std::uint64_t updates{};
        lux::ecs::SystemRegistry systems;
        lux::ecs::SystemRelations relations;
        std::vector<lux::ecs::SystemId> ids;
    };

    void benchmarkSystemCompile(Evidence& evidence, std::size_t count)
    {
        struct Case final
        {
            std::string_view name;
            EGraphShape shape;
            bool conflict;
        };
        for (const Case value : {
                 Case{"none", EGraphShape::NONE, false},
                 Case{"chain", EGraphShape::CHAIN, false},
                 Case{"diamond", EGraphShape::DIAMOND, false},
                 Case{"all_conflict", EGraphShape::NONE, true}})
        {
            SystemCompileFixture fixture(count, value.shape, value.conflict);
            lux::ecs::SystemTaskGraphCompiler compiler;
            evidence.measure(
                std::string{"system_compile_"} + std::string{value.name},
                count,
                [&]
                {
                    auto compilation = compiler.compile(
                        fixture.systems,
                        fixture.relations
                    );
                    if (!compilation)
                        std::abort();
                    return Observation{
                        .dispatch_calls = compilation->graph.taskCount()
                    };
                }
            );
        }
    }

    void benchmarkSystemExecute(Evidence& evidence, std::size_t count)
    {
        lux::ecs::World world;
        lux::ecs::detail::SystemTestRig execution(world);
        std::uint64_t updates{};
        for (std::size_t index{}; index < count; ++index)
        {
            if ((index & 3U) == 0U)
                (void)execution.add<OwnerNoopSystem>(updates);
            else
                (void)execution.add<NoopSystem>(updates);
        }
        if (!execution.compile())
            std::abort();
        std::uint64_t tick{};
        evidence.measure("system_execute_mixed_affinity", count, [&]
        {
            if (!execution.run(1.0F / 60.0F, ++tick))
                std::abort();
            return Observation{};
        });
        checksum = checksum + updates;
    }

    template <std::size_t... Index>
    class MultiWriteSystem final
        : public lux::ecs::StaticSystemAccess<
            lux::ecs::Write<BenchmarkWrite<Index>>...
        >
    {
    public:
        void update(lux::ecs::SystemContext& context) noexcept
        {
            auto query = context.query<
                lux::ecs::Write<BenchmarkWrite<Index>>...
            >();
            for (auto values : query)
                ((++std::get<Index + 1U>(values).value), ...);
        }
    };

    template <std::size_t... Index>
    [[nodiscard]] std::unique_ptr<lux::ecs::World> multiWriteWorld(
        std::size_t count,
        std::index_sequence<Index...>
    )
    {
        lux::ecs::WorldConfig config;
        constexpr std::size_t kRecordBudget = sizeof...(Index) * 16U;
        if (count <= (std::numeric_limits<std::size_t>::max)() /
                         (std::max)(kRecordBudget, std::size_t{1U}))
        {
            config.changes.max_bytes = (std::max)(
                config.changes.max_bytes,
                count * kRecordBudget
            );
        }
        auto world = std::make_unique<lux::ecs::World>(config);
        auto result = world->mutate();
        if (!result)
            std::abort();
        auto mutation = std::move(*result);
        (mutation.reserve<BenchmarkWrite<Index>>(count), ...);
        for (std::size_t row{}; row < count; ++row)
        {
            const auto entity = mutation.create();
            (mutation.emplace<BenchmarkWrite<Index>>(entity, row), ...);
        }
        mutation = {};
        lux::ecs::detail::establishWorldChangeBaseline(*world);
        return world;
    }

    template <std::size_t... Index>
    void benchmarkWorldMutationWrites(
        Evidence& evidence,
        std::size_t count,
        std::index_sequence<Index...> sequence
    )
    {
        auto world = multiWriteWorld(count, sequence);
        auto& journal = lux::ecs::detail::WorldChangeAccess::journal(*world);
        evidence.measure(
            "world_change_log_write_" + std::to_string(sizeof...(Index)),
            count,
            [&]
            {
                const auto bind_before = journal.streamBindCountForTest();
                const auto lookup_before = journal.perRecordLookupCountForTest();
                const auto epoch_before = journal.epoch();
                auto result = world->mutate();
                if (!result)
                    std::abort();
                auto mutation = std::move(*result);
                auto query = mutation.template query<
                    lux::ecs::Write<BenchmarkWrite<Index>>...
                >();
                for (auto values : query)
                    ((++std::get<Index + 1U>(values).value), ...);
                mutation = {};
                return Observation{
                    .dispatch_calls = static_cast<std::size_t>(
                        journal.streamBindCountForTest() - bind_before
                    ),
                    .storage_lookups = static_cast<std::size_t>(
                        journal.perRecordLookupCountForTest() - lookup_before
                    ),
                    .history_losses = static_cast<std::size_t>(
                        journal.epoch() - epoch_before
                    )
                };
            }
        );
    }

    template <std::size_t... Index>
    void benchmarkSystemWrites(
        Evidence& evidence,
        std::size_t count,
        std::index_sequence<Index...> sequence
    )
    {
        auto world = multiWriteWorld(count, sequence);
        lux::ecs::detail::SystemTestRig execution(*world);
        (void)execution.add<MultiWriteSystem<Index...>>();
        if (!execution.compile(count))
            std::abort();
        std::uint64_t tick{};
        evidence.measure(
            "system_write_query_" + std::to_string(sizeof...(Index)),
            count,
            [&]
            {
                const auto bind_before = execution.laneBindCount();
                const auto lookup_before = execution.perRecordLookupCount();
                if (!execution.run(1.0F / 60.0F, ++tick))
                    std::abort();
                return Observation{
                    .dispatch_calls = static_cast<std::size_t>(
                        execution.laneBindCount() - bind_before
                    ),
                    .storage_lookups = static_cast<std::size_t>(
                        execution.perRecordLookupCount() - lookup_before
                    )
                };
            }
        );
    }

    void benchmarkWorldChangeLog(Evidence& evidence, std::size_t count)
    {
        benchmarkWorldMutationWrites(
            evidence,
            count,
            std::make_index_sequence<1U>{}
        );
        benchmarkWorldMutationWrites(
            evidence,
            count,
            std::make_index_sequence<4U>{}
        );
        benchmarkWorldMutationWrites(
            evidence,
            count,
            std::make_index_sequence<16U>{}
        );
    }

    void benchmarkSystemWriteQuery(Evidence& evidence, std::size_t count)
    {
        benchmarkSystemWrites(
            evidence,
            count,
            std::make_index_sequence<1U>{}
        );
        benchmarkSystemWrites(
            evidence,
            count,
            std::make_index_sequence<4U>{}
        );
        benchmarkSystemWrites(
            evidence,
            count,
            std::make_index_sequence<16U>{}
        );
    }

    void benchmarkQuery(Evidence& evidence, std::size_t requested_size)
    {
        std::vector<std::size_t> sizes{requested_size};
        if (requested_size >= 1'000'000U)
            sizes.insert(sizes.begin(), 100'000U);
        for (const std::size_t size : sizes)
        {
            entt::basic_registry<lux::ecs::Entity> raw;
            for (std::size_t index{}; index < size; ++index)
                raw.emplace<BenchmarkPosition>(raw.create(), index);
            auto world = positionWorld(size);
            evidence.measure("raw_entt_read", size, [&]
            {
                std::uint64_t value{};
                for (std::size_t repeat{}; repeat < 8U; ++repeat)
                {
                    for (const auto [entity, position] :
                         raw.view<const BenchmarkPosition>().each())
                    {
                        value += position.value +
                            lux::ecs::entityBits(entity);
                    }
                }
                checksum = checksum + value;
                return Observation{};
            });
            evidence.measure("world_read_query", size, [&]
            {
                std::uint64_t value{};
                for (std::size_t repeat{}; repeat < 8U; ++repeat)
                {
                    for (const auto [entity, position] :
                         world->query<lux::ecs::Read<BenchmarkPosition>>())
                    {
                        value += position.value +
                            lux::ecs::entityBits(entity);
                    }
                }
                checksum = checksum + value;
                return Observation{};
            });
            evidence.measure("world_edit_write_query", size, [&]
            {
                auto result = world->mutate();
                auto edit = std::move(*result);
                for (auto [entity, position] :
                     edit.query<lux::ecs::Write<BenchmarkPosition>>())
                {
                    position.value += lux::ecs::entityBits(entity) & 1U;
                }
                return Observation{};
            });

            lux::ecs::detail::SystemTestRig schedule(*world);
            (void)schedule.add<PositionWriteSystem>();
            if (!schedule.compile(size)) std::abort();
            std::uint64_t tick{};
            evidence.measure("system_context_write_query", size, [&]
            {
                if (!schedule.run(1.0F / 60.0F, ++tick)) std::abort();
                if (schedule.perRecordLookupCount() != 0U) std::abort();
                return Observation{
                    0U,
                    schedule.systemCapacity(),
                    0U,
                    static_cast<std::size_t>(schedule.laneBindCount())
                };
            });
        }

        for (const std::size_t count : {1U, 16U, 64U, 256U, 1024U})
        {
            lux::ecs::World world;
            lux::ecs::detail::SystemTestRig schedule(world);
            std::uint64_t updates{};
            for (std::size_t index{}; index < count; ++index)
                (void)schedule.add<NoopSystem>(updates);
            if (!schedule.compile()) std::abort();
            std::uint64_t tick{};
            evidence.measure("system_taskgraph_run", count, [&]
            {
                for (std::size_t repeat{}; repeat < 1'000U; ++repeat)
                    if (!schedule.run(1.0F / 60.0F, ++tick)) std::abort();
                return Observation{};
            });
            checksum = checksum + updates;
        }
    }

    enum class EHierarchyShape : std::uint8_t { BALANCED, DEEP, STAR };

    std::pair<std::unique_ptr<lux::ecs::World>, std::vector<lux::ecs::Entity>>
    hierarchyWorld(
        std::size_t count,
        EHierarchyShape shape,
        lux::ecs::WorldConfig config = {}
    )
    {
        auto world = std::make_unique<lux::ecs::World>(config);
        std::vector<lux::ecs::Entity> entities;
        entities.reserve(count);
        auto result = world->mutate();
        auto edit = std::move(*result);
        edit.reserve<lux::ecs::Parent>(count == 0U ? 0U : count - 1U);
        for (std::size_t index{}; index < count; ++index)
        {
            const auto entity = edit.create();
            entities.push_back(entity);
            if (index == 0U) continue;
            std::size_t parent{};
            if (shape == EHierarchyShape::BALANCED) parent = (index - 1U) / 2U;
            else if (shape == EHierarchyShape::DEEP) parent = index - 1U;
            edit.emplace<lux::ecs::Parent>(entity, entities[parent]);
        }
        return {std::move(world), std::move(entities)};
    }

    void installHierarchy(
        lux::ecs::detail::SystemTestRig& schedule,
        lux::ecs::HierarchyIndex& hierarchy
    )
    {
        (void)schedule.add<lux::ecs::HierarchySystem>(hierarchy);
        if (!schedule.compile()) std::abort();
    }

    void benchmarkHierarchy(Evidence& evidence, std::size_t requested_size)
    {
        auto [world, entities] = hierarchyWorld(
            requested_size,
            EHierarchyShape::BALANCED
        );
        evidence.measure("hierarchy_balanced_initial_sync", requested_size, [&]
        {
            lux::ecs::HierarchyIndex hierarchy(*world);
            lux::ecs::detail::SystemTestRig schedule(*world);
            installHierarchy(schedule, hierarchy);
            if (!schedule.run(1.0F / 60.0F, 1U)) std::abort();
            if (!hierarchy.synchronized()) std::abort();
            return Observation{
                0U, 0U, 0U, 0U,
                lux::ecs::detail::HierarchyIndexTestAccess::visitedNodes(
                    hierarchy
                )
            };
        });
        lux::ecs::HierarchyIndex hierarchy(*world);
        lux::ecs::detail::SystemTestRig schedule(*world);
        installHierarchy(schedule, hierarchy);
        std::uint64_t tick{1U};
        if (!schedule.run(1.0F / 60.0F, tick)) std::abort();
        evidence.measure("hierarchy_real_no_change", requested_size, [&]
        {
            if (!schedule.run(1.0F / 60.0F, ++tick)) std::abort();
            const auto visited =
                lux::ecs::detail::HierarchyIndexTestAccess::visitedNodes(
                    hierarchy
                );
            if (visited != 0U) std::abort();
            return Observation{0U, 0U, 0U, 0U, visited};
        });

        const std::size_t stress = std::min<std::size_t>(
            requested_size,
            100'000U
        );
        for (const auto [metric, shape] : {
                 std::pair{std::string{"hierarchy_deep_initial_sync"},
                           EHierarchyShape::DEEP},
                 std::pair{std::string{"hierarchy_star_initial_sync"},
                           EHierarchyShape::STAR}})
        {
            auto [stress_world, stress_entities] = hierarchyWorld(stress, shape);
            evidence.measure(metric, stress, [&]
            {
                lux::ecs::HierarchyIndex local(*stress_world);
                lux::ecs::detail::SystemTestRig local_schedule(*stress_world);
                installHierarchy(local_schedule, local);
                if (!local_schedule.run(1.0F / 60.0F, 1U)) std::abort();
                if (!local.synchronized()) std::abort();
                return Observation{
                    0U, 0U, 0U, 0U,
                    lux::ecs::detail::HierarchyIndexTestAccess::visitedNodes(
                        local
                    )
                };
            });
            checksum = checksum + stress_entities.size();
        }
        auto [star_world, star_entities] = hierarchyWorld(
            stress,
            EHierarchyShape::STAR
        );
        lux::ecs::HierarchyIndex star_index(*star_world);
        lux::ecs::detail::SystemTestRig star_schedule(*star_world);
        installHierarchy(star_schedule, star_index);
        std::uint64_t star_tick{1U};
        if (!star_schedule.run(1.0F / 60.0F, star_tick)) std::abort();
        bool nested{};
        evidence.measure("hierarchy_star_reparent", stress, [&]
        {
            nested = !nested;
            auto result = star_world->mutate();
            auto edit = std::move(*result);
            edit.update<lux::ecs::Parent>(
                star_entities.back(),
                [&](lux::ecs::Parent& parent) noexcept
                {
                    parent.entity = nested
                        ? star_entities[1U]
                        : star_entities.front();
                }
            );
            edit = {};
            if (!star_schedule.run(1.0F / 60.0F, ++star_tick)) std::abort();
            if (!star_index.synchronized()) std::abort();
            return Observation{
                0U, 0U, 0U, 0U,
                lux::ecs::detail::HierarchyIndexTestAccess::visitedNodes(
                    star_index
                )
            };
        });

        const lux::ecs::WorldConfig tiny_history{
            lux::ecs::WorldChangeLogConfig{4096U, 4096U}};
        auto [resync_world, resync_entities] = hierarchyWorld(
            stress,
            EHierarchyShape::STAR,
            tiny_history
        );
        lux::ecs::HierarchyIndex resync_index(*resync_world);
        lux::ecs::detail::SystemTestRig resync_schedule(*resync_world);
        installHierarchy(resync_schedule, resync_index);
        std::uint64_t resync_tick{1U};
        if (!resync_schedule.run(1.0F / 60.0F, resync_tick)) std::abort();
        evidence.measure("hierarchy_cursor_overflow_resync", stress, [&]
        {
            auto result = resync_world->mutate();
            auto edit = std::move(*result);
            for (std::size_t index{}; index < 512U; ++index)
            {
                edit.update<lux::ecs::Parent>(
                    resync_entities.back(),
                    [](lux::ecs::Parent&) noexcept {}
                );
            }
            edit = {};
            if (!resync_schedule.run(1.0F / 60.0F, ++resync_tick)) std::abort();
            if (!resync_index.synchronized()) std::abort();
            return Observation{
                0U, 0U, 0U, 0U,
                lux::ecs::detail::HierarchyIndexTestAccess::visitedNodes(
                    resync_index
                )
            };
        });
        checksum = checksum + entities.size();
    }

    void addTransforms(
        lux::ecs::World& world,
        std::span<const lux::ecs::Entity> entities
    )
    {
        auto result = world.mutate();
        auto edit = std::move(*result);
        edit.reserve<lux::ecs::Transform3D>(entities.size());
        for (const auto entity : entities)
            edit.emplace<lux::ecs::Transform3D>(entity);
    }

    void benchmarkTransform(Evidence& evidence, std::size_t requested_size)
    {
        for (const auto [metric, shape] : {
                 std::pair{std::string{"transform_large_root_subtree"},
                           EHierarchyShape::STAR},
                 std::pair{std::string{"transform_deep_propagation"},
                           EHierarchyShape::DEEP}})
        {
            auto [world, entities] = hierarchyWorld(requested_size, shape);
            addTransforms(*world, entities);
            lux::ecs::HierarchyIndex hierarchy(*world);
            lux::ecs::detail::SystemTestRig schedule(*world);
            const auto hierarchy_handle =
                schedule.add<lux::ecs::HierarchySystem>(hierarchy);
            const auto transform_handle =
                schedule.add<lux::ecs::Transform3DSystem>(hierarchy);
            schedule.before(hierarchy_handle, transform_handle);
            if (!schedule.compile()) std::abort();
            auto* transform = std::addressof(
                schedule.system<lux::ecs::Transform3DSystem>(transform_handle)
            );
            std::uint64_t tick{1U};
            if (!schedule.run(1.0F / 60.0F, tick)) std::abort();
            evidence.measure(metric, requested_size, [&]
            {
                auto update_result = world->mutate();
                auto update = std::move(*update_result);
                update.update<lux::ecs::Transform3D>(
                    entities.front(),
                    [](lux::ecs::Transform3D& value) noexcept
                    {
                        value.translation.x() += 1.0F;
                    }
                );
                update = {};
                if (!schedule.run(1.0F / 60.0F, ++tick)) std::abort();
                return Observation{
                    0U,
                    lux::ecs::detail::TransformSystemTestAccess::
                        retainedDenseBytes(*transform),
                    0U,
                    0U,
                    lux::ecs::detail::TransformSystemTestAccess::visitedNodes(
                        *transform
                    )
                };
            });
        }

        const std::size_t active = std::max<std::size_t>(
            1U,
            requested_size / 10U
        );
        auto sparse_world = std::make_unique<lux::ecs::World>();
        std::vector<lux::ecs::Entity> sparse_entities;
        sparse_entities.reserve(requested_size);
        auto setup_result = sparse_world->mutate();
        auto setup = std::move(*setup_result);
        setup.reserve<lux::ecs::Transform3D>(requested_size);
        for (std::size_t index{}; index < requested_size; ++index)
        {
            const auto entity = setup.create();
            setup.emplace<lux::ecs::Transform3D>(entity);
            sparse_entities.push_back(entity);
        }
        for (std::size_t index{}; index < requested_size - active; ++index)
            setup.destroy(sparse_entities[index]);
        setup = {};

        lux::ecs::HierarchyIndex sparse_hierarchy(*sparse_world);
        lux::ecs::detail::SystemTestRig sparse_schedule(*sparse_world);
        const auto hierarchy_handle =
            sparse_schedule.add<lux::ecs::HierarchySystem>(sparse_hierarchy);
        const auto transform_handle =
            sparse_schedule.add<lux::ecs::Transform3DSystem>(sparse_hierarchy);
        sparse_schedule.before(hierarchy_handle, transform_handle);
        if (!sparse_schedule.compile()) std::abort();
        auto* transform = std::addressof(
            sparse_schedule.system<lux::ecs::Transform3DSystem>(
                transform_handle
            )
        );
        std::uint64_t sparse_tick{1U};
        if (!sparse_schedule.run(1.0F / 60.0F, sparse_tick)) std::abort();
        evidence.measure("transform_sparse_high_water", active, [&]
        {
            auto result = sparse_world->mutate();
            auto edit = std::move(*result);
            edit.update<lux::ecs::Transform3D>(
                sparse_entities.back(),
                [](lux::ecs::Transform3D& value) noexcept
                {
                    value.translation.y() += 1.0F;
                }
            );
            edit = {};
            if (!sparse_schedule.run(
                    1.0F / 60.0F,
                    ++sparse_tick
                ))
            {
                std::abort();
            }
            return Observation{
                0U,
                lux::ecs::detail::TransformSystemTestAccess::retainedDenseBytes(
                    *transform
                ),
                0U,
                0U,
                lux::ecs::detail::TransformSystemTestAccess::visitedNodes(
                    *transform
                )
            };
        });
    }

    void benchmarkSnapshot(Evidence& evidence, std::size_t requested_size)
    {
        const auto draft = lux::ecs::makeComponentSchema<BenchmarkPosition>(
            lux::ecs::componentSchemaId("benchmark.snapshot.position")
        );
        auto schemas = lux::ecs::ComponentSchemaSet::build({draft});
        const auto* schema = schemas->find(draft.id);
        const std::array bindings{
            lux::ecs::bindComponentSnapshot<BenchmarkPosition>(*schema)};
        const lux::ecs::ComponentSnapshotContribution contribution{{}, bindings};
        auto components = lux::ecs::ComponentSnapshotSet::build(
            *schemas,
            std::span(&contribution, 1U)
        );
        if (!components) std::abort();
        std::vector<std::size_t> sizes{requested_size};
        if (requested_size >= 1'000'000U)
            sizes = {10'000U, 100'000U, requested_size};
        for (const std::size_t size : sizes)
        {
            auto source = positionWorld(size);
            evidence.measure("snapshot_capture", size, [&]
            {
                lux::ecs::detail::ComponentSnapshotTestStats::reset();
                auto snapshot = lux::ecs::WorldSnapshot::capture(
                    *source,
                    *components
                );
                if (!snapshot) std::abort();
                return Observation{
                    0U, 0U,
                    lux::ecs::detail::ComponentSnapshotTestStats::clone_calls,
                    lux::ecs::detail::ComponentSnapshotTestStats::storage_lookups
                };
            });
            auto snapshot = lux::ecs::WorldSnapshot::capture(
                *source,
                *components
            );
            evidence.measure("snapshot_instantiate", size, [&]
            {
                if (!snapshot->instantiate()) std::abort();
                return Observation{};
            });
            auto target = std::make_unique<lux::ecs::World>();
            evidence.measure("snapshot_restore", size, [&]
            {
                if (!snapshot->restore(*target)) std::abort();
                return Observation{};
            });
        }
    }

    [[nodiscard]] lux::ecs::WorldSectionId benchmarkSectionId()
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[0] = 0x42U;
        bytes[15] = 0x7fU;
        return lux::ecs::WorldSectionId{uuids::uuid(bytes)};
    }

    struct LoadPlan final
    {
        lux::ecs::ComponentSchemaSet schemas;
        lux::ecs::ComponentLoadSet loads;
        lux::ecs::WorldSectionImage image;
        std::size_t columns{};
    };

    [[nodiscard]] lux::cxx::expected<
        lux::ecs::WorldSectionInstance,
        lux::ecs::WorldSectionFailure>
    loadBenchmarkSection(
        lux::ecs::World& world,
        const lux::ecs::ComponentLoadSet& loads,
        const lux::ecs::WorldSectionImage& image
    ) noexcept
    {
        auto begun = lux::ecs::WorldSectionLoader::begin(
            world,
            lux::ecs::world_section::test::fixtureLoadScratchBudget(),
            lux::serialization::SerializationLimits{}
        );
        if (!begun)
            return lux::cxx::unexpected(begun.error());
        lux::ecs::WorldSectionInstance instance;
        auto staged = begun->load(loads, image, instance);
        if (!staged)
            return lux::cxx::unexpected(staged.error());
        auto committed = begun->commit();
        if (!committed)
            return lux::cxx::unexpected(committed.error());
        return std::move(instance);
    }

    [[nodiscard]] bool unloadBenchmarkSection(
        lux::ecs::World& world,
        lux::ecs::WorldSectionInstance& instance
    ) noexcept
    {
        auto begun = lux::ecs::WorldSectionLoader::begin(
            world,
            lux::ecs::world_section::test::fixtureLoadScratchBudget(),
            lux::serialization::SerializationLimits{}
        );
        return begun && begun->unload(instance) && begun->commit();
    }

    template <std::size_t... Index>
    [[nodiscard]] LoadPlan makeFixedPlanImpl(
        std::size_t entity_count,
        std::size_t column_count,
        std::size_t density,
        std::index_sequence<Index...>
    )
    {
        std::vector<lux::ecs::ComponentSchema> drafts;
        drafts.reserve(column_count);
        const auto append_schema = [&]<std::size_t Current>()
        {
            if (Current < column_count)
            {
                drafts.push_back(
                    lux::ecs::makeComponentSchema<BenchmarkFixed32<Current>>(
                        lux::ecs::componentSchemaId(
                            "benchmark.fixed32." + std::to_string(Current)
                        )
                    )
                );
            }
        };
        (append_schema.template operator()<Index>(), ...);
        auto schemas = lux::ecs::ComponentSchemaSet::build(std::move(drafts));
        if (!schemas) std::abort();

        std::vector<lux::ecs::ComponentLoadBinding> bindings;
        bindings.reserve(column_count);
        const auto append_binding = [&]<std::size_t Current>()
        {
            if (Current < column_count)
            {
                const auto id = lux::ecs::componentSchemaId(
                    "benchmark.fixed32." + std::to_string(Current)
                );
                bindings.push_back(
                    lux::ecs::bindComponentLoad<BenchmarkFixed32<Current>>(
                        *schemas->find(id)
                    )
                );
            }
        };
        (append_binding.template operator()<Index>(), ...);
        const lux::ecs::ComponentLoadContribution contribution{{}, bindings};
        auto loads = lux::ecs::ComponentLoadSet::build(
            *schemas,
            std::span(&contribution, 1U)
        );
        if (!loads) std::abort();

        const std::size_t rows = entity_count * density / 100U;
        std::vector<lux::ecs::world_section::test::FixtureColumn> columns;
        columns.reserve(column_count);
        for (std::size_t index{}; index < column_count; ++index)
        {
            lux::ecs::world_section::test::FixtureColumn column;
            column.schema_name =
                "benchmark.fixed32." + std::to_string(index);
            column.value_encoding =
                lux::ecs::EWorldSectionValueEncoding::FIXED;
            column.fixed_stride = 32U;
            if (density == 100U)
            {
                column.ordinal_encoding =
                    lux::ecs::EWorldSectionOrdinalEncoding::DENSE;
            }
            else
            {
                column.ordinal_encoding =
                    lux::ecs::EWorldSectionOrdinalEncoding::U32_LIST;
                column.ordinals.reserve(rows);
                for (std::size_t row{}; row < rows; ++row)
                {
                    column.ordinals.push_back(static_cast<std::uint32_t>(
                        row * entity_count / std::max<std::size_t>(1U, rows)
                    ));
                }
            }
            column.payload.resize(rows * 32U);
            columns.push_back(std::move(column));
        }
        auto bytes = lux::ecs::world_section::test::buildFixture(
            benchmarkSectionId(),
            static_cast<std::uint32_t>(entity_count),
            std::move(columns)
        );
        auto image = lux::ecs::WorldSectionImage::open(
            std::move(bytes),
            lux::ecs::world_section::test::fixtureValidationBudget()
        );
        if (!image) std::abort();
        return LoadPlan{
            std::move(*schemas),
            std::move(*loads),
            std::move(*image),
            column_count
        };
    }

    [[nodiscard]] LoadPlan makeFixedPlan(
        std::size_t entity_count,
        std::size_t column_count,
        std::size_t density = 100U
    )
    {
        if (column_count == 0U || column_count > 64U) std::abort();
        return makeFixedPlanImpl(
            entity_count,
            column_count,
            density,
            std::make_index_sequence<64>{}
        );
    }

    template <class Component>
    [[nodiscard]] LoadPlan makeSinglePlan(
        std::size_t entity_count,
        std::string schema_name,
        lux::ecs::world_section::test::FixtureColumn column
    )
    {
        const auto id = lux::ecs::componentSchemaId(schema_name);
        auto schemas = lux::ecs::ComponentSchemaSet::build({
            lux::ecs::makeComponentSchema<Component>(id)
        });
        if (!schemas) std::abort();
        const std::array bindings{
            lux::ecs::bindComponentLoad<Component>(*schemas->find(id))};
        const lux::ecs::ComponentLoadContribution contribution{{}, bindings};
        auto loads = lux::ecs::ComponentLoadSet::build(
            *schemas,
            std::span(&contribution, 1U)
        );
        if (!loads) std::abort();
        column.schema_name = std::move(schema_name);
        auto bytes = lux::ecs::world_section::test::buildFixture(
            benchmarkSectionId(),
            static_cast<std::uint32_t>(entity_count),
            {std::move(column)}
        );
        auto image = lux::ecs::WorldSectionImage::open(
            std::move(bytes),
            lux::ecs::world_section::test::fixtureValidationBudget()
        );
        if (!image) std::abort();
        return LoadPlan{
            std::move(*schemas),
            std::move(*loads),
            std::move(*image),
            1U
        };
    }

    void measureLoadPlan(
        Evidence& evidence,
        const std::string& prefix,
        std::size_t size,
        LoadPlan& plan,
        bool include_phases
    )
    {
        if (include_phases)
        {
            evidence.measure(prefix + "_open", size, [&]
            {
                const auto source = plan.image.bytes();
                std::vector<std::byte> bytes(source.begin(), source.end());
                const auto begin = Clock::now();
                auto opened = lux::ecs::WorldSectionImage::open(
                    std::move(bytes),
                    lux::ecs::world_section::test::fixtureValidationBudget()
                );
                const auto end = Clock::now();
                if (!opened) std::abort();
                return Observation{static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        end - begin
                    ).count()
                )};
            });
            evidence.measure(prefix + "_preflight", size, [&]
            {
                std::size_t found{};
                for (const auto& column : plan.image.columns())
                {
                    const auto* binding = plan.loads.find(
                        column.schemaHash(),
                        column.schemaName()
                    );
                    if (binding == nullptr ||
                        binding->schema().version != column.schemaVersion() ||
                        binding->valueEncoding() != column.valueEncoding())
                    {
                        std::abort();
                    }
                    ++found;
                }
                checksum = checksum + found;
                return Observation{};
            });
            evidence.measure(prefix + "_entity_create", size, [&]
            {
                lux::ecs::World world;
                std::vector<lux::ecs::Entity> entities(size);
                auto edit = lux::ecs::detail::WorldColdAccess::sectionEdit(world);
                lux::ecs::detail::WorldSectionTransactionAccess::createEntities(
                    edit,
                    entities
                );
                return Observation{};
            });

            const auto fixed_column = std::find_if(
                plan.image.columns().begin(),
                plan.image.columns().end(),
                [](const lux::ecs::WorldSectionColumnView& column)
                {
                    return column.schemaName() == "benchmark.fixed32.0";
                }
            );
            if (fixed_column == plan.image.columns().end()) std::abort();
            evidence.measure(prefix + "_decode", size, [&]
            {
                std::uint64_t sum{};
                for (std::size_t row{}; row < fixed_column->rowCount(); ++row)
                {
                    lux::serialization::BinaryReader reader(
                        fixed_column->payload().subspan(row * 32U, 32U)
                    );
                    auto value = lux::serialization::read<BenchmarkFixed32<0>>(
                        reader
                    );
                    if (!value || reader.remaining() != 0U) std::abort();
                    sum += value->words[0];
                }
                checksum = checksum + sum;
                return Observation{};
            });

            std::vector<lux::ecs::Entity> entities(size);
            std::vector<BenchmarkFixed32<0>> values(size);
            evidence.measure(prefix + "_raw_entt_insert", size, [&]
            {
                entt::basic_registry<lux::ecs::Entity> registry;
                registry.create(entities.begin(), entities.end());
                auto local_values = values;
                registry.storage<BenchmarkFixed32<0>>().insert(
                    entities.begin(),
                    entities.end(),
                    std::make_move_iterator(local_values.begin())
                );
                return Observation{};
            });
            evidence.measure(prefix + "_predecoded_fixed_insert", size, [&]
            {
                lux::ecs::World world;
                auto local_entities = entities;
                auto local_values = values;
                auto edit = lux::ecs::detail::WorldColdAccess::sectionEdit(
                    world
                );
                lux::ecs::detail::WorldSectionTransactionAccess::createEntities(
                    edit,
                    local_entities
                );
                lux::ecs::detail::WorldSectionTransactionAccess::
                    insertPredecodedForBenchmark<BenchmarkFixed32<0>>(
                        edit,
                        local_entities,
                        local_values
                    );
                return Observation{};
            });
            evidence.measure(prefix + "_world_edit_insert", size, [&]
            {
                lux::ecs::World world;
                auto edit_result = world.mutate();
                if (!edit_result) std::abort();
                auto edit = std::move(*edit_result);
                edit.reserve<BenchmarkFixed32<0>>(size);
                for (std::size_t index{}; index < size; ++index)
                {
                    const auto entity = edit.create();
                    edit.emplace<BenchmarkFixed32<0>>(entity, values[index]);
                }
                return Observation{};
            });
        }

        evidence.measure(prefix + "_full_load", size, [&]
        {
            lux::ecs::World world;
            lux::ecs::detail::ComponentLoadTestStats::reset();
            auto instance = loadBenchmarkSection(
                world,
                plan.loads,
                plan.image
            );
            if (!instance) std::abort();
            if (lux::ecs::detail::ComponentLoadTestStats::load_calls !=
                    plan.columns ||
                lux::ecs::detail::ComponentLoadTestStats::storage_lookups !=
                    plan.columns)
            {
                std::abort();
            }
            const Observation result{
                0U,
                0U,
                lux::ecs::detail::ComponentLoadTestStats::load_calls,
                lux::ecs::detail::ComponentLoadTestStats::storage_lookups
            };
            if (!unloadBenchmarkSection(world, *instance))
                std::abort();
            return result;
        });
        if (include_phases)
        {
            evidence.measure(prefix + "_unload", size, [&]
            {
                lux::ecs::World world;
                auto instance = loadBenchmarkSection(
                    world,
                    plan.loads,
                    plan.image
                );
                if (!instance) std::abort();
                const auto begin = Clock::now();
                if (!unloadBenchmarkSection(world, *instance))
                    std::abort();
                const auto end = Clock::now();
                return Observation{static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        end - begin
                    ).count()
                )};
            });
        }
    }

    void appendU32(
        std::vector<std::byte>& bytes,
        std::uint32_t value
    )
    {
        for (std::size_t index{}; index < sizeof(value); ++index)
        {
            bytes.push_back(static_cast<std::byte>(value & 0xffU));
            value >>= 8U;
        }
    }

    void appendFloat(std::vector<std::byte>& bytes, float value)
    {
        appendU32(bytes, std::bit_cast<std::uint32_t>(value));
    }

    [[nodiscard]] LoadPlan makeLiveStreamingPlan(std::size_t entity_count)
    {
        const auto parent_id = lux::ecs::componentSchemaId("lux.ecs.Parent");
        const auto transform_id =
            lux::ecs::componentSchemaId("lux.ecs.Transform3D");
        auto schemas = lux::ecs::ComponentSchemaSet::build({
            lux::ecs::makeComponentSchema<lux::ecs::Parent>(parent_id),
            lux::ecs::makeComponentSchema<lux::ecs::Transform3D>(transform_id),
        });
        if (!schemas) std::abort();
        const std::array bindings{
            lux::ecs::bindComponentLoad<lux::ecs::Parent>(
                *schemas->find(parent_id)
            ),
            lux::ecs::bindComponentLoad<lux::ecs::Transform3D>(
                *schemas->find(transform_id)
            ),
        };
        const lux::ecs::ComponentLoadContribution contribution{{}, bindings};
        auto loads = lux::ecs::ComponentLoadSet::build(
            *schemas,
            std::span(&contribution, 1U)
        );
        if (!loads) std::abort();

        lux::ecs::world_section::test::FixtureColumn parent;
        parent.schema_name = "lux.ecs.Parent";
        parent.value_encoding = lux::ecs::EWorldSectionValueEncoding::FIXED;
        parent.ordinal_encoding =
            lux::ecs::EWorldSectionOrdinalEncoding::U32_LIST;
        parent.fixed_stride = 4U;
        parent.ordinals.reserve(entity_count == 0U ? 0U : entity_count - 1U);
        parent.payload.reserve(
            (entity_count == 0U ? 0U : entity_count - 1U) * 4U
        );
        for (std::size_t index = 1U; index < entity_count; ++index)
        {
            parent.ordinals.push_back(static_cast<std::uint32_t>(index));
            appendU32(
                parent.payload,
                static_cast<std::uint32_t>((index - 1U) / 2U)
            );
        }

        lux::ecs::world_section::test::FixtureColumn transform;
        transform.schema_name = "lux.ecs.Transform3D";
        transform.value_encoding =
            lux::ecs::EWorldSectionValueEncoding::FIXED;
        transform.fixed_stride = 40U;
        transform.payload.reserve(entity_count * transform.fixed_stride);
        for (std::size_t index{}; index < entity_count; ++index)
        {
            appendFloat(transform.payload, 0.0F);
            appendFloat(transform.payload, 0.0F);
            appendFloat(transform.payload, 0.0F);
            appendFloat(transform.payload, 0.0F);
            appendFloat(transform.payload, 0.0F);
            appendFloat(transform.payload, 0.0F);
            appendFloat(transform.payload, 1.0F);
            appendFloat(transform.payload, 1.0F);
            appendFloat(transform.payload, 1.0F);
            appendFloat(transform.payload, 1.0F);
        }

        auto bytes = lux::ecs::world_section::test::buildFixture(
            benchmarkSectionId(),
            static_cast<std::uint32_t>(entity_count),
            {std::move(parent), std::move(transform)}
        );
        auto image = lux::ecs::WorldSectionImage::open(
            std::move(bytes),
            lux::ecs::world_section::test::fixtureValidationBudget()
        );
        if (!image) std::abort();
        return LoadPlan{
            std::move(*schemas),
            std::move(*loads),
            std::move(*image),
            2U
        };
    }

    void benchmarkLiveStreaming(
        Evidence& evidence,
        std::size_t resident_count
    )
    {
        const std::size_t section_count = std::min<std::size_t>(
            resident_count,
            10'000U
        );
        auto plan = makeLiveStreamingPlan(section_count);
        lux::ecs::World world;
        std::vector<lux::ecs::Entity> residents(
            resident_count,
            lux::ecs::NullEntity
        );
        {
            auto edit = lux::ecs::detail::WorldColdAccess::sectionEdit(world);
            lux::ecs::detail::WorldSectionTransactionAccess::createEntities(
                edit,
                residents
            );
        }

        lux::ecs::HierarchyIndex hierarchy(world);
        lux::ecs::detail::SystemTestRig schedule(world);
        const auto hierarchy_handle =
            schedule.add<lux::ecs::HierarchySystem>(hierarchy);
        const auto transform_handle =
            schedule.add<lux::ecs::Transform3DSystem>(hierarchy);
        schedule.before(hierarchy_handle, transform_handle);
        if (!schedule.compile()) std::abort();
        auto* transform = std::addressof(
            schedule.system<lux::ecs::Transform3DSystem>(transform_handle)
        );
        std::uint64_t tick{1U};
        if (!schedule.run(1.0F / 60.0F, tick)) std::abort();

        evidence.measure(
            "world_section_live_resident_reconcile",
            resident_count,
            [&]
            {
                const auto epoch = lux::ecs::detail::worldChangeEpoch(world);
                const auto begin = Clock::now();
                auto instance = loadBenchmarkSection(
                    world,
                    plan.loads,
                    plan.image
                );
                if (!instance) std::abort();
                if (!schedule.run(1.0F / 60.0F, ++tick)) std::abort();
                const auto end = Clock::now();
                if (lux::ecs::detail::worldChangeEpoch(world) != epoch)
                    std::abort();

                const std::size_t hierarchy_visited =
                    lux::ecs::detail::HierarchyIndexTestAccess::visitedNodes(
                        hierarchy
                    );
                const std::size_t transform_visited =
                    lux::ecs::detail::TransformSystemTestAccess::visitedNodes(
                        *transform
                    );
                if (!unloadBenchmarkSection(world, *instance))
                    std::abort();
                if (!schedule.run(1.0F / 60.0F, ++tick)) std::abort();
                if (lux::ecs::detail::worldChangeEpoch(world) != epoch)
                    std::abort();
                if (!schedule.run(1.0F / 60.0F, ++tick)) std::abort();
                if (lux::ecs::detail::HierarchyIndexTestAccess::visitedNodes(
                        hierarchy
                    ) != 0U ||
                    lux::ecs::detail::TransformSystemTestAccess::visitedNodes(
                        *transform
                    ) != 0U)
                {
                    std::abort();
                }
                return Observation{
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            end - begin
                        ).count()
                    ),
                    lux::ecs::detail::TransformSystemTestAccess::
                        retainedDenseBytes(*transform),
                    2U,
                    2U,
                    hierarchy_visited + transform_visited,
                    0U
                };
            }
        );
        checksum = checksum + residents.size();
    }

    [[nodiscard]] std::vector<std::byte> dynamicPayload(
        std::size_t rows,
        std::size_t text_bytes,
        std::vector<std::uint32_t>& offsets
    )
    {
        std::vector<std::byte> payload;
        payload.reserve(rows * (16U + text_bytes));
        offsets.reserve(rows + 1U);
        offsets.push_back(0U);
        for (std::size_t row{}; row < rows; ++row)
        {
            for (std::size_t byte{}; byte < 8U; ++byte)
            {
                payload.push_back(static_cast<std::byte>(
                    (row >> (byte * 8U)) & 0xffU
                ));
            }
            for (std::size_t byte{}; byte < 8U; ++byte)
            {
                payload.push_back(static_cast<std::byte>(
                    (text_bytes >> (byte * 8U)) & 0xffU
                ));
            }
            payload.insert(payload.end(), text_bytes, std::byte{'x'});
            offsets.push_back(static_cast<std::uint32_t>(payload.size()));
        }
        return payload;
    }

    template <std::size_t Count>
    [[nodiscard]] LoadPlan makeReferencePlan(std::size_t entity_count)
    {
        lux::ecs::world_section::test::FixtureColumn column;
        column.value_encoding =
            lux::ecs::EWorldSectionValueEncoding::FIXED;
        column.ordinal_encoding =
            lux::ecs::EWorldSectionOrdinalEncoding::DENSE;
        column.fixed_stride = static_cast<std::uint32_t>(Count * 4U);
        column.payload.reserve(entity_count * Count * 4U);
        for (std::size_t row{}; row < entity_count; ++row)
        {
            for (std::size_t reference{}; reference < Count; ++reference)
            {
                const auto ordinal = static_cast<std::uint32_t>(
                    (row + reference) % entity_count
                );
                for (std::size_t byte{}; byte < 4U; ++byte)
                {
                    column.payload.push_back(static_cast<std::byte>(
                        (ordinal >> (byte * 8U)) & 0xffU
                    ));
                }
            }
        }
        return makeSinglePlan<BenchmarkEntityReferences<Count>>(
            entity_count,
            "benchmark.entity_refs." + std::to_string(Count),
            std::move(column)
        );
    }

    void benchmarkWorldSection(Evidence& evidence, std::size_t requested_size)
    {
        std::vector<std::size_t> scaling{requested_size};
        if (requested_size >= 1'000'000U)
            scaling.insert(scaling.begin(), 100'000U);
        for (const std::size_t size : scaling)
        {
            auto plan = makeFixedPlan(size, 3U);
            measureLoadPlan(
                evidence,
                "world_section_dense_fixed3",
                size,
                plan,
                true
            );
        }

        const std::size_t matrix = std::min<std::size_t>(
            requested_size,
            100'000U
        );
        for (const std::size_t columns : {3U, 16U, 64U})
        {
            auto plan = makeFixedPlan(matrix, columns);
            measureLoadPlan(
                evidence,
                "world_section_columns_" + std::to_string(columns),
                matrix,
                plan,
                false
            );
        }
        if (requested_size >= 1'000'000U)
        {
            for (const std::size_t density : {1U, 10U, 50U, 100U})
            {
                auto plan = makeFixedPlan(requested_size, 16U, density);
                measureLoadPlan(
                    evidence,
                    "world_section_density_" + std::to_string(density),
                    requested_size,
                    plan,
                    false
                );
            }
        }

        lux::ecs::world_section::test::FixtureColumn tag_column;
        tag_column.value_encoding = lux::ecs::EWorldSectionValueEncoding::TAG;
        tag_column.ordinal_encoding =
            lux::ecs::EWorldSectionOrdinalEncoding::DENSE;
        auto tag = makeSinglePlan<BenchmarkTag>(
            matrix,
            "benchmark.tag",
            std::move(tag_column)
        );
        measureLoadPlan(
            evidence,
            "world_section_payload_tag",
            matrix,
            tag,
            false
        );
        for (const std::size_t fixed_bytes : {16U, 64U})
        {
            lux::ecs::world_section::test::FixtureColumn column;
            column.value_encoding =
                lux::ecs::EWorldSectionValueEncoding::FIXED;
            column.ordinal_encoding =
                lux::ecs::EWorldSectionOrdinalEncoding::DENSE;
            column.fixed_stride = static_cast<std::uint32_t>(fixed_bytes);
            column.payload.resize(matrix * fixed_bytes);
            std::optional<LoadPlan> plan;
            if (fixed_bytes == 16U)
            {
                plan.emplace(makeSinglePlan<BenchmarkFixed16>(
                    matrix,
                    "benchmark.fixed16",
                    std::move(column)
                ));
            }
            else
            {
                plan.emplace(makeSinglePlan<BenchmarkFixed64>(
                    matrix,
                    "benchmark.fixed64",
                    std::move(column)
                ));
            }
            measureLoadPlan(
                evidence,
                "world_section_payload_fixed_" +
                    std::to_string(fixed_bytes),
                matrix,
                *plan,
                false
            );
        }
        for (const std::size_t text_bytes : {32U, 256U})
        {
            lux::ecs::world_section::test::FixtureColumn column;
            column.value_encoding =
                lux::ecs::EWorldSectionValueEncoding::VARIABLE;
            column.ordinal_encoding =
                lux::ecs::EWorldSectionOrdinalEncoding::DENSE;
            column.payload = dynamicPayload(
                matrix,
                text_bytes,
                column.offsets
            );
            auto plan = makeSinglePlan<BenchmarkDynamicPayload>(
                matrix,
                "benchmark.dynamic." + std::to_string(text_bytes),
                std::move(column)
            );
            measureLoadPlan(
                evidence,
                "world_section_payload_dynamic_" +
                    std::to_string(text_bytes),
                matrix,
                plan,
                false
            );
        }

        lux::ecs::world_section::test::FixtureColumn refs0_column;
        refs0_column.value_encoding =
            lux::ecs::EWorldSectionValueEncoding::TAG;
        refs0_column.ordinal_encoding =
            lux::ecs::EWorldSectionOrdinalEncoding::DENSE;
        auto refs0 = makeSinglePlan<BenchmarkTag>(
            matrix,
            "benchmark.entity_refs.0",
            std::move(refs0_column)
        );
        measureLoadPlan(
            evidence,
            "world_section_entity_refs_0",
            matrix,
            refs0,
            false
        );
        auto refs1 = makeReferencePlan<1U>(requested_size);
        measureLoadPlan(
            evidence,
            "world_section_entity_refs_1",
            requested_size,
            refs1,
            false
        );
        auto refs4 = makeReferencePlan<4U>(requested_size);
        measureLoadPlan(
            evidence,
            "world_section_entity_refs_4",
            requested_size,
            refs4,
            false
        );
        benchmarkLiveStreaming(evidence, requested_size);
    }
} // namespace

void* operator new(std::size_t size)
{
    if (count_allocations.load(std::memory_order_relaxed))
        allocation_count.fetch_add(1U, std::memory_order_relaxed);
    if (void* value = std::malloc(size == 0U ? 1U : size)) return value;
    throw std::bad_alloc();
}

void operator delete(void* value) noexcept { std::free(value); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* value) noexcept { ::operator delete(value); }

int main(int argc, char** argv)
{
    auto options = parseOptions(argc, argv);
    if (!options)
    {
        std::cerr <<
            "usage: ecs_l1_benchmark --group "
            "query|hierarchy|transform|snapshot|world-section|"
            "task-graph-build|task-graph-execute|system-compile|"
            "system-execute|world-change-log|system-write-query "
            "--mode diagnostic|qualification --size N --output file.csv\n";
        return 2;
    }
    try
    {
        Evidence evidence(*options);
        switch (options->group)
        {
        case EGroup::QUERY:
            benchmarkQuery(evidence, options->size);
            break;
        case EGroup::HIERARCHY:
            benchmarkHierarchy(evidence, options->size);
            break;
        case EGroup::TRANSFORM:
            benchmarkTransform(evidence, options->size);
            break;
        case EGroup::SNAPSHOT:
            benchmarkSnapshot(evidence, options->size);
            break;
        case EGroup::WORLD_SECTION:
            benchmarkWorldSection(evidence, options->size);
            break;
        case EGroup::TASK_GRAPH_BUILD:
            benchmarkTaskGraphBuild(evidence, options->size);
            break;
        case EGroup::TASK_GRAPH_EXECUTE:
            benchmarkTaskGraphExecute(evidence, options->size);
            break;
        case EGroup::SYSTEM_COMPILE:
            benchmarkSystemCompile(evidence, options->size);
            break;
        case EGroup::SYSTEM_EXECUTE:
            benchmarkSystemExecute(evidence, options->size);
            break;
        case EGroup::WORLD_CHANGE_LOG:
            benchmarkWorldChangeLog(evidence, options->size);
            break;
        case EGroup::SYSTEM_WRITE_QUERY:
            benchmarkSystemWriteQuery(evidence, options->size);
            break;
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 4;
    }
    return 0;
}
