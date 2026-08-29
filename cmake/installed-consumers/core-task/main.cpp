#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <array>
#include <utility>

int
main()
{
    int value{};
    lux::task::TaskGraphBuilder builder;
    auto first = builder.add([&value]() noexcept { ++value; });
    if (!first)
        return 1;
    const std::array prerequisites{*first};
    if (!builder.add(lux::task::dependencies(prerequisites), [&value]() noexcept { ++value; }))
        return 2;
    auto graph = std::move(builder).build();
    if (!graph)
        return 3;
    auto executor = lux::task::TaskExecutor::create({1U, graph->taskCount()});
    return executor && executor->execute(*graph) && value == 2 ? 0 : 4;
}
