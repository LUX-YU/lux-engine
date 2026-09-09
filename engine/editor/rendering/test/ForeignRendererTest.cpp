#include <lux/engine/editor/rendering/EditorRenderer.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/editor/ui/shell/EditorWindow.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

int main()
{
    using namespace lux::editor;
    using Clock = std::chrono::steady_clock;
    lux::meta::ReflectionRegistry::initRegistry();
    {
        lux::window::GlfwRuntime platform;
        assert(platform.valid());
        lux::object::ObjectMessageQueue messages;
        auto first_window = ui::EditorWindow::create(messages.dispatcherRef(), {256, 256, "Foreign A", false});
        auto second_window = ui::EditorWindow::create(messages.dispatcherRef(), {256, 256, "Foreign B", false});
        assert(first_window && second_window);
        rendering::RendererConfig config;
        config.validation = true;
        auto first =
            rendering::EditorRenderer::create((*first_window)->nativeWindow(), (*first_window)->uiSession(), config);
        auto second =
            rendering::EditorRenderer::create((*second_window)->nativeWindow(), (*second_window)->uiSession(), config);
        assert(first && second && first->get() != second->get());
        auto runtime = (*second)->acquire();
        assert(runtime);
        auto created_scene = runtime->control().createScene("Foreign renderer scene");
        const auto deadline = Clock::now() + std::chrono::seconds{20};
        while (!created_scene.isReady())
        {
            assert(Clock::now() < deadline && (*second)->poll(1));
            std::this_thread::yield();
        }
        const auto scene = created_scene.tryResult();
        assert(scene);
        auto view = (*second)->openView(scene->get().scene_id, {{64, 64}, true});
        assert(view);
        rendering::CameraFrame camera;
        camera.view[0] = camera.view[5] = camera.view[10] = camera.view[15] = 1;
        camera.projection = camera.view;
        camera.desired = {1, 1, 1, 1};
        assert((*view)->setCamera(camera));
        while ((*view)->status().state != rendering::EViewState::READY)
        {
            assert(Clock::now() < deadline && (*second)->poll(1));
            assert((*view)->status().state != rendering::EViewState::FAILED);
            std::this_thread::yield();
        }
        auto foreign = (*view)->acquireImage();
        assert(foreign);
        assert((*first_window)->beginFrame({{256, 256}, 1.0F / 60, {1, 1}}));
        assert((*first_window)->drawPanes());
        auto snapshot = (*first_window)->finishFrame();
        assert(snapshot && snapshot->valid());
        const auto accepted = (*first)->statistics().accepted_frames;
        const auto foreign_id = foreign->view;
        const auto sealed = (*first)->sealFrame(*snapshot, std::span<const rendering::ViewImage>{&*foreign, 1});
        assert(!sealed && sealed.error().code == rendering::ERendererError::STALE_IMAGE);
        assert(snapshot->valid() && foreign->lease.valid() && foreign->view == foreign_id);
        assert((*first)->statistics().accepted_frames == accepted);
        assert((*view)->beginClose());
        for (unsigned i = 0; i != 4; ++i)
        {
            assert((*second)->poll(1));
            assert(*(*view)->advanceClose() == rendering::ERenderClose::PENDING);
        }
        foreign = lux::cxx::unexpected(rendering::RendererFailure{});
        for (;;)
        {
            assert(Clock::now() < deadline && (*second)->poll(1));
            const auto closed = (*view)->advanceClose();
            assert(closed);
            if (*closed == rendering::ERenderClose::COMPLETE)
                break;
            std::this_thread::yield();
        }
        view->reset();
        assert(runtime->control().destroyScene(scene->get().scene_id));
        runtime = lux::cxx::unexpected(lux::scene::RenderRuntimeFailure{});
        for (auto *renderer : {first->get(), second->get()})
        {
            assert(renderer->beginClose());
            for (;;)
            {
                assert(Clock::now() < deadline);
                const auto closed = renderer->advanceClose();
                assert(closed);
                if (*closed == rendering::ERenderClose::COMPLETE)
                    break;
                std::this_thread::yield();
            }
            assert(renderer->joinStopped() && renderer->statistics().validation_errors == 0);
        }
        first->reset();
        second->reset();
        assert((*first_window)->closeAfterRendererStopped());
        assert((*second_window)->closeAfterRendererStopped());
        first_window->reset();
        second_window->reset();
        std::puts("foreign View PASS two real Renderer instances; rejected input retained; held lease gates close");
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
}
