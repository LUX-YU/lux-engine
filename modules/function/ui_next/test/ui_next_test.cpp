#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include <lux/engine/ui_next/UiNext.hpp>

#include <array>
#include <memory>

namespace
{
    class TestPane final : public lux::object::Object<TestPane, lux::ui::Pane>
    {
    public:
        TestPane(
            lux::object::ObjectDispatcherRef dispatcher,
            lux::ui::PaneId id,
            std::string title
        )
            : Object(
                  dispatcher,
                  std::move(id),
                  lux::ui::PaneTypeId{"test.pane"},
                  std::move(title)
              )
        {
        }

        [[nodiscard]] std::span<const lux::ui::UiContextIdView>
        contexts() const noexcept override
        {
            return contexts_;
        }

        void contextualDelete() { ++contextual_count; }
        void globalDelete() { ++global_count; }
        [[nodiscard]] bool contextualEnabled() const { return false; }

        int contextual_count{0};
        int global_count{0};

    protected:
        void draw(lux::ui::PaneDrawContext& context) override
        {
            context.activateContext(lux::ui::UiContextIdView{"test.local.first"});
            context.activateContext(lux::ui::UiContextIdView{"test.local.second"});
            ImGui::TextUnformatted("UI vNext");
        }

    private:
        std::array<lux::ui::UiContextIdView, 1> contexts_{
            lux::ui::UiContextIdView{"test.selection"}
        };
    };

    class Receiver final : public lux::object::Object<Receiver>
    {
    public:
        void invoke() { ++count; }
        int count{0};
    };
} // namespace

