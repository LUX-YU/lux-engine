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
    // Bounded attachment view adds two pointer-sized words. Rebuild DLL consumers;
    // the old R2 binary ABI (160/328) is deliberately not claimed compatible.
    static_assert(sizeof(IpcMessage) == 176 && sizeof(IpcReply) == 360);
    static_assert(IpcMessage::attachment_capacity == 4);
    IpcMessage input;
    if (!client->trySend(input)) return 2;
    ReceivedRequest work;
    if (server->tryReceive(work) != EIpcReceive::RECEIVED) return 3;
    IpcMessage reply;
    if (!server->tryReply(work, reply)) return 4;
    IpcReply output;
    return client->tryReceive(output) == EIpcReceive::RECEIVED ? 0 : 5;
}
