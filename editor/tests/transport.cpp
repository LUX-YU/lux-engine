#include "../../cmake/installed-consumers/common/RenderRegistration.hpp"
#include "../../cmake/installed-consumers/common/UiTestContent.hpp"
#include <cassert>
#include <cstdio>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/EngineRendering.hpp>
#include <lux/engine/RenderContext.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/LuxEngine.hpp>
#include <lux/engine/editor/detail/EditorUiScene.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/scene/RenderResources.hpp>
#include <lux/engine/scene/RenderViewRequest.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/rendering/RenderFeature.hpp>
#include <thread>
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace lux;
using namespace lux::editor;
using namespace std::chrono_literals;
namespace FixtureErrors
{
    inline constexpr lux::error::ErrorId EditorExpectedSecondFactoryRefusal =
        lux::error::errorId("lux.editor.expected_second_factory_refusal");
}

namespace
{
    struct Service final
    {
        std::vector<int>& deaths;
        ~Service()
        {
            deaths.push_back(2);
        }
    };
    class TestPane final : public ui::Pane
    {
    public:
        TestPane(std::string name, std::vector<int>& deaths)
            : Pane(std::move(name)), deaths_(deaths), content_{}, label_("Actual UI GPU content")
        {
            assert(content_.addElement(label_) && addElement(content_));
        }
        ~TestPane() override
        {
            deaths_.push_back(1);
        }

    private:
        std::vector<int>& deaths_;
        ui::Layout content_;
        ui::Label label_;
    };
    template <class T> T take(FrameworkResult<T> result)
    {
        if (!result)
        {
            std::fprintf(stderr, "framework error: %s\n", error::format(result.error()).c_str());
        }
        assert(result);
        return std::move(*result);
    }
    void offscreen()
    {
        auto created_engine = engine::EngineContext::create({1, 64, 64, {64}}, {0, 128});
        assert(created_engine);
        auto engine = std::move(*created_engine);
        assert(engine::initializeRendering(*engine, {}));
        auto* rendering = engine->renderContext();
        assert(rendering && rendering->registerFeatures({render::kUiRenderRenderFeatureRegistration}));
        auto* runtime = &rendering->runtime();
        auto* resources = &rendering->resources();
        auto* execution = &engine->execution();
        auto* scenes = &engine->sceneRuntime();
        auto made_root = ui::Root::create();
        assert(made_root);
        auto root = std::move(*made_root);
        auto configuration = ui::makeRenderConfiguration(*root);
        assert(configuration);
        auto ui_scene =
            take(EditorUiScene::create(*engine, std::move(*configuration), scene::ViewConfig{.extent = {640, 480}}));
        std::vector<int> deaths;
        std::unique_ptr<ui::Pane> pane = std::make_unique<TestPane>("gpu", deaths);
        assert(root->addPane(std::move(pane)));
        const auto pump = [&]
        {
            std::size_t controls = 8, programs = 4;
            assert(runtime->collectCompletions(32));
            assert(runtime->submitPending(controls, programs));
            assert(execution->collectCompletions());
            if (ui_scene)
            {
                assert(ui_scene->publishFrame());
            }
            auto driven = scenes->driveFrame();
            assert(driven && driven->empty());
        };
        unsigned waiting{};
        auto until = [&](auto predicate)
        {
            std::fprintf(stderr, "GPU wait %u\n", ++waiting);
            const auto deadline = std::chrono::steady_clock::now() + 20s;
            while (!predicate())
            {
                assert(std::chrono::steady_clock::now() < deadline);
                pump();
                std::this_thread::sleep_for(1ms);
            }
        };
        until([&] { return ui_scene->outputReady(); });
        scene::RenderResourceId view;
        {
            auto registry = std::as_const(*scenes).borrowInstance(ui_scene->sceneId());
            assert(registry);
            for (auto entity : registry->get().view<scene::RenderViewResult>())
            {
                view = registry->get().get<scene::RenderViewResult>(entity).view;
            }
        }
        assert(view.isValid());
        auto capture = [&](const ui::DrawData& data) noexcept { return ui_scene->captureDrawData(data); };
        for (unsigned i{}; i < 8; ++i)
        {
            ui::DrawData* data{};
            until(
                [&]
                {
                    data = ui_scene->acquireDrawData();
                    return data != nullptr;
                }
            );
            assert(root->update({{640, 480}, 0.016F}, *data, ui::Root::Capture{capture}));
            assert(ui_scene->acquireDrawData() == nullptr); // Captured pending frame is never redrawn.
            pump();
        }
        until([&] { return runtime->statistics().gpu_completed > 0; });
        auto readPixels = [&]
        {
            auto output = resources->viewOutput(view);
            assert(output);
            auto info = resources->outputInfo(*output);
            assert(info);
            std::vector<std::byte> bytes(std::size_t(info->extent.width) * info->extent.height * 8);
            auto control = runtime->control();
            assert(control);
            auto request = control->get().readbackTargetAsync(info->target, bytes.data(), bytes.size(), 4);
            until(
                [&]
                {
                    if (request.isReady())
                    {
                        return true;
                    }
                    // Readback settles on render ticks. Keep producing frames, with the same backpressure contract.
                    if (auto* data = ui_scene->acquireDrawData())
                    {
                        assert(root->update({{640, 480}, 0.016F}, *data, ui::Root::Capture{capture}));
                    }
                    return false;
                }
            );
            auto response = request.tryResult();
            assert(response && response->get().status == 0 && response->get().bytes_written > 0);
            bytes.resize(response->get().bytes_written);
            return bytes;
        };
        auto populated = readPixels();
        assert(root->clearPanes());
        for (unsigned i{}; i < 8; ++i)
        {
            ui::DrawData* data{};
            until(
                [&]
                {
                    data = ui_scene->acquireDrawData();
                    return data != nullptr;
                }
            );
            assert(root->update({{640, 480}, 0.016F}, *data, ui::Root::Capture{capture}));
            pump();
        }
        auto empty = readPixels();
        assert(populated != empty);
        ui_scene.reset();
        resources->beginClose();
        until([&] { return resources->empty(); });
        assert(runtime->statistics().validation_errors == 0);
        std::printf(
            "PASS UI RenderSystem GPU readback differs with Pane; pending frame retained; zero validation errors\n"
        );
    }
} // namespace
int main()
{
    offscreen();
}
