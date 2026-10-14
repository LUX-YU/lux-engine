#include <lux/engine/render/runtime/RenderServer.hpp>
#include <chrono>
#include <thread>

int main()
{
    using namespace lux::render;
    auto server = RenderServer::create({"installed.runtime", true});
    auto client = RenderClient::create("installed.runtime");
    if (!server || !client) return 1;
    bool run_ok = false;
    std::thread thread([&] { run_ok = bool((*server)->run()); });
    auto id = (*client)->hello();
    bool ready = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (id && std::chrono::steady_clock::now() < deadline)
    {
        RenderCompletion reply;
        auto status = (*client)->tryReceive(reply);
        if (!status) break;
        if (*status == ERenderReceive::RECEIVED)
        {
            ready = reply.header.request == *id && reply.header.outcome == EOperationOutcome::SUCCESS;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    (*server)->stop();
    thread.join();
    return ready && run_ok && (*server)->stats().validation_errors == 0 ? 0 : 2;
}
