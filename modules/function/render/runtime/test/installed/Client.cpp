#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)
#error Native closure leaked into the synchronous consumer
#endif
int main()
{
    auto requester = lux::platform::ipc::IpcCore::instance().createRequester("installed.absent");
    if (!requester) return 1;
    lux::platform::ipc::IpcMessage message;
    message.emplace<unsigned>(17u);
    if (requester->trySend(message)) return 2;
    return message.get<unsigned>() == 17 ? 0 : 3;
}
