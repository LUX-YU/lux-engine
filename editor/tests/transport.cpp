#include <cassert>
#include <chrono>
#include <cstdio>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/detail/EditorUiScene.hpp>
#include <lux/engine/rendering/RenderDiagnostics.hpp>
#include <lux/engine/rendering/RenderExecution.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <thread>

using namespace lux;
using namespace lux::render;
using namespace lux::rendering;
using namespace lux::editor;
using namespace std::chrono_literals;

namespace
{
    template <class T> auto take(T result)
    {
        if constexpr (requires { result.error().type; })
        {
            if (!result)
            {
                reportRenderFailure(result.error(), "Editor UI transport");
            }
        }
        assert(result);
        return std::move(*result);
    }

    struct Content final : ui::Pane
    {
        Content() : Pane("Actual UI GPU content"), label("Font, clipping and alpha use UiRasterFeature")
        {
            assert(addElement(label));
        }

        ui::Label label;
    };
} // namespace

int main()
{
    auto execution = take(execution::ExecutionRuntime::create({2, 128, {32}}));
    auto window = take(EditorWindow::create({640, 480, "Render UI integration"}));
    execution.setWake(&window::LuxWindow::wakeEvents);
    RenderExecutionConfig configuration;
    configuration.render.channel_name = "editor.ui.test";
    configuration.render.validation = true;
    configuration.render.frames_in_flight = 3;
    configuration.render.uploads.emplace();
    configuration.device.texture_descriptors = true;
    auto rendering = take(RenderExecution::create(std::move(configuration), execution));
    SurfaceDescription surface;
#if defined(_WIN32)
    surface.window.windows.hwnd = window->nativeHandle();
#endif
    auto& root = window->uiRoot();
    auto ui_scene = take(EditorUiScene::create(
        *rendering,
        {640, 480, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::SURFACE},
        surface,
        take(root.fontAtlas())
    ));
    std::unique_ptr<ui::Pane> content = std::make_unique<Content>();
    assert(root.addPane(std::move(content)));
    auto& client = rendering->mainClient();
    std::size_t completed = 0, captured_vertices = 0;
    auto drive = [&](bool capture)
    {
        window::LuxWindow::pollEvents();
        rendering->consumeResponses();
        assert(execution.dispatchMainCompletions() && execution.dispatchTaskEvents());
        assert(ui_scene->maintain());
        const bool draw = capture && take(ui_scene->outputReady());
        if ((!draw && !client.hasQueuedFrameCommands()) || !client.hasFrameCapacity())
        {
            return;
        }
        auto* frame = take(client.beginFrame());
        if (draw)
        {
            auto& data = ui_scene->captureBuffer(*frame);
            assert(root.update({{640, 480}, 0.016f}, data));
            captured_vertices += data.data().vertices.size();
            assert(ui_scene->appendFrameCommands(*frame));
        }
        assert(execution.submit(
            {},
            [&](execution::TaskReporter) noexcept
            {
                return stdexec::then(
                    client.submitFrame(*frame),
                    [&, frame](FrameCompletion response) noexcept
                    {
                        if (response.error.type)
                        {
                            reportRenderFailure(response.error, "UI frame");
                        }
                        assert(response.error.type == 0);
                        assert(client.releaseFrame(*frame));
                        ++completed;
                    }
                );
            }
        ));
    };
    auto until = [&](auto condition, bool draw)
    {
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (!condition())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            drive(draw);
            std::this_thread::sleep_for(1ms);
        }
    };
    until([&] { return completed >= 12 && rendering->stats().presentations >= 6; }, true);
    assert(captured_vertices > 0);
    // A malformed real output fails asynchronously; it is not reported as a ready UI scene.
    auto invalid =
        take(EditorUiScene::create(*rendering, {0, 480, rdesc::ETextureFormat::RGBA8_UNORM}, {}, take(root.fontAtlas()))
        );
    until([&] { return !invalid->outputReady(); }, true);
    assert(!invalid->outputReady());
    invalid->stopFrames();
    until(
        [&]
        {
            assert(invalid->maintain());
            return invalid->settled();
        },
        true
    );
    invalid.reset();
    until([&] { return client.pendingRequests() == 0; }, false);
    assert(root.clearPanes());
    const auto before = completed;
    until([&] { return completed >= before + 6; }, true);
    ui_scene->setExtent(0, 0, false);
    until([&] { return !take(ui_scene->outputReady()) && client.pendingRequests() == 0; }, false);
    ui_scene->setExtent(640, 480, true);
    until([&] { return take(ui_scene->outputReady()) && completed >= before + 10; }, true);
    ui_scene->stopFrames();
    until([&] { return ui_scene->settled() && client.pendingRequests() == 0; }, false);
    assert(rendering->shutdown());
    assert(rendering->stats().validation_errors == 0);
    ui_scene.reset();
    rendering.reset();
    execution.requestStop();
    assert(execution.waitUntil([&]() noexcept { return execution.settled(); }));
    assert(execution.join());
    std::puts("PASS actual Root/font/UiRasterFeature, shared Main frame, minimize/restore and native shutdown; "
              "pixel/clip/blend assertions are retained in engine.render_ui");
}
