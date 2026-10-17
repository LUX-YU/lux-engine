#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <chrono>
#include <thread>
#include <cstdlib>

int main()
{
    using namespace lux::render;
    namespace ipc = lux::platform::ipc;
    auto server = RenderServer::create({"installed.runtime", true});
    auto client = ipc::IpcCore::instance().createRequester("installed.runtime");
    if (!server || !client) return 1;
    auto directory_id = (*server)->commandId<ListCommands>();
    if (!directory_id) return 2;
    ipc::IpcMessage message;
    message.emplace<ListCommands::Request>();
    message.setMetadata(RequestHeader{RequestId{1}, *directory_id});
    if (!client->trySend(message)) return 3;
    bool run_ok = false;
    std::thread thread([&] { run_ok = bool((*server)->run()); });
    bool discovered = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (client->tryReceive(message) == ipc::EIpcReceive::RECEIVED)
        {
            const auto header = message.metadata<ReplyHeader>();
            if (header.request != RequestId{1} || header.outcome != ECommandOutcome::SUCCESS) std::abort();
            for (const auto& entry : message.get<CommandList>().commands)
                if (entry.name == CreateScene::name && !entry.id.isNull()) discovered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    (*server)->stop();
    thread.join();
    return discovered && run_ok && (*server)->stats().validation_errors == 0 ? 0 : 4;
}
