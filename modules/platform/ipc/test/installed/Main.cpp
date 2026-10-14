#include <lux/engine/platform/ipc/IpcCore.hpp>
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
    static_assert(sizeof(IpcMessage) == 160 && sizeof(IpcReply) == 328);
    IpcMessage input;
    if (!client->trySend(input)) return 2;
    ReceivedRequest work;
    if (server->tryReceive(work) != EIpcReceive::RECEIVED) return 3;
    IpcMessage reply;
    if (!server->tryReply(work, reply)) return 4;
    IpcReply output;
    return client->tryReceive(output) == EIpcReceive::RECEIVED ? 0 : 5;
}
