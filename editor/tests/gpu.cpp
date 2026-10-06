#include "../../cmake/installed-consumers/common/UiTestContent.hpp"
#include <cassert>
#include <cstdio>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/RenderContext.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/LuxEngine.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/function/render/runtime/RenderRuntime.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
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
            : Pane(std::move(name)), deaths_(deaths), content_{}, label_("Actual UI GPU content"), draw_probe_(*this)
        {
            assert(content_.addElement(label_) && content_.addElement(draw_probe_) && addElement(content_));
        }
        inline static std::uint64_t draws{};
        void drawTestContent(ui::Element&) noexcept
        {
            ++draws;
        }
        ~TestPane() override
        {
            deaths_.push_back(1);
        }

    private:
        std::vector<int>& deaths_;
        ui::Layout content_;
        ui::Label label_;
        TUiTestContent<TestPane> draw_probe_;
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
        auto* root = &engine->window().uiRoot();
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
        auto connection = object::LuxObject::connect(root, &ui::Root::paneChanged, &listener, &Listener::attached);
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
                assert(take(engine->frame()) == EFrameStatus::RUNNING);
                std::this_thread::sleep_for(1ms);
            }
        };
        until([&] { return TestPane::draws >= 4 && runtime->renderContext()->runtime().statistics().frames > 0; });
        auto* original = engine->context();
        assert(!engine->openProject({"", directory}, layout, assembly) && engine->context() == original);
        const EditorLayout duplicate_names{{"test", "same", "One"}, {"test", "same", "Two"}};
        assert(!engine->openProject({"Invalid", directory}, duplicate_names, assembly));
        assert(engine->context() == original && ui_test::paneCount(*root) == 2);
        assert(engine->openProject({"B", directory}, layout, assembly));
        assert((deaths == std::vector<int>{1, 1, 2}));
        assert(&engine->engine() == runtime && &engine->window() == window && &engine->window().uiRoot() == root);
        auto state = window->state();
        assert(state);
        auto placement = state->placement;
        placement.normal.width = 720;
        placement.normal.height = 520;
        assert(window->applyPlacement(placement));
        auto before = TestPane::draws;
        until([&] { return TestPane::draws > before + 2; });
#if defined(_WIN32)
        ShowWindow(static_cast<HWND>(window->nativeHandle()), SW_MINIMIZE);
        until([&] { return window->minimized(); });
        before = TestPane::draws;
        for (int i{}; i < 5; ++i)
        {
            assert(take(engine->frame()) == EFrameStatus::RUNNING);
        }
        assert(TestPane::draws == before);
        ShowWindow(static_cast<HWND>(window->nativeHandle()), SW_RESTORE);
        until([&] { return !window->minimized() && TestPane::draws > before; });
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
                        return cxx::unexpected(error::Error{FixtureErrors::EditorExpectedSecondFactoryRefusal, {}});
                    }
                    return std::unique_ptr<ui::Pane>{new TestPane(description.name, deaths)};
                }
            );
        };
        assert(!engine->openProject({"C", directory}, layout, failing));
        assert(!engine->context() && (ui_test::paneCount(*root) == 0));
        assert((deaths == std::vector<int>{1, 1, 2, 1, 1, 2, 1, 2}));
        assert(engine->openProject({"D", directory}, layout, assembly));
        until([&] { return TestPane::draws > before + 2; });
        const auto statistics = runtime->renderContext()->runtime().statistics();
        assert(statistics.frames > 0);
        window->exit();
        assert(take(engine->frame()) == EFrameStatus::EXIT_REQUESTED);
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
} // namespace
int main(int argc, char** argv)
{
    const lux::error::ErrorDescriptor fixture_errors[]{
        {"lux.editor.expected_second_factory_refusal",
         "Expected second factory refusal",
         lux::error::ERecovery::PERMANENT}
    };
    assert(lux::error::ErrorRegistry::instance().registerTypes(fixture_errors));

    nativeLifecycle();
}