int main()
{
    auto* external_context = ImGui::CreateContext();
    ImGui::SetCurrentContext(external_context);
    {
        lux::ui::UISession first_session;
        lux::ui::UISession second_session;
        assert(first_session.imguiContext() != second_session.imguiContext());
        assert(ImGui::GetCurrentContext() == external_context);
        first_session.beginFrame({64.0F, 64.0F}, 1.0F / 60.0F);
        static_cast<void>(first_session.endFrame());
        assert(ImGui::GetCurrentContext() == external_context);
        second_session.beginFrame({64.0F, 64.0F}, 1.0F / 60.0F);
        static_cast<void>(second_session.endFrame());
        assert(ImGui::GetCurrentContext() == external_context);
    }
    {
        lux::ui::UISession session;
        assert(ImGui::GetCurrentContext() == external_context);
        TestPane first{session.dispatcherRef(), lux::ui::PaneId{"first"}, "First"};
        TestPane duplicate{
            session.dispatcherRef(),
            lux::ui::PaneId{"first"},
            "Duplicate"
        };
        TestPane second{session.dispatcherRef(), lux::ui::PaneId{"second"}, "Second"};

        auto first_registration = session.registerPane(first);
        assert(first_registration);
        auto duplicate_registration = session.registerPane(duplicate);
        assert(!duplicate_registration);
        auto second_registration = session.registerPane(second);
        assert(second_registration);

        auto factory_registration = session.registerFactory(lux::ui::PaneFactory{
            lux::ui::PaneTypeId{"test.pane"},
            "Test Pane",
            [](lux::ui::PaneCreateContext context,
               lux::ui::PaneId id) -> std::unique_ptr<lux::ui::Pane>
            {
                return std::make_unique<TestPane>(
                    std::move(context.dispatcher),
                    std::move(id),
                    "Created"
                );
            }
        });
        assert(factory_registration);
        auto duplicate_factory = session.registerFactory(lux::ui::PaneFactory{
            lux::ui::PaneTypeId{"test.pane"},
            "Duplicate",
            [](lux::ui::PaneCreateContext,
               lux::ui::PaneId) -> std::unique_ptr<lux::ui::Pane> { return {}; }
        });
        assert(!duplicate_factory);
        auto created = session.createPane(
            lux::ui::PaneTypeIdView{"test.pane"},
            lux::ui::PaneId{"created"}
        );
        assert(created);

        int focus_changes = 0;
        bool last_focus = false;
        auto focus_connection = first.observeScoped<lux::ui::Pane::focusChanged>(
            [&](const lux::ui::PaneFocusChanged& change) noexcept
            {
                ++focus_changes;
                last_focus = change.focused;
            }
        );
        assert(focus_connection);

        int visibility_changes = 0;
        auto visibility_connection =
            first.observeScoped<lux::ui::Pane::visibilityChanged>(
                [&](const lux::ui::PaneVisibilityChanged&) noexcept
                { ++visibility_changes; }
            );
        assert(visibility_connection);
        first.setVisible(false);
        first.setVisible(true);
        assert(visibility_changes == 2);

        auto& router = session.commandRouter();
        auto delete_command =
            router.defineCommand({lux::ui::UiCommandId{"delete"}, "Delete"});
        assert(delete_command);
        auto duplicate_command =
            router.defineCommand({lux::ui::UiCommandId{"delete"}, "Delete Again"});
        assert(!duplicate_command);
        auto contextual =
            router.bind<&TestPane::contextualDelete, &TestPane::contextualEnabled>(
                *delete_command,
                lux::ui::UiContextId{"test.selection"},
                first,
                first
            );
        assert(contextual);
        auto duplicate_binding = router.bind<&TestPane::contextualDelete>(
            *delete_command,
            lux::ui::UiContextId{"test.selection"},
            first,
            first
        );
        assert(!duplicate_binding);
        auto second_contextual = router.bind<&TestPane::contextualDelete>(
            *delete_command,
            lux::ui::UiContextId{"test.selection"},
            second,
            second
        );
        assert(second_contextual);
        auto global =
            router.bindGlobal<&TestPane::globalDelete>(*delete_command, first);
        assert(global);

        assert(session.focusPane(first.id().view()));
        assert(first.focused());
        assert(focus_changes == 1 && last_focus);
        assert(session.focusedContexts().size() == 2);
        assert(
            session.focusedContexts()[0] == lux::ui::UiContextIdView{"test.selection"}
        );
        assert(router.state(*delete_command).found);
        assert(!router.state(*delete_command).enabled);
        assert(
            router.invoke(*delete_command) == lux::ui::ECommandDispatchResult::DISABLED
        );
        assert(first.contextual_count == 0);
        assert(first.global_count == 0);
        contextual->reset();
        assert(
            router.invoke(*delete_command) == lux::ui::ECommandDispatchResult::EXECUTED
        );
        assert(first.global_count == 1);

        assert(session.focusPane(second.id().view()));
        assert(!first.focused());
        assert(focus_changes == 2 && !last_focus);
        assert(
            router.invoke(*delete_command) == lux::ui::ECommandDispatchResult::EXECUTED
        );
        assert(second.contextual_count == 1);
        assert(first.global_count == 1);

        lux::ui::CommandRegistration dead_binding;
        {
            Receiver receiver;
            const auto temporary =
                router.defineCommand({lux::ui::UiCommandId{"temporary"}, "Temporary"});
            assert(temporary);
            auto binding = router.bindGlobal<&Receiver::invoke>(*temporary, receiver);
            assert(binding);
            dead_binding = std::move(*binding);
        }
        const auto temporary =
            router.findCommand(lux::ui::UiCommandIdView{"temporary"});
        assert(temporary);
        assert(router.invoke(*temporary) == lux::ui::ECommandDispatchResult::NOT_FOUND);

        session.beginFrame({800.0F, 600.0F}, 1.0F / 60.0F);
        session.drawPanes();
        assert(session.endFrame() != nullptr);
        const auto layout = session.captureLayout();
        assert(!layout.bytes.empty());
        assert(session.restoreLayout(layout.bytes));
        session.feedInput(lux::ui::UiPointerMove{20.0F, 30.0F});
        session.feedInput(
            lux::ui::UiPointerButton{lux::ui::EUiPointerButton::LEFT, true}
        );
        session.beginFrame({800.0F, 600.0F}, 1.0F / 60.0F);
        session.drawPanes();
        static_cast<void>(session.endFrame());
        assert(ImGui::GetCurrentContext() == external_context);

        lux::ui::MenuModel menu{{lux::ui::MenuItem{
            lux::ui::EMenuItemKind::COMMAND,
            "Delete",
            lux::ui::CommandPresentation{*delete_command},
            {}
        }}};
        lux::ui::ToolbarModel toolbar{{lux::ui::ToolbarItem{
            lux::ui::EToolbarItemKind::COMMAND,
            lux::ui::CommandPresentation{*delete_command}
        }}};
        assert(
            menu.items.front().presentation.command ==
            toolbar.items.front().presentation.command
        );

        second_registration->reset();
        auto replacement_registration = session.registerPane(duplicate);
        assert(!replacement_registration);
        first_registration->reset();
        replacement_registration = session.registerPane(duplicate);
        assert(replacement_registration);
    }
    assert(ImGui::GetCurrentContext() == external_context);
    ImGui::SetCurrentContext(external_context);
    ImGui::DestroyContext(external_context);
}
