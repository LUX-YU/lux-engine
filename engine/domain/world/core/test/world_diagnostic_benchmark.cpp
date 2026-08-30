#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/WorldPartition.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
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

    template <class Type>
    [[nodiscard]] Type id(std::uint64_t value)
    {
        std::array<std::uint8_t, 16> bytes{};
        for (std::size_t index{}; index < sizeof(value); ++index)
        {
            bytes[15U - index] = static_cast<std::uint8_t>(value & 0xffU);
            value >>= 8U;
        }
        return Type{uuids::uuid(bytes)};
    }
}

int main(int argc, char** argv)
{
    using namespace lux::world;

    const std::string_view output_path = argc > 1 ? argv[1] : "world_diagnostic.csv";
    constexpr std::uint32_t kPartitionCount = 1'000'000U;

    const auto build_begin = Clock::now();
    WorldDescriptionBuilder builder;
    if (!builder.setIdentity(id<WorldBundleId>(1U), id<WorldBundleGeneration>(2U), "benchmark") ||
        !builder.addSchema(worldDataSchemaId("benchmark.transform")) ||
        !builder.setPartitioner({worldPartitionerId("benchmark.grid"), 1U}, kPartitionCount) ||
        !builder.addStorageVolume({"benchmark.wvol0", 1U, 1U, 4096U}) ||
        !builder.addPartitionTablePage({lux::partition::PartitionOrdinal{0U}, kPartitionCount, {0U, 0U}}))
    {
        return 1;
    }
    auto world = std::move(builder).build();
    if (!world)
        return 2;
    const auto build_end = Clock::now();

    std::uint64_t lookup_checksum{};
    const auto lookup_begin = Clock::now();
    for (std::uint32_t partition{}; partition < kPartitionCount; ++partition)
    {
        const auto* page = world->partitionTable().findPage(lux::partition::PartitionOrdinal{partition});
        if (page == nullptr)
            return 3;
        lookup_checksum += page->first.value + page->count;
    }
    const auto lookup_end = Clock::now();

    constexpr std::size_t kObjectCount = 100'000U;
    std::vector<lux::domain::WorldObjectId> objects;
    objects.reserve(kObjectCount);
    for (std::size_t index{}; index < kObjectCount; ++index)
        objects.push_back(id<lux::domain::WorldObjectId>(index + 1U));

    const auto layout_begin = Clock::now();
    WorldPartitionLayoutBuilder layout_builder(objects);
    constexpr std::size_t kObjectsPerPartition = 1'000U;
    for (std::size_t first{}; first < objects.size(); first += kObjectsPerPartition)
    {
        const std::size_t count = std::min(kObjectsPerPartition, objects.size() - first);
        if (!layout_builder.addPartition(
                id<WorldPartitionId>(first / kObjectsPerPartition + 1U),
                std::span<const lux::domain::WorldObjectId>(objects).subspan(first, count)
            ))
        {
            return 4;
        }
    }
    auto layout = std::move(layout_builder).build();
    if (!layout)
        return 5;
    const auto layout_end = Clock::now();

    const auto ns = [](auto begin, auto end) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
    };

    std::ofstream output{std::string(output_path), std::ios::trunc};
    if (!output)
        return 6;
    output << "commit,build,metric,count,nanoseconds,retained_bytes,checksum\n";
    output << LUX_BENCHMARK_GIT_COMMIT << ',' << LUX_BENCHMARK_BUILD_TYPE << ",root_build,"
           << kPartitionCount << ',' << ns(build_begin, build_end) << ',' << world->retainedBytes() << ",0\n";
    output << LUX_BENCHMARK_GIT_COMMIT << ',' << LUX_BENCHMARK_BUILD_TYPE << ",page_lookup,"
           << kPartitionCount << ',' << ns(lookup_begin, lookup_end) << ',' << world->retainedBytes() << ','
           << lookup_checksum << "\n";
    output << LUX_BENCHMARK_GIT_COMMIT << ',' << LUX_BENCHMARK_BUILD_TYPE << ",layout_build,"
           << kObjectCount << ',' << ns(layout_begin, layout_end) << ",0," << layout->partitionCount() << "\n";
    return output ? 0 : 7;
}
