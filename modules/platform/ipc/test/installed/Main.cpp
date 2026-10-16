#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <vector>
#ifdef IPC_PROTOCOL_CONSUMER
#include <lux/engine/render/protocol/Protocol.hpp>
static_assert(sizeof(lux::render::RequestId) == 8);
#endif

int main()
{
    using namespace lux::platform::ipc;
    auto server = IpcCore::instance().createResponder("installed");
    auto client = IpcCore::instance().createRequester("installed");
    if (!server || !client || !client->ready()) return 1;
    static_assert(IpcMessage::inline_capacity == 128);
    // G1's typed ABI includes a code pin and temporary byte-compatible SBO. All DLL consumers rebuild together.
    static_assert(sizeof(IpcMessage) == 208 && sizeof(IpcReply) == 432 && alignof(IpcMessage) == 16);
    static_assert(IpcMessage::attachment_capacity == 4);
    IpcMessage input;
    if (!client->trySend(input)) return 2;
    ReceivedRequest work;
    if (server->tryReceive(work) != EIpcReceive::RECEIVED) return 3;
    IpcMessage reply;
    if (!server->tryReply(work, reply)) return 4;
    IpcReply output;
    if (client->tryReceive(output) != EIpcReceive::RECEIVED) return 5;
    output = {};
    std::vector<unsigned> values{3, 7, 11};
    values.reserve(64);
    const auto* pointer = values.data();
    auto bound = IpcMessage::bindType<std::vector<unsigned>>(IpcMessage::type<std::vector<unsigned>>());
    if (!bound) return 6;
    input.emplace(*bound, std::move(values));
    if (!client->trySend(input)) return 7;
    if (server->tryReceive(work) != EIpcReceive::RECEIVED ||
        !work.message().holds(bound->operations())) return 8;
    reply.emplace<unsigned>(work.message().get<std::vector<unsigned>>()[1]);
    if (!server->tryReply(work, reply) || client->tryReceive(output) != EIpcReceive::RECEIVED) return 9;
    return output.message.get<unsigned>() == 7 &&
        output.original_request.get<std::vector<unsigned>>().data() == pointer &&
        output.original_request.get<std::vector<unsigned>>().capacity() == 64 ? 0 : 10;
}
