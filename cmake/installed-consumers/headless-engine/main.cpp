#include <lux/engine/Engine.hpp>

int main()
{
    auto engine = lux::engine::Engine::create({1, 16, {16}}, 64);
    if (!engine)
    {
        return 1;
    }
    auto& execution = (*engine)->execution();
    bool delivered = false;
    bool succeeded = false;
    auto work = stdexec::schedule(execution.cpuScheduler()) | stdexec::then([]() noexcept { return true; }) |
                stdexec::upon_error([](lux::execution::EExecutionError) noexcept { return false; }) |
                stdexec::upon_stopped([]() noexcept { return false; }) |
                stdexec::continues_on(execution.mainScheduler()) |
                stdexec::then(
                    [&](bool success) noexcept
                    {
                        succeeded = success;
                        delivered = true;
                    }
                );
    auto started = execution.spawn(std::move(work));
    if (!started || !execution.waitUntil([&]() noexcept { return delivered; }))
    {
        return 2;
    }
    if (!succeeded || !(*engine)->sceneRuntime().driveFrame())
    {
        return 3;
    }
    return 0;
}
