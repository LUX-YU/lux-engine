#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <utility>

int
main()
{
    int value{};
    lux::task::TaskGraphBuilder builder;
    if (!builder.add([&value]() noexcept { ++value; }))
        return 1;
    auto graph = std::move(builder).build();
    if (!graph)
        return 2;
    lux::task::TaskExecutor executor({0U, graph->taskCount()});
    return executor.execute(*graph) && value == 1 ? 0 : 3;
}
