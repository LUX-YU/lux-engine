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
    IpcMessage input;
    if (!client->trySend(input)) return 2;
    auto work = server->tryReceive();
    if (!work || !*work) return 3;
    IpcMessage reply;
    if (!server->tryReply(**work, reply)) return 4;
    auto output = client->tryReceive();
    return output && *output ? 0 : 5;
}
