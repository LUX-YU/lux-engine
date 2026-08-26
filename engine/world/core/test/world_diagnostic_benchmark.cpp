#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/WorldPartition.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <new>
#include <optional>
#include <span>
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
    using namespace lux::world;

    std::atomic_bool g_count_allocations{};
    std::atomic_size_t g_allocations{};

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
        std::size_t objects{10'000U};
        std::size_t data_records{1U};
        std::size_t partitions{16U};
        std::string output{"world_diagnostic.csv"};
    };

    [[nodiscard]] bool parseSize(
        std::string_view value,
        std::size_t& output
    ) noexcept
    {
        const auto result = std::from_chars(
            value.data(),
            value.data() + value.size(),
            output
        );
        return result.ec == std::errc{} &&
               result.ptr == value.data() + value.size() &&
               output != 0U;
    }

    [[nodiscard]] std::optional<Options> parseOptions(
        int argc,
        char** argv
    )
    {
        Options result;
        for (int index = 1; index < argc; ++index)
        {
            if (index + 1 >= argc)
                return std::nullopt;
            const std::string_view key{argv[index++]};
            const std::string_view value{argv[index]};
            if (key == "--size")
            {
                if (!parseSize(value, result.objects))
                    return std::nullopt;
            }
            else if (key == "--data-records")
            {
                if (!parseSize(value, result.data_records) ||
                    result.data_records > 16U)
                    return std::nullopt;
            }
            else if (key == "--partitions")
            {
                if (!parseSize(value, result.partitions) ||
                    result.partitions > 4096U)
                    return std::nullopt;
            }
            else if (key == "--output")
            {
                result.output = value;
            }
            else return std::nullopt;
        }
        return result;
    }

    [[nodiscard]] WorldObjectId objectId(std::size_t index) noexcept
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[0] = 0x80U;
        std::uint64_t value = static_cast<std::uint64_t>(index + 1U);
        for (std::size_t byte{}; byte < 8U; ++byte)
        {
            bytes[15U - byte] = static_cast<std::uint8_t>(value & 0xffU);
            value >>= 8U;
        }
        return WorldObjectId{uuids::uuid(bytes)};
    }

    [[nodiscard]] WorldPartitionId partitionId(std::size_t index) noexcept
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[0] = 0x40U;
        std::uint64_t value = static_cast<std::uint64_t>(index + 1U);
        for (std::size_t byte{}; byte < 8U; ++byte)
        {
            bytes[15U - byte] = static_cast<std::uint8_t>(value & 0xffU);
            value >>= 8U;
        }
        return WorldPartitionId{uuids::uuid(bytes)};
    }

    [[nodiscard]] std::vector<WorldDataSchemaId> schemas(
        std::size_t count
    )
    {
        std::vector<WorldDataSchemaId> result;
        result.reserve(count);
        for (std::size_t index{}; index < count; ++index)
            result.push_back(worldDataSchemaId(
                "benchmark.data." + std::to_string(index)
            ));
        return result;
    }

    [[nodiscard]] WorldDescription makeWorld(
        std::size_t object_count,
        std::span<const WorldDataSchemaId> schema_values
    )
    {
        WorldDescriptionBuilder builder;
        std::array<std::byte, 16> payload{};
        for (std::size_t object{}; object < object_count; ++object)
        {
            const auto id = objectId(object);
            if (!builder.addObject(id))
                std::abort();
            payload[0] = static_cast<std::byte>(object & 0xffU);
            payload[1] = static_cast<std::byte>((object >> 8U) & 0xffU);
            for (std::size_t data{}; data < schema_values.size(); ++data)
            {
                payload[2] = static_cast<std::byte>(data);
                if (!builder.addData(id, schema_values[data], 1U, payload))
                    std::abort();
            }
        }
        auto result = std::move(builder).build();
        if (!result)
            std::abort();
        return std::move(*result);
    }

    struct Measurement final
    {
        std::uint64_t nanoseconds{};
        std::size_t allocations{};
        std::size_t operations{};
        std::size_t retained_bytes{};
    };

    struct OperationResult final
    {
        std::size_t operations{};
        std::size_t retained_bytes{};
    };

    template <class Operation>
    [[nodiscard]] Measurement measure(Operation&& operation)
    {
        g_allocations.store(0U, std::memory_order_relaxed);
        g_count_allocations.store(true, std::memory_order_release);
        const auto begin = Clock::now();
        const OperationResult result = operation();
        const auto end = Clock::now();
        g_count_allocations.store(false, std::memory_order_release);
        return {
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    end - begin
                ).count()
            ),
            g_allocations.load(std::memory_order_relaxed),
            result.operations,
            result.retained_bytes,
        };
    }

    void writeRow(
        std::ostream& output,
        std::string_view kind,
        std::string_view metric,
        std::size_t size,
        const Options& options,
        std::string_view sample,
        const Measurement& value
    )
    {
        output << "4," << LUX_BENCHMARK_GIT_COMMIT << ','
               << compilerName() << ',' << compilerVersion() << ','
               << LUX_BENCHMARK_BUILD_TYPE << ',' << platformName() << ','
               << architectureName() << ',' << kind << ",world," << metric
               << ',' << size << ',' << sample << ','
               << value.nanoseconds << ',' << value.allocations << ','
               << value.retained_bytes << ',' << value.operations
               << ",0,0,0,0,0,0,0,0,0,0,0,0,0\n";
    }

    void writeSeries(
        std::ostream& output,
        std::string_view metric,
        std::size_t size,
        const Options& options,
        const std::vector<Measurement>& values
    )
    {
        for (std::size_t index{}; index < values.size(); ++index)
        {
            writeRow(
                output,
                "raw",
                metric,
                size,
                options,
                std::to_string(index),
                values[index]
            );
        }
        std::vector<std::uint64_t> times;
        std::vector<std::size_t> allocations;
        times.reserve(values.size());
        allocations.reserve(values.size());
        for (const auto& value : values)
        {
            times.push_back(value.nanoseconds);
            allocations.push_back(value.allocations);
        }
        std::sort(times.begin(), times.end());
        std::sort(allocations.begin(), allocations.end());
        Measurement median = values.back();
        median.nanoseconds = times[times.size() / 2U];
        median.allocations = allocations[allocations.size() / 2U];
        writeRow(
            output,
            "summary",
            metric,
            size,
            options,
            "median",
            median
        );
    }
}

