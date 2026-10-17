#include <lux/engine/render/runtime/VulkanServer.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <chrono>
#include <thread>
#include <cstdlib>

int main()
{
    using namespace lux::render;
    namespace ipc = lux::platform::ipc;
    auto host = lux::execution::ExecutionRuntime::create({2, 128, 128, {128}});
    if (!host) return 1;
    auto server = vulkan::createRenderServer({"installed.runtime", true}, *host);
    auto client = ipc::IpcCore::instance().createRequester("installed.runtime");
    if (!server || !client) return 1;
    auto directory_id = (*server)->commandId<ListCommands>();
    if (!directory_id) return 2;
    ipc::IpcMessage message;
    message.emplace<ListCommands::Request>();
    if (!client->trySend(commandTag(*directory_id), 1, message)) return 3;
    bool run_ok = false;
    std::thread thread([&] { run_ok = bool((*server)->run()); });
    bool discovered = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::uint64_t correlation = 0;
        if (client->tryReceive(correlation, message) == ipc::EIpcReceive::RECEIVED)
        {
            if (correlation != 1 || !message.get<ListCommands::Response>()) std::abort();
            for (const auto& entry : message.get<ListCommands::Response>()->commands)
                if (entry.name == CreateScene::name && !entry.id.isNull()) discovered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    (*server)->stop();
    thread.join();
    return discovered && run_ok && (*server)->stats().validation_errors == 0 ? 0 : 4;
}
