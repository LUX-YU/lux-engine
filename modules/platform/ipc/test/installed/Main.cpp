#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <vector>
#ifdef IPC_PROTOCOL_CONSUMER
#include <lux/engine/render/protocol/Protocol.hpp>
#endif
using namespace lux::platform::ipc;
int main()
{
    static_assert(sizeof(IpcMessage) == 128);
    auto server = IpcCore::instance().createResponder("installed");
    auto client = IpcCore::instance().createRequester("installed");
    if (!server || !client) return 1;
    IpcMessage input;
    std::vector<unsigned> values{3, 7};
    values.reserve(64);
    const auto* pointer = values.data();
    input.emplace<std::vector<unsigned>>(std::move(values));
    input.setMetadata(std::uint64_t{19});
    if (!client->trySend(input)) return 2;
    ReceivedRequest work;
    if (server->tryReceive(work) != EIpcReceive::RECEIVED) return 3;
    const auto& request = work.message().get<std::vector<unsigned>>();
    if (request.data() != pointer || request.capacity() != 64) return 4;
    IpcMessage reply;
    reply.emplace<unsigned>(request[1]);
    reply.setMetadata(work.message().metadata<std::uint64_t>());
    if (!server->tryReply(work, reply)) return 5;
    IpcMessage output;
    if (client->tryReceive(output) != EIpcReceive::RECEIVED) return 6;
    client->reset();
    server->reset();
    return output.get<unsigned>() == 7 && output.metadata<std::uint64_t>() == 19 ? 0 : 7;
}