int main(int argc, char** argv)
{
    const auto options = parseOptions(argc, argv);
    if (!options)
        return 2;

    std::ofstream output(options->output, std::ios::trunc);
    if (!output)
        return 3;
    output << "benchmark_schema_version,git_commit,compiler,"
              "compiler_version,build_type,platform,architecture,kind,"
              "group,metric,size,sample,nanoseconds,allocations,"
              "retained_bytes,dispatch_calls,storage_lookups,"
              "visited_nodes,history_losses,lane_binds,"
              "journal_stream_binds,record_appends,per_record_lookups,"
              "membership_entry_capacity_bytes,"
              "membership_node_capacity_bytes,active_tracked_entities,"
              "active_memberships,duplicate_comparisons\n";

    const auto schema_values = schemas(options->data_records);
    constexpr std::size_t warmups = 1U;
    constexpr std::size_t samples = 3U;
    std::vector<Measurement> build_samples;
    for (std::size_t sample{}; sample < warmups + samples; ++sample)
    {
        const auto built = measure([&]()
        {
            auto world = makeWorld(options->objects, schema_values);
            return OperationResult{
                world.objectCount() + world.dataCount(),
                world.retainedBytes()
            };
        });
        if (sample >= warmups)
            build_samples.push_back(built);
    }
    writeSeries(
        output,
        "world_description_build",
        options->objects,
        *options,
        build_samples
    );

    const auto world = makeWorld(options->objects, schema_values);
    std::vector<Measurement> lookup_samples;
    for (std::size_t sample{}; sample < warmups + samples; ++sample)
    {
        const auto lookup = measure([&]()
        {
            std::size_t found{};
            for (std::size_t index{}; index < options->objects; ++index)
            {
                const auto object = world.findObject(objectId(index));
                found += static_cast<bool>(object) ? 1U : 0U;
                for (const auto& schema : schema_values)
                    found += static_cast<bool>(object.findData(schema)) ? 1U : 0U;
            }
            return OperationResult{found, world.retainedBytes()};
        });
        if (sample >= warmups)
            lookup_samples.push_back(lookup);
    }
    writeSeries(
        output,
        "world_binary_lookup",
        options->objects,
        *options,
        lookup_samples
    );

    std::vector<std::vector<WorldObjectId>> groups(options->partitions);
    for (std::size_t index{}; index < options->objects; ++index)
        groups[index % groups.size()].push_back(objectId(index));
    std::vector<Measurement> freeze_samples;
    for (std::size_t sample{}; sample < warmups + samples; ++sample)
    {
        const auto frozen = measure([&]()
        {
            WorldPartitionLayoutBuilder builder(world);
            for (std::size_t index{}; index < groups.size(); ++index)
            {
                if (!groups[index].empty() &&
                    !builder.addPartition(partitionId(index), groups[index]))
                    std::abort();
            }
            auto layout = std::move(builder).build();
            if (!layout)
                std::abort();
            return OperationResult{
                layout->partitionCount() + options->objects,
                0U
            };
        });
        if (sample >= warmups)
            freeze_samples.push_back(frozen);
    }
    writeSeries(
        output,
        "world_partition_freeze",
        options->partitions,
        *options,
        freeze_samples
    );

    std::vector<std::uint8_t> quadrants(options->objects);
    std::vector<Measurement> rebuild_samples;
    for (std::size_t sample{}; sample < warmups + samples; ++sample)
    {
        const auto rebuilt = measure([&]()
        {
            for (std::size_t index{}; index < world.objectCount(); ++index)
            {
                const auto payload = world.objectAt(index).dataAt(0U).payload();
                const auto x = std::to_integer<std::uint8_t>(payload[0]);
                const auto y = std::to_integer<std::uint8_t>(payload[1]);
                quadrants[index] = static_cast<std::uint8_t>(
                    (x >= 128U ? 1U : 0U) | (y >= 128U ? 2U : 0U)
                );
            }
            return OperationResult{quadrants.size(), quadrants.capacity()};
        });
        if (sample >= warmups)
            rebuild_samples.push_back(rebuilt);
    }
    writeSeries(
        output,
        "world_quadtree_full_rebuild",
        options->objects,
        *options,
        rebuild_samples
    );
    const std::size_t edit_count = std::max<std::size_t>(1U, options->objects / 100U);
    std::vector<Measurement> incremental_samples;
    for (std::size_t sample{}; sample < warmups + samples; ++sample)
    {
        const auto incremental = measure([&]()
        {
            for (std::size_t index{}; index < edit_count; ++index)
                quadrants[index] = static_cast<std::uint8_t>((quadrants[index] + 1U) & 3U);
            return OperationResult{edit_count, quadrants.capacity()};
        });
        if (sample >= warmups)
            incremental_samples.push_back(incremental);
    }
    writeSeries(
        output,
        "world_quadtree_incremental_edit",
        options->objects,
        *options,
        incremental_samples
    );
    return output ? 0 : 4;
}

void* operator new(std::size_t size)
{
    if (g_count_allocations.load(std::memory_order_relaxed))
        g_allocations.fetch_add(1U, std::memory_order_relaxed);
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
