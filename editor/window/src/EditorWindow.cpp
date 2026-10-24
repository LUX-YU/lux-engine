#include <algorithm>
#include <limits>
#include <lux/engine/editor/EditorUiErrors.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/WindowInput.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>

namespace lux::editor
{
    EditorWindow::EditorWindow(NativeWindowOwner native, std::unique_ptr<ui::Root> root) noexcept
        : LuxWindow(std::move(native)), root_(std::move(root))
    {
        root_->bindWindow(this);
        size(metrics_.width, metrics_.height);
        framebufferSize(metrics_.framebuffer_width, metrics_.framebuffer_height);
        metrics_.minimized = minimized();
        on_resize = [owner = this](const window::WindowResizeEvent& event) noexcept
        {
            auto& metrics = owner->metrics_;
            const bool changed = metrics.width != event.width || metrics.height != event.height;
            if (changed)
            {
                metrics.width = event.width;
                metrics.height = event.height;
                owner->metricsChanged();
            }
        };
        on_framebuffer_resize = [owner = this](const window::FramebufferResizeEvent& event) noexcept
        {
            auto& metrics = owner->metrics_;
            const bool changed = metrics.framebuffer_width != event.width || metrics.framebuffer_height != event.height;
            if (changed)
            {
                metrics.framebuffer_width = event.width;
                metrics.framebuffer_height = event.height;
                owner->metricsChanged();
            }
        };
        on_minimized = [owner = this](const window::WindowMinimizedEvent& event) noexcept
        {
            if (owner->metrics_.minimized != event.minimized)
            {
                owner->metrics_.minimized = event.minimized;
                owner->metricsChanged();
            }
        };
    }

    EditorWindow::~EditorWindow() = default;

    FrameworkResult<std::unique_ptr<EditorWindow>> EditorWindow::create(const window::InitParameter& config) noexcept
    {
        if (auto registered = registerEditorUiErrors(); !registered)
        {
            return cxx::unexpected(registered.error());
        }
        static std::unique_ptr<window::GlfwRuntime> runtime;
        if (!runtime)
        {
            auto created = window::GlfwRuntime::create();
            if (!created)
            {
                return cxx::unexpected(error::Error{Errors::EditorGlfwInitializationFailed, {}});
            }
            runtime = std::move(*created);
        }
        auto native = prepareNative(config);
        if (!native)
        {
            return cxx::unexpected(
                error::Error{Errors::EditorNativeWindowCreationFailed, {static_cast<std::uint64_t>(native.error())}}
            );
        }
        auto root = ui::Root::create();
        if (!root)
        {
            return cxx::unexpected(
                error::Error{Errors::EditorRootInitializationFailed, {static_cast<std::uint64_t>(root.error())}}
            );
        }
        return std::unique_ptr<EditorWindow>(new EditorWindow(std::move(*native), std::move(*root)));
    }

    void EditorWindow::metricsChanged() noexcept
    {
        if (++metrics_.revision == 0)
        {
            std::terminate();
        }
        if (metrics_changed_)
        {
            metrics_changed_(metrics_receiver_, metrics_);
        }
        wakeEvents();
    }

    void EditorWindow::setMetricsReceiver(
        void* receiver,
        void (*changed)(void*, const WindowMetrics&) noexcept
    ) noexcept
    {
        metrics_receiver_ = receiver;
        metrics_changed_ = changed;
        if (changed)
        {
            changed(receiver, metrics_);
        }
    }

    FrameworkResult<void> EditorWindow::updateUi(ui::DrawData& output) noexcept
    {
        return updateUiFrame(&output);
    }

    FrameworkResult<void> EditorWindow::maintainUi() noexcept
    {
        return updateUiFrame(nullptr);
    }

    FrameworkResult<void> EditorWindow::updateUiFrame(ui::DrawData* output) noexcept
    {
        const auto now = std::chrono::steady_clock::now();
        const float elapsed =
            std::max(std::chrono::duration<float>(now - last_ui_frame_).count(), std::numeric_limits<float>::min());
        const ui::FrameInfo info{
            {float(metrics_.width), float(metrics_.height)},
            elapsed,
            {metrics_.width ? float(metrics_.framebuffer_width) / metrics_.width : 1.F,
             metrics_.height ? float(metrics_.framebuffer_height) / metrics_.height : 1.F}
        };
        const auto updated = output ? root_->update(info, *output) : root_->update();
        if (!updated)
        {
            return cxx::unexpected(error::Error{Errors::UiCapture, {static_cast<std::uint64_t>(updated.error())}});
        }
        if (output)
        {
            last_ui_frame_ = now;
        }
        const auto consumed = root_->inputSnapshot();
        input_.evaluate(elapsed, output && !consumed.keyboard_captured, output && !consumed.pointer_captured);
        return {};
    }

    ui::Root& EditorWindow::uiRoot() noexcept
    {
        return *root_;
    }

    FrameworkResult<void> EditorWindow::sampleInput() noexcept
    {
        input_.sample(*this);
        auto fed = ui::feedWindowInput(*root_, input_.snapshot());
        if (!fed)
        {
            return cxx::unexpected(
                error::Error{Errors::EditorNativeInputDeliveryFailed, {static_cast<std::uint64_t>(fed.error())}}
            );
        }
        return {};
    }
} // namespace lux::editor
