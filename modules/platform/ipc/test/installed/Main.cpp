#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <vector>
#ifdef IPC_PROTOCOL_CONSUMER
#include <lux/engine/render/protocol/Protocol.hpp>
#endif
using namespace lux::platform::ipc;
int main()
{
    static_assert(sizeof(IpcMessage) == 96);
    auto server = IpcCore::instance().createResponder("installed");
    auto client = IpcCore::instance().createRequester("installed");
    if (!server || !client) return 1;
    IpcMessage input;
    std::vector<unsigned> values{3, 7};
    values.reserve(64);
    const auto* pointer = values.data();
    input.emplace<std::vector<unsigned>>(std::move(values));
    if (!client->trySend(3, 19, input)) return 2;
    ReceivedRequest work;
    if (server->tryReceive(work) != EIpcReceive::RECEIVED) return 3;
    const auto& request = work.message().get<std::vector<unsigned>>();
    if (request.data() != pointer || request.capacity() != 64) return 4;
    IpcMessage reply;
    reply.emplace<unsigned>(request[1]);
    if (!server->trySendResponse(work.requesterId(), work.correlation(), reply)) return 5;
    IpcMessage output;
    std::uint64_t correlation = 0;
    if (client->tryReceive(correlation, output) != EIpcReceive::RECEIVED) return 6;
    client->reset();
    server->reset();
    return output.get<unsigned>() == 7 && correlation == 19 ? 0 : 7;
}
