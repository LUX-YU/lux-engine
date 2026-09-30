#include <lux/engine/editor/desktop/DesktopShell.hpp>
#include <lux/engine/editor/desktop/WindowInput.hpp>

namespace lux::editor::desktop
{
    struct DesktopShell::Impl final
    {
        class ShellRoot final : public lux::ui::Root
        {
        public:
            explicit ShellRoot(object::ObjectDispatcherRef dispatcher) : Root(dispatcher) {}
            [[nodiscard]] lux::cxx::expected<void, lux::ui::EInitError> start(lux::ui::RootConfig config) noexcept
            {
                return initialize(config);
            }
            Presentation* presentation{};

        private:
            lux::cxx::expected<void, lux::ui::ECaptureError> drawDataReady(const lux::ui::DrawData& data
            ) noexcept override
            {
                return presentation->captureDrawData(data);
            }
        };
        ShellRoot root_;
        std::unique_ptr<Presentation> presentation_;
        ViewHost host_;
        Impl(object::ObjectDispatcherRef dispatcher, ViewHostLimits limits) : root_(dispatcher), host_(root_, limits) {}
        ~Impl() noexcept
        {
            root_.closeInput();
            root_.bindWindow(nullptr);
        }
    };
    DesktopResult<std::unique_ptr<DesktopShell>> DesktopShell::create(
        object::ObjectDispatcherRef dispatcher,
        process::ExecutionRuntime& execution,
        lux::scene::SceneRuntime& scenes,
        render::RenderRuntime& renderer,
        lux::scene::RenderResources& resources,
        window::LuxWindow* window,
        lux::ui::RootConfig config,
        ViewHostLimits limits
    )
    {
        auto impl = std::make_unique<Impl>(dispatcher, limits);
        const auto started = impl->root_.start(config);
        if (!started)
            return cxx::unexpected(DesktopFailure{"desktop.root", started.error()});
        impl->root_.bindWindow(window);
        auto presentation = Presentation::create(impl->root_, execution, scenes, renderer, resources, window);
        if (!presentation)
            return cxx::unexpected(presentation.error());
        impl->presentation_ = std::move(*presentation);
        impl->root_.presentation = impl->presentation_.get();
        return std::unique_ptr<DesktopShell>(new DesktopShell(std::move(impl)));
    }
    DesktopShell::DesktopShell(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    DesktopShell::~DesktopShell() noexcept = default;
    lux::ui::Root& DesktopShell::root() noexcept
    {
        return impl_->root_;
    }
    ViewHost& DesktopShell::views() noexcept
    {
        return impl_->host_;
    }
    Presentation& DesktopShell::presentation() noexcept
    {
        return *impl_->presentation_;
    }
    DesktopResult<void> DesktopShell::feedInput(const input::InputSnapshot& input) noexcept
    {
        const auto result = feedWindowInput(impl_->root_, input);
        if (!result)
            return cxx::unexpected(DesktopFailure{"desktop.input", result.error()});
        return {};
    }
    DesktopResult<ViewDrain> DesktopShell::update(std::optional<lux::ui::FrameInfo> frame)
    {
        auto& root = impl_->root_;
        auto& presentation = *impl_->presentation_;
        root.applyPendingChanges();
        const auto information = frame ? *frame : presentation.frameInfo();
        auto* ui_draw_data = information.display_size.width > 0 ? presentation.tryAcquireDrawData() : nullptr;
        const auto updated = root.update(information, ui_draw_data);
        if (!updated)
            return cxx::unexpected(DesktopFailure{"desktop.update", updated.error()});
        root.applyPendingChanges();
        auto drained = impl_->host_.drain();
        if (!drained)
            return cxx::unexpected(DesktopFailure{"desktop.views", drained.error()});
        const auto applied = presentation.applySceneInput();
        if (!applied)
            return cxx::unexpected(applied.error());
        return *drained;
    }
}
