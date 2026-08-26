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

namespace
{
    using Clock = std::chrono::steady_clock;
    using namespace lux::world;

    std::atomic_bool g_count_allocations{};
    std::atomic_size_t g_allocations{};

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
    };

    template <class Operation>
    [[nodiscard]] Measurement measure(Operation&& operation)
    {
        g_allocations.store(0U, std::memory_order_relaxed);
        g_count_allocations.store(true, std::memory_order_release);
        const auto begin = Clock::now();
        const std::size_t operations = operation();
        const auto end = Clock::now();
        g_count_allocations.store(false, std::memory_order_release);
        return {
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    end - begin
                ).count()
            ),
            g_allocations.load(std::memory_order_relaxed),
            operations,
        };
    }

    void writeRow(
        std::ostream& output,
        std::string_view metric,
        const Options& options,
        std::size_t sample,
        const Measurement& value
    )
    {
        output << metric << ',' << options.objects << ','
               << options.data_records << ',' << options.partitions << ','
               << sample << ',' << value.nanoseconds << ','
               << value.allocations << ',' << value.operations << '\n';
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
    output << "metric,objects,data_records,partitions,sample,nanoseconds,"
              "allocations,operations\n";

    const auto schema_values = schemas(options->data_records);
    constexpr std::size_t warmups = 1U;
    constexpr std::size_t samples = 3U;
    for (std::size_t sample{}; sample < warmups + samples; ++sample)
    {
        const auto built = measure([&]()
        {
            auto world = makeWorld(options->objects, schema_values);
            return world.objectCount() + world.dataCount();
        });
        if (sample >= warmups)
            writeRow(output, "description_build", *options, sample - warmups, built);
    }

    const auto world = makeWorld(options->objects, schema_values);
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
            return found;
        });
        if (sample >= warmups)
            writeRow(output, "binary_lookup", *options, sample - warmups, lookup);
    }

    std::vector<std::vector<WorldObjectId>> groups(options->partitions);
    for (std::size_t index{}; index < options->objects; ++index)
        groups[index % groups.size()].push_back(objectId(index));
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
            return layout->partitionCount() + options->objects;
        });
        if (sample >= warmups)
            writeRow(output, "layout_freeze", *options, sample - warmups, frozen);
    }

    std::vector<std::uint8_t> quadrants(options->objects);
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
            return quadrants.size();
        });
        if (sample >= warmups)
            writeRow(output, "quadtree_full_rebuild", *options, sample - warmups, rebuilt);
    }
    const std::size_t edit_count = std::max<std::size_t>(1U, options->objects / 100U);
    for (std::size_t sample{}; sample < warmups + samples; ++sample)
    {
        const auto incremental = measure([&]()
        {
            for (std::size_t index{}; index < edit_count; ++index)
                quadrants[index] = static_cast<std::uint8_t>((quadrants[index] + 1U) & 3U);
            return edit_count;
        });
        if (sample >= warmups)
            writeRow(output, "quadtree_incremental_edit", *options, sample - warmups, incremental);
    }
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
