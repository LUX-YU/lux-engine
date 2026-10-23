#include <lux/engine/core/execution/CompletionWork.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/core/execution/RunLoop.hpp>
#include <cstdlib>
#include <thread>

int main()
{
    using namespace lux::execution;
    RunLoop loop;
    unsigned ready = 0;
    stdexec::counting_scope scope;
    stdexec::spawn(
        stdexec::schedule(loop.scheduler()) | stdexec::then([&]() noexcept { ++ready; }), scope.get_token()
    );
    if (ready != 0 || loop.executeReady(1) != 1 || ready != 1) std::abort();
    scope.close();
    if (!stdexec::sync_wait(scope.join())) std::abort();
    auto runtime = ExecutionRuntime::create({1, 16, {16}});
    if (!runtime) std::abort();
    struct State final
    {
        std::thread::id owner;
        unsigned completed = 0;
    } state{std::this_thread::get_id()};

    CompletionWork work{*runtime, &state, [](void* value) noexcept
    {
        auto& result = *static_cast<State*>(value);
        if (std::this_thread::get_id() != result.owner) std::abort();
        ++result.completed;
    }};
    const auto request = work.requester();
    std::thread producer([request] { request.request(); });
    producer.join();
    if (state.completed != 0) std::abort();
    if (!runtime->dispatchMainCompletions() || state.completed != 1)
    {
        std::abort();
    }
    work.cancel();
    request.request();
    if (!runtime->dispatchMainCompletions() || state.completed != 1)
    {
        std::abort();
    }
    runtime->requestStop();
    if (!runtime->join()) std::abort();
}
