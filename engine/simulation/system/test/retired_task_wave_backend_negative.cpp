#include <lux/engine/task/TaskGraph.hpp>

#include <span>

void
wave(void*, std::span<const lux::task::TaskExecutionItem>, void*) noexcept
{
}

int
main()
{
    lux::task::TaskExecutionBackendRef backend{nullptr, &wave};
}
