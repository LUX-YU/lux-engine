#include "../../cmake/installed-consumers/common/RenderRegistration.hpp"
#include <cassert>
#include <cstdio>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/RenderContext.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/editor/EditorUiScene.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/LuxEngine.hpp>
#include <lux/engine/scene/RenderResources.hpp>
#include <lux/engine/scene/RenderViewRequest.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>
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
            : Pane(std::move(name)), deaths_(deaths),
              content_(ui::ElementId{"layout"}),
              label_(ui::ElementId{"label"}, "Actual UI GPU content")
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
    void nativeLifecycle()
    {
        assert(!LuxEngine::create({"Invalid extent", 0, 480}));
        std::vector<int> deaths;
        auto engine = take(LuxEngine::create({"Framework lifecycle qualification", 640, 480, 1}));
        auto* runtime = &engine->engine();
        auto* window = &engine->window();
        auto* root = &engine->uiRoot();
        class Listener final : public object::LuxObject
        {
        public:
            explicit Listener(LuxEngine& host) : LuxObject(), host_(host) {}
            void attached(const ui::PaneChanged&) noexcept
            {
                auto result = host_.closeProject();
                assert(
                    !result &&
                    result.error().type == error::errorId("lux.editor.project_change_inside_a_host_operation")
                );
                ++calls;
            }
            unsigned calls{};

        private:
            LuxEngine& host_;
        } listener(*engine);
        auto connection =
            object::LuxObject::connect(root, &ui::Root::paneChanged, &listener, &Listener::attached);
        assert(connection);
        auto assembly = [&](EditorContext& context) noexcept -> FrameworkResult<void>
        {
            assert(context.services().registerFactory<Service>(
                [&](EditorContext&) -> FrameworkResult<std::unique_ptr<Service>>
                { return std::make_unique<Service>(deaths); }
            ));
            return context.ui().registerFactory(
                "test",
                [&](EditorContext& context,
                    const PaneDescription& description) -> FrameworkResult<std::unique_ptr<ui::Pane>>
                {
                    assert(context.service<Service>());
                    return std::unique_ptr<ui::Pane>{new TestPane(description.name, deaths)};
                }
            );
        };
        const auto directory = std::filesystem::current_path();
        const EditorLayout layout{{"test", "one", "One"}, {"test", "two", "Two"}};
        assert(engine->openProject({"A", directory}, layout, assembly));
        unsigned waiting{};
        auto until = [&](auto predicate)
        {
            std::fprintf(stderr, "GPU wait %u\n", ++waiting);
            const auto deadline = std::chrono::steady_clock::now() + 20s;
            while (!predicate())
            {
                assert(std::chrono::steady_clock::now() < deadline);
                assert(take(engine->frame()));
                std::this_thread::sleep_for(1ms);
            }
        };
        until([&]
              { return engine->capturedFrames() >= 4 && runtime->renderContext()->runtime().statistics().frames > 0; });
        auto* original = engine->context();
        assert(!engine->openProject({"", directory}, layout, assembly) && engine->context() == original);
        const EditorLayout duplicate_names{{"test", "same", "One"}, {"test", "same", "Two"}};
        assert(!engine->openProject({"Invalid", directory}, duplicate_names, assembly));
        assert(engine->context() == original && root->panes().size() == 2);
        assert(engine->openProject({"B", directory}, layout, assembly));
        assert((deaths == std::vector<int>{1, 1, 2}));
        assert(&engine->engine() == runtime && &engine->window() == window && &engine->uiRoot() == root);
        auto state = window->state();
        assert(state);
        auto placement = state->placement;
        placement.normal.width = 720;
        placement.normal.height = 520;
        assert(window->applyPlacement(placement));
        auto before = engine->capturedFrames();
        until([&] { return engine->capturedFrames() > before + 2; });
#if defined(_WIN32)
        ShowWindow(static_cast<HWND>(window->nativeHandle()), SW_MINIMIZE);
        until([&] { return window->minimized(); });
        before = engine->capturedFrames();
        for (int i{}; i < 5; ++i)
        {
            assert(take(engine->frame()));
        }
        assert(engine->capturedFrames() == before);
        ShowWindow(static_cast<HWND>(window->nativeHandle()), SW_RESTORE);
        until([&] { return !window->minimized() && engine->capturedFrames() > before; });
#endif
        auto failing = [&](EditorContext& context) noexcept -> FrameworkResult<void>
        {
            assert(context.services().registerFactory<Service>(
                [&](EditorContext&) -> FrameworkResult<std::unique_ptr<Service>>
                { return std::make_unique<Service>(deaths); }
            ));
            return context.ui().registerFactory(
                "test",
                [&](EditorContext& context,
                    const PaneDescription& description) -> FrameworkResult<std::unique_ptr<ui::Pane>>
                {
                    assert(context.service<Service>());
                    if (description.name == "two")
                    {
                        return cxx::unexpected(error::makeError(
                            {"lux.editor.expected_second_factory_refusal",
                             "Expected second factory refusal",
                             error::ERecovery::PERMANENT}
                        ));
                    }
                    return std::unique_ptr<ui::Pane>{new TestPane(description.name, deaths)};
                }
            );
        };
        assert(!engine->openProject({"C", directory}, layout, failing));
        assert(!engine->context() && root->panes().empty());
        assert((deaths == std::vector<int>{1, 1, 2, 1, 1, 2, 1, 2}));
        assert(engine->openProject({"D", directory}, layout, assembly));
        until([&] { return engine->capturedFrames() > before + 2; });
        const auto statistics = runtime->renderContext()->runtime().statistics();
        assert(statistics.frames > 0);
        window->exit();
        assert(!take(engine->frame()));
        assert(listener.calls > 0);
        connection->disconnect();
        engine.reset(); // GPU work may still be in flight; original Runtime performs its retirement drain.
        assert(deaths.size() == 11 && deaths.back() == 2);
        std::printf(
            "PASS native framework: %llu renderer frames, resize/minimize, A->B, failed C, D and in-flight "
            "destruction\n",
            static_cast<unsigned long long>(statistics.frames)
        );
    }
    void offscreen()
    {
        render::RendererConfig config;
        config.validation = true;
        auto made_runtime = render::RenderRuntime::create(
            config,
            [](auto severity, auto text)
            {
                if (severity == 2)
                {
                    std::fprintf(stderr, "%.*s\n", int(text.size()), text.data());
                }
            }
        );
        assert(made_runtime);
        auto runtime = std::move(*made_runtime);
        registerRenderFeatures(*runtime, {render::kUiRenderRenderFeatureRegistration});
        auto execution = process::ExecutionRuntime::create({1, 64, 64, {64}});
        assert(execution);
        process::TaskScope tasks{*execution};
        auto made_resources = scene::RenderResources::create(*runtime, tasks, execution->cpu());
        assert(made_resources);
        auto resources = std::move(*made_resources);
        auto& messages = object::ObjectRuntime::instance();
        auto made_root = ui::Root::create();
        assert(made_root);
        auto root = std::move(*made_root);
        auto scenes_created = scene::SceneRuntime::create(*execution, {0, 128});
        assert(scenes_created);
        auto scenes = std::move(*scenes_created);
        auto configuration = ui::makeRenderConfiguration(*root);
        assert(configuration);
        auto ui_scene = take(EditorUiScene::create(
            *execution,
            *scenes,
            *runtime,
            *resources,
            std::move(*configuration),
            scene::ViewConfig{.extent = {640, 480}}
        ));
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
                assert(ui_scene->publishInput());
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
            assert(root->update({{640, 480}, 0.016F}, data, ui::Root::Capture{capture}));
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
                        assert(root->update({{640, 480}, 0.016F}, data, ui::Root::Capture{capture}));
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
            assert(root->update({{640, 480}, 0.016F}, data, ui::Root::Capture{capture}));
            pump();
        }
        auto empty = readPixels();
        assert(populated != empty);
        ui_scene.reset();
        resources->beginClose();
        until([&] { return resources->empty(); });
        scenes.reset();
        assert(runtime->statistics().validation_errors == 0);
        std::printf(
            "PASS UI RenderSystem GPU readback differs with Pane; pending frame retained; zero validation errors\n"
        );
    }
} // namespace
int main(int argc, char** argv)
{
    if (argc > 1 && std::string_view(argv[1]) == "--native")
    {
        nativeLifecycle();
    }
    else
    {
        offscreen();
    }
}
