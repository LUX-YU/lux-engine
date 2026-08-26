#include <lux/engine/ecs/EcsTaskAccess.hpp>
#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/WorldTaskResources.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    struct Options final
    {
        std::string group{"task-graph"};
        std::string mode{"diagnostic"};
        std::size_t size{1024U};
        std::string output{"ecs_l1_benchmark.csv"};
    };

    [[nodiscard]] Options parse(int count, char** values)
    {
        Options result;
        for (int index{1}; index + 1 < count; index += 2)
        {
            const std::string_view key{values[index]};
            if (key == "--group") result.group = values[index + 1];
            else if (key == "--mode") result.mode = values[index + 1];
            else if (key == "--size")
                result.size = static_cast<std::size_t>(
                    std::stoull(values[index + 1])
                );
            else if (key == "--output") result.output = values[index + 1];
        }
        return result;
    }

    struct Sample final
    {
        std::uint64_t nanoseconds{};
        std::size_t size{};
    };

    [[nodiscard]] Sample taskGraphSample(std::size_t size)
    {
        std::uint64_t calls{};
        lux::task::TaskGraphBuilder builder;
        for (std::size_t index{}; index < size; ++index)
        {
            if (!builder.add([&calls]() noexcept { ++calls; }))
                return {};
        }
        auto graph = std::move(builder).build();
        if (!graph)
            return {};
        lux::task::TaskExecutor executor({0U, graph->taskCount()});
        const auto begin = std::chrono::steady_clock::now();
        if (!executor.execute(*graph))
            return {};
        const auto end = std::chrono::steady_clock::now();
        return {
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    end - begin
                ).count()
            ),
            static_cast<std::size_t>(calls)
        };
    }
}

int main(int argc, char** argv)
{
    const Options options = parse(argc, argv);
    const std::size_t warmups = options.mode == "qualification" ? 5U : 1U;
    const std::size_t samples = options.mode == "qualification" ? 30U : 3U;
    for (std::size_t index{}; index < warmups; ++index)
        (void)taskGraphSample(options.size);

    std::ofstream output(options.output, std::ios::trunc);
    if (!output)
        return 2;
    output << "benchmark_schema_version,group,metric,size,sample,nanoseconds,record_appends\n";
    for (std::size_t index{}; index < samples; ++index)
    {
        const auto sample = taskGraphSample(options.size);
        output << "3," << options.group << ",execute," << options.size
               << ',' << index << ',' << sample.nanoseconds << ','
               << sample.size << '\n';
        output.flush();
    }
    return 0;
}
