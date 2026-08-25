#include <lux/engine/task/TaskGraph.hpp>

#include <utility>

namespace
{
    void invoke(void* target, void*) noexcept
    {
        ++*static_cast<int*>(target);
    }
}

int main()
{
    int value{};
    lux::task::TaskGraphBuilder builder;
    const auto task = builder.addTask({&value, &invoke});
    if (!task)
        return 1;
    auto graph = std::move(builder).build();
    if (!graph)
        return 2;
    lux::task::TaskExecutionScratch scratch;
    if (!scratch.prepare(*graph))
        return 3;
    lux::task::executeTaskGraph(
        lux::task::referenceTaskExecutionBackend(),
        *graph,
        nullptr,
        scratch
    );
    return value == 1 ? 0 : 4;
}
