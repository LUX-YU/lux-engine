#include <lux/engine/editor/rendering/EditorRenderer.hpp>
#include <lux/engine/editor/rendering/detail/RendererTestAccess.hpp>
#include <lux/engine/editor/ui/shell/EditorWindow.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <cassert>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <string_view>

int main(int argc, char **argv)
{
    using namespace lux::editor;
    using namespace rendering;
    using rendering::detail::EStartupFault;
    using rendering::detail::RendererTestAccess;
    const std::string_view selected = argc == 2 ? argv[1] : "all";
    const bool without_validation = selected == "initialize_no_validation";
    constexpr std::string_view names[]{"none", "thread_launch", "initialize", "after_device", "after_attach"};
    assert(argc <= 2 &&
           (selected == "all" || without_validation ||
            std::find(std::begin(names) + 1, std::end(names), selected) != std::end(names)));
    unsigned executed{};
    lux::meta::ReflectionRegistry::initRegistry();
    {
        lux::window::GlfwRuntime platform;
        assert(platform.valid());
        lux::object::ObjectMessageQueue messages;
        auto window = ui::EditorWindow::create(messages.dispatcherRef(), {256, 256, "ER1 startup recovery", false});
        assert(window);
        auto *const native = &(*window)->nativeWindow();
        auto *const session = &(*window)->uiSession();
        RendererConfig config;
        config.validation = !without_validation;
        for (const auto fault : {EStartupFault::THREAD_LAUNCH, EStartupFault::INITIALIZE, EStartupFault::AFTER_DEVICE,
                                 EStartupFault::AFTER_ATTACH})
        {
            if (selected != "all" && selected != names[static_cast<unsigned>(fault)] &&
                !(without_validation && fault == EStartupFault::INITIALIZE))
                continue;
            ++executed;
            const lux::render::RenderError injected{{901, 17}, {static_cast<unsigned>(fault), 29, 41}};
            RendererTestAccess::failStartup(fault, injected);
            std::printf("startup attempt fault=%u validation=%u\n", unsigned(fault), unsigned(config.validation));
            std::fflush(stdout);
            auto failed = EditorRenderer::create(*native, *session, config);
            assert(!failed);
            const auto expected_code = fault == EStartupFault::THREAD_LAUNCH ? ERendererError::EXTERNAL_FAILURE
                                                                             : ERendererError::DEVICE_FAILURE;
            assert(failed.error().code == expected_code);
            const auto backend = failed.error().render_error;
            if (fault != EStartupFault::INITIALIZE)
                assert(backend.type == injected.type && backend.args == injected.args);
            else
            {
                const auto expected = lux::render::renderError<lux::render::err::device::VulkanObjectCreationFailed>();
                assert(backend.type == expected.type && backend.args == expected.args);
            }
            const auto trace = RendererTestAccess::startupTrace();
            const auto worker_count = fault == EStartupFault::THREAD_LAUNCH ? 0U : 1U;
            assert(trace.workers_started == worker_count && trace.workers_exited == worker_count);
            assert(trace.servers_created == worker_count && trace.servers_destroyed == worker_count);
            assert(trace.initialized == (fault == EStartupFault::AFTER_DEVICE || fault == EStartupFault::AFTER_ATTACH));
            assert(trace.attached == (fault == EStartupFault::AFTER_ATTACH));
            assert(&(*window)->nativeWindow() == native && &(*window)->uiSession() == session &&
                   native->isInitialized());
            assert((*window)->beginFrame({{256, 256}, 1.0F / 60, {1, 1}}));
            assert((*window)->drawPanes());
            auto snapshot = (*window)->finishFrame();
            assert(snapshot && snapshot->valid());
            std::printf("startup fault=%u code=%u backend=%u:%u args=%u,%u,%u workers=%u/%u servers=%u/%u "
                        "initialized=%u attached=%u window_ui_retained=1\n",
                        static_cast<unsigned>(fault), static_cast<unsigned>(failed.error().code), backend.type.index,
                        backend.type.gen, backend.args[0], backend.args[1], backend.args[2], trace.workers_started,
                        trace.workers_exited, trace.servers_created, trace.servers_destroyed, trace.initialized,
                        trace.attached);
            std::fflush(stdout);
            RendererTestAccess::failStartup(EStartupFault::NONE, {});
            auto recovered = EditorRenderer::create(*native, *session, config);
            assert(recovered);
            auto packet = (*recovered)->sealFrame(*snapshot, {});
            assert(packet && !snapshot->valid());
            auto submitted = (*recovered)->trySubmitFrame(*packet);
            assert(submitted && *submitted == EFrameSubmit::SUBMITTED);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
            while ((*recovered)->statistics().gpu_completed == 0)
            {
                assert(std::chrono::steady_clock::now() < deadline && (*recovered)->poll(8));
                if (!packet->valid())
                {
                    assert((*window)->beginFrame({{256, 256}, 1.0F / 60, {1, 1}}));
                    assert((*window)->drawPanes());
                    snapshot = (*window)->finishFrame();
                    assert(snapshot);
                    packet = (*recovered)->sealFrame(*snapshot, {});
                    assert(packet);
                }
                submitted = (*recovered)->trySubmitFrame(*packet);
                assert(submitted);
                std::this_thread::yield();
            }
            packet = lux::cxx::unexpected(RendererFailure{});
            assert((*recovered)->beginClose());
            for (;;)
            {
                assert(std::chrono::steady_clock::now() < deadline);
                auto closed = (*recovered)->advanceClose();
                assert(closed);
                if (*closed == ERenderClose::COMPLETE)
                    break;
                std::this_thread::yield();
            }
            assert((*recovered)->joinStopped());
            assert((*recovered)->statistics().validation_errors == 0);
            recovered->reset();
            const auto retried = RendererTestAccess::startupTrace();
            assert(retried.workers_started == 1 && retried.workers_exited == 1);
            assert(retried.servers_created == 1 && retried.servers_destroyed == 1);
            assert(retried.initialized == 1 && retried.attached == 1);
            std::printf("startup retry=%u completed_gpu_frame=1 explicit_close=1 joined=1 validation=0\n",
                        static_cast<unsigned>(fault));
            std::fflush(stdout);
        }
        assert((*window)->closeAfterRendererStopped());
        window->reset();
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
    std::printf("startup failure protocol PASS cases=%u; same Window/UI retry; "
                "real GPU completion; owners closed\n", executed);
}
