#include <cassert>
#include <chrono>
#include <cstdio>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/editor/AppErrors.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/LuxEngine.hpp>
#include <lux/engine/editor/ProjectManifest.hpp>
#include <lux/engine/editor/detail/LuxEngineTestAccess.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/rendering/RenderExecution.hpp>
#include <thread>

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
    const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while ((*host)->rendering().stats().presentations == 0)
    {
        assert(std::chrono::steady_clock::now() < ready_deadline);
        auto advanced = editor::detail::LuxEngineTestAccess::pumpOnce(**host);
        assert(advanced && *advanced);
    }
    auto& rendering = (*host)->rendering();
    auto& client = rendering.mainClient();
    bool rejected_recursive_drive{};
    assert(rendering.execution().spawn(stdexec::then(
        stdexec::schedule(rendering.execution().mainScheduler()),
        [&]() noexcept
        {
            // Ordinary Execution callbacks are not Object event dispatch. The outer driver still forbids
            // reentry, and a rejected nested run must not initiate application shutdown itself.
            const auto nested_run = (*host)->run();
            const auto nested_step = editor::detail::LuxEngineTestAccess::pumpOnce(**host);
            assert(!nested_run && nested_run.error().type == editor::Errors::EditorRecursiveHostFrame);
            assert(!nested_step && nested_step.error().type == editor::Errors::EditorRecursiveHostFrame);
            assert((*host)->project().project().name == "Shutdown");
            rejected_recursive_drive = true;
        }
    )));
    auto command = client.bind<render::Stop>();
    assert(command);
    render::EmptyRequest request_stop;
    bool acknowledged{};
    assert(rendering.execution().spawn(stdexec::then(
        client.request(*command, request_stop),
        [&](render::AckResponse reply) noexcept
        {
            assert(reply.error.type == 0);
            acknowledged = true;
        }
    )));
    const auto stopped = (*host)->run();
    assert(!stopped && stopped.error().type == render::kCommandCancelled && acknowledged && rejected_recursive_drive);
    assert(client.pendingRequests() == 0 && rendering.stats().validation_errors == 0);
    host->reset();
    std::puts("execution failure reported after UI/Clients/Server cleanup; destructor completed");
}
