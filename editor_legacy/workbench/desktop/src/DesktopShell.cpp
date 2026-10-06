#include <lux/engine/editor/desktop/DesktopShell.hpp>
#include <lux/engine/editor/WindowInput.hpp>
#include <lux/engine/object/ObjectEvent.hpp>

namespace lux::editor::desktop
{
#if defined(LUX_DESKTOP_TEST_ACCESS)
    namespace testing
    {
        thread_local bool reject_menu_connection{};
        void rejectNextMenuConnection() noexcept
        {
            reject_menu_connection = true;
        }
    } // namespace testing
#endif

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
            void clearWindows() noexcept
            {
                clearChildren();
            }
            Presentation* presentation{};
            // This synchronous private route is the only menu receiver. The request is a stack borrow;
            // no queued connection or public signal endpoint can retain it beyond event dispatch.
            object::TSignal<lux::ui::MenuRequest*> menuRequested{*this};
            void event(object::EventView& event) noexcept override
            {
                if (auto* request = event.getIf<lux::ui::MenuRequest>())
                {
                    (void)emit(menuRequested, request);
                    event.accept();
                }
            }

        private:
            lux::cxx::expected<void, lux::ui::ECaptureError> drawDataReady(const lux::ui::DrawData& data
            ) noexcept override
            {
                return presentation->captureDrawData(data);
            }
        };
        ShellRoot root_;
        std::unique_ptr<Presentation> presentation_;
        std::unique_ptr<CommandMenu> menu_;
        object::Connection menu_connection_;
        explicit Impl(object::ObjectDispatcherRef dispatcher) : root_(dispatcher) {}
        ~Impl() noexcept
        {
            root_.clearWindows();
            menu_connection_.disconnect();
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
        lux::ui::RootConfig config
    )
    {
        auto impl = std::make_unique<Impl>(dispatcher);
        const auto started = impl->root_.start(config);
        if (!started)
        {
            return cxx::unexpected(DesktopFailure{"desktop.root", started.error()});
        }
        impl->root_.bindWindow(window);
        auto presentation = Presentation::create(impl->root_, execution, scenes, renderer, resources, window);
        if (!presentation)
        {
            return cxx::unexpected(presentation.error());
        }
        impl->presentation_ = std::move(*presentation);
        impl->root_.presentation = impl->presentation_.get();
        return std::unique_ptr<DesktopShell>(new DesktopShell(std::move(impl)));
    }
    DesktopShell::DesktopShell(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    DesktopShell::~DesktopShell() noexcept = default;
    commands::CommandResult<void> DesktopShell::installCommands(
        commands::CommandRegistry& registry,
        commands::CommandDispatcher& dispatcher,
        CommandMenu::Capture capture
    )
    {
        if (impl_->menu_ || !capture)
        {
            return cxx::unexpected(
                commands::CommandFailure{commands::ECommandError::INVALID_ARGUMENT, "desktop.commands"}
            );
        }
        auto menu = std::make_unique<CommandMenu>(impl_->root_, registry, dispatcher, std::move(capture));
        auto connected = object::LuxObject::connect(
            &impl_->root_,
            &Impl::ShellRoot::menuRequested,
            [receiver = menu.get()](lux::ui::MenuRequest* request) noexcept { receiver->receive(*request); }
        );
#if defined(LUX_DESKTOP_TEST_ACCESS)
        if (std::exchange(testing::reject_menu_connection, false))
        {
            connected = cxx::unexpected(object::EConnectError::CAPACITY_EXHAUSTED);
        }
#endif
        if (!connected)
        {
            return cxx::unexpected(commands::CommandFailure{
                commands::ECommandError::DOMAIN_FAILURE,
                "object.connect",
                static_cast<std::uint64_t>(connected.error()),
                "Desktop menu route could not be installed"
            });
        }
        auto installed = menu->update();
        if (!installed)
        {
            return installed;
        }
        impl_->menu_ = std::move(menu);
        impl_->menu_connection_ = std::move(*connected);
        return {};
    }
    CommandMenu* DesktopShell::commands() noexcept
    {
        return impl_->menu_.get();
    }
    lux::ui::Root& DesktopShell::root() noexcept
    {
        return impl_->root_;
    }
    Presentation& DesktopShell::presentation() noexcept
    {
        return *impl_->presentation_;
    }
    DesktopResult<void> DesktopShell::feedInput(const input::InputSnapshot& input) noexcept
    {
        const auto result = feedWindowInput(impl_->root_, input);
        if (!result)
        {
            return cxx::unexpected(DesktopFailure{"desktop.input", result.error()});
        }
        return {};
    }
    DesktopResult<void> DesktopShell::update(std::optional<lux::ui::FrameInfo> frame)
    {
        auto& root = impl_->root_;
        auto& presentation = *impl_->presentation_;
        root.applyPendingChanges();
        const auto information = frame ? *frame : presentation.frameInfo();
        auto* ui_draw_data = information.display_size.width > 0 ? presentation.tryAcquireDrawData() : nullptr;
        const auto updated = root.update(information, ui_draw_data);
        if (!updated)
        {
            return cxx::unexpected(DesktopFailure{"desktop.update", updated.error()});
        }
        root.applyPendingChanges();
        if (impl_->menu_)
        {
            const auto commands = impl_->menu_->update();
            if (!commands)
            {
                return cxx::unexpected(DesktopFailure{"desktop.commands", commands.error()});
            }
        }
        const auto applied = presentation.applySceneInput();
        if (!applied)
        {
            return cxx::unexpected(applied.error());
        }
        return {};
    }
} // namespace lux::editor::desktop
