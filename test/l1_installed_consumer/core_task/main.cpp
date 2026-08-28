#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <utility>

int
main()
{
    int value{};
    lux::task::TaskGraphBuilder builder;
    const auto task = builder.add([&value]() noexcept { ++value; });
    if (!task)
        return 1;
    auto graph = std::move(builder).build();
    if (!graph)
        return 2;

    auto executor = lux::task::TaskExecutor::create({0U, graph->taskCount()});
    if (!executor)
        return 3;
    if (!executor->execute(*graph))
        return 4;
    return value == 1 ? 0 : 5;
}
