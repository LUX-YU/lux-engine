#include <cassert>
#include <chrono>
#include <cstdio>
#include <exec/task.hpp>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/editor/AppErrors.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/LuxEngine.hpp>
#include <lux/engine/editor/ProjectManifest.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/rendering/RenderExecution.hpp>
#include <thread>

namespace
{
    exec::task<lux::editor::FrameworkResult<void>> stopAfterPresentation(
        lux::editor::LuxEngine& application,
        bool& rejected_recursive_drive,
        bool& acknowledged
    ) noexcept
    {
        using namespace lux;
        auto& rendering = application.rendering();
        auto& execution = rendering.execution();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (rendering.stats().presentations == 0)
        {
            assert(std::chrono::steady_clock::now() < deadline);
            auto delayed = execution.timer().after(std::chrono::milliseconds(2)) |
                           stdexec::then([]() noexcept { return true; }) |
                           stdexec::upon_error([](auto) noexcept { return false; }) |
                           stdexec::upon_stopped([]() noexcept { return false; });
            assert(co_await stdexec::continues_on(std::move(delayed), execution.mainScheduler()));
        }
        // A public SDK consumer uses run() as its only application driver. Ordinary Main callbacks
        // must not nest it or turn a rejected reentry into application shutdown.
        const auto nested = application.run();
        assert(!nested && nested.error().type == editor::Errors::EditorRecursiveHostFrame);
        assert(application.project().project().name == "Shutdown");
        rejected_recursive_drive = true;
        auto& client = rendering.mainClient();
        auto command = client.bind<render::Stop>();
        assert(command);
        render::EmptyRequest input;
        const auto reply = co_await client.request(*command, input);
        assert(reply.error.type == 0);
        acknowledged = true;
        co_return editor::FrameworkResult<void>{};
    }
} // namespace

int main(int argc, char** argv)
{
    using namespace lux;
    assert(argc == 2);
    auto file = std::filesystem::absolute(argv[1]);
    editor::ProjectManifest manifest{1, *uuids::uuid::from_string("889c1234-0000-0000-0000-000000000000"), "Shutdown"};
    assert(editor::writeProjectManifestAtomic(file, manifest, editor::EProjectWrite::REPLACE));
    editor::EditorConfig config{"Posted shutdown", 200, 160};
    config.project_file = file;
    auto host = editor::LuxEngine::create(std::move(config));
    assert(host);
    editor::OpenProjectRequest request{file};
    assert(object::sendEvent(**host, request) && !request.rejection.type);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!(*host)->engine().execution().hasReadyWork())
    {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::puts("real MainScheduler completion ready; destroying host");
    std::fflush(stdout);
    host->reset();
    assert(!object::ObjectRuntime::instance().statistics().pending);
    std::puts("host destruction returned; accepted completion settled");
    editor::EditorConfig failure_config{"Execution failure close", 200, 160};
    failure_config.project_file = file;
    host = editor::LuxEngine::create(std::move(failure_config));
    assert(host);
    auto& rendering = (*host)->rendering();
    auto& client = rendering.mainClient();
    bool rejected_recursive_drive{}, acknowledged{};
    auto work = stdexec::starts_on(
        rendering.execution().mainScheduler(),
        stopAfterPresentation(**host, rejected_recursive_drive, acknowledged)
    );
    assert(rendering.execution().spawn(
        stdexec::then(execution::taskResult(std::move(work)), [](auto&& result) noexcept { assert(result); })
    ));
    const auto stopped = (*host)->run();
    assert(!stopped && stopped.error().type == render::kCommandCancelled && acknowledged && rejected_recursive_drive);
    assert(client.pendingRequests() == 0 && rendering.stats().validation_errors == 0);
    host->reset();
    std::puts("execution failure reported after UI/Clients/Server cleanup; destructor completed");
}
