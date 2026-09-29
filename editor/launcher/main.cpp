#include <lux/engine/editor/launcher/ProjectCreationPane.hpp>
#include <lux/engine/editor/ui/Presentation.hpp>
#include <lux/engine/editor/ui/WindowOutput.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/editor/ui/WindowInput.hpp>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/EngineRendering.hpp>
#include <lux/engine/platform/Process.hpp>
#include <lux/engine/ui/rendering/RenderFeature.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/input/Input.hpp>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <exception>

namespace
{
    using namespace lux;
    class LauncherRoot final : public ui::Root
    {
    public:
        explicit LauncherRoot(object::ObjectDispatcherRef dispatcher) : Root(dispatcher) {}
        auto initializeUi() noexcept
        {
            return initialize({});
        }
        editor::ui::Presentation* presentation{};

    private:
        cxx::expected<void, ui::ECaptureError> drawDataReady(const ui::DrawData& data) noexcept override
        {
            return presentation->captureDrawData(data);
        }
    };
    class Launcher final
    {
        window::GlfwRuntime platform_;
        std::unique_ptr<window::LuxWindow> window_;
        std::unique_ptr<engine::EngineContext> engine_;
        std::optional<object::ObjectMessageQueue> messages_;
        std::unique_ptr<LauncherRoot> root_;
        std::unique_ptr<editor::ui::Presentation> ui_;
        std::unique_ptr<editor::ProjectCreationPane> creation_;
        input::Input input_;
        bool close_requested_{};
        int outcome_{};

        void fail(std::string_view operation) noexcept
        {
            std::fprintf(stderr, "launcher.%.*s failed\n", static_cast<int>(operation.size()), operation.data());
            outcome_ = 3;
            close_requested_ = true;
        }
        bool initialize(const std::filesystem::path& installation) noexcept
        {
            if (!platform_.valid())
                return false;
            window_ = std::make_unique<window::LuxWindow>(1100, 820, "Lux Launcher");
            if (!window_->isInitialized())
                return false;
            window_->on_close = [this](const window::WindowCloseEvent&) { close_requested_ = true; };
            auto queue = object::ObjectMessageQueue::create(64);
            if (!queue)
            {
                if (queue.error() == object::EObjectQueueError::ALLOCATION_FAILURE)
                    std::terminate();
                return false;
            }
            messages_.emplace(std::move(*queue));
            messages_->setWake(&window::LuxWindow::wakeEvents);
            engine_->execution().setWake(&window::LuxWindow::wakeEvents);
            if (!engine::initializeRendering(*engine_, window::LuxWindow::requiredVulkanInstanceExtensions()))
                return false;
            auto& rendering = *engine_->renderContext();
            auto& runtime = rendering.runtime();
            if (!rendering.registerFeatures({render::kUiRenderRenderFeatureRegistration}))
                return false;
            root_ = std::make_unique<LauncherRoot>(messages_->dispatcherRef());
            if (!root_->initializeUi())
                return false;
            root_->bindWindow(window_.get());
            auto presentation = editor::ui::Presentation::create(
                *root_,
                engine_->execution(),
                engine_->sceneRuntime(),
                runtime,
                rendering.resources(),
                window_.get()
            );
            if (!presentation)
                return false;
            ui_ = std::move(*presentation);
            root_->presentation = ui_.get();
            auto pane = editor::ProjectCreationPane::create(*root_, engine_->execution(), installation);
            if (!pane)
                return false;
            creation_ = std::move(*pane);
            return true;
        }
        void dispatchCompletions() noexcept
        {
            if (!engine_->execution().collectCompletions())
                fail("main");
            if (!engine_->execution().dispatchTaskEvents())
                fail("tasks");
            if (messages_)
                static_cast<void>(messages_->dispatchPending());
        }
        void waitForWork() noexcept
        {
            if (engine_->execution().hasPendingWork() || messages_->statistics().pending)
                return;
            const auto deadline = ui_->nextFrameTime();
            const auto now = std::chrono::steady_clock::now();
            if (deadline <= now)
                return;
            if (deadline == std::chrono::steady_clock::time_point::max())
                window::LuxWindow::waitEvents();
            else
                window::LuxWindow::waitEvents(std::chrono::duration<double>(deadline - now).count());
        }

    public:
        explicit Launcher(std::unique_ptr<engine::EngineContext> engine) : engine_(std::move(engine)) {}
        ~Launcher() noexcept
        {
            if (window_)
                window_->hide(true);
            creation_.reset();
            if (root_)
            {
                root_->closeInput();
                root_->bindWindow(nullptr);
            }
            ui_.reset();
            root_.reset();
            if (messages_)
                messages_->close();
        }
        int run(const std::filesystem::path& installation, unsigned smoke_frames)
        {
            if (!initialize(installation))
                fail("initialize");
            unsigned frames{};
            while (!close_requested_ && creation_ && !creation_->closed())
            {
                window::LuxWindow::pollEvents();
                input_.sample(*window_);
                if (!editor::ui::feedWindowInput(*root_, input_.snapshot()))
                    fail("input");
                dispatchCompletions();
                const auto frame = ui_->frameInfo();
                auto* data = frame.display_size.width > 0 ? ui_->tryAcquireDrawData() : nullptr;
                if (!root_->update(frame, data))
                    fail("ui.update");
                if (!ui_->applySceneInput())
                    fail("ui.submit");
                const auto advanced = engine_->sceneRuntime().driveFrame();
                if (!advanced || !advanced->empty())
                    fail("scenes");
                if (smoke_frames && ui_->capturedFrames() >= smoke_frames)
                    close_requested_ = true;
                if (smoke_frames && ++frames > 5000)
                    fail("smoke.timeout");
                if (!close_requested_ && !creation_->closed())
                    waitForWork();
            }
            return outcome_;
        }
    };
}

int main(int argc, char** argv)
{
    auto arguments = lux::engine::platform::processArguments(argc, argv);
    auto executable = lux::engine::platform::executablePath();
    if (!arguments || !executable)
        return 2;
    const bool help = std::ranges::find(*arguments, "--help") != arguments->end();
    if (help)
    {
        std::puts("lux_launcher: create a project and open it in a new Editor process.");
        return 0;
    }
    const bool smoke = std::ranges::find(*arguments, "--smoke") != arguments->end();
    auto engine =
        lux::engine::EngineContext::create({2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}}, {0, 1024});
    if (!engine)
        return 3;
    Launcher launcher{std::move(*engine)};
    return launcher.run(executable->parent_path().parent_path(), smoke ? 8 : 0);
}
