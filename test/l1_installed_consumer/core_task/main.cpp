#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <array>
#include <utility>

int
main()
{
    int value{};
    lux::task::TaskGraphBuilder builder;
    const auto task = builder.add([&value]() noexcept { ++value; });
    if (!task)
        return 1;
    const std::array prerequisites{*task};
    if (!builder.add(lux::task::dependencies(prerequisites), [&value]() noexcept { ++value; }))
        return 2;
    auto graph = std::move(builder).build();
    if (!graph)
        return 3;

    auto executor = lux::task::TaskExecutor::create({1U, graph->taskCount()});
    if (!executor)
        return 4;
    if (!executor->execute(*graph))
        return 5;
    return value == 2 ? 0 : 6;
}
