#include <array>
#include <chrono>
#include <lux/engine/render/commands/DiscoveryCommands.hpp>
#include <cstdlib>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <probe/Probe.hpp>
#include <thread>

namespace
{
    void check(bool condition)
    {
        if (!condition)
        {
            std::abort();
        }
    }
} // namespace

int main()
{
    using namespace lux::render;
    namespace ipc = lux::platform::ipc;
    auto execution = lux::execution::ExecutionRuntime::create({1, 64, {64}});
    check(bool(execution));
    auto server = RenderServer::create({"spatial.external"}, **execution);
    check(bool(server));
    auto handler = probe::install(**server);
    check(bool(handler));
    auto second = probe::install(**server);
    check(bool(second));
    auto client = ipc::IpcCore::instance().createRequester("spatial.external");
    check(bool(client));
    std::thread render([&]() { check(bool((*server)->run())); });
    ipc::IpcMessage message;
    const std::array names{CommandName{probe::SetValues::name}};
    message.emplace<QueryCommandIds::Request>(names);
    check(bool(client->trySend(commandTag(QueryCommandIds::id), 1, message)));
    const auto receive = [&](std::uint64_t expected)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::uint64_t correlation = 0;
        while (client->tryReceive(correlation, message) != ipc::EIpcReceive::RECEIVED)
        {
            check(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        check(correlation == expected);
    };
    receive(1);
    const auto ids = std::move(message.get<QueryCommandIds::Response>());
    check(!ids.error.type && ids.ids.size() == 1);
    message.emplace<probe::SetValues::Request>(probe::SetValues::Request{{{1, 2.5}, {2, 7.5}}});
    check(bool(client->trySend(commandTag(ids.ids.front()), 2, message)));
    receive(2);
    const auto& reply = message.get<probe::SetValues::Response>();
    check(!reply.error.type && reply.sequence == 2 && reply.sum == 10);
    (*server)->stop();
    render.join();
    server->reset();
    execution->reset();
}
