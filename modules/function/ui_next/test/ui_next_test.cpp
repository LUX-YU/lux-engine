#include <lux/engine/ui_next/UiNext.hpp>

#include <array>
#include <cassert>
#include <memory>

namespace
{
    class TestPane final : public lux::object::Object<TestPane, lux::ui::Pane>
    {
      public:
        TestPane(
            lux::object::ObjectDispatcher& dispatcher,
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

        [[nodiscard]] std::span<const lux::ui::UiContextId>
        contexts() const noexcept override
        {
            return contexts_;
        }

        void contextualDelete() { ++contextual_count; }
        void globalDelete() { ++global_count; }

        int contextual_count{0};
        int global_count{0};

      protected:
        void draw(lux::ui::PaneDrawContext& context) override
        {
            context.activateContext(
                lux::ui::UiContextId{"test.local.first"}
            );
            context.activateContext(
                lux::ui::UiContextId{"test.local.second"}
            );
            ImGui::TextUnformatted("UI vNext");
        }

      private:
        std::array<lux::ui::UiContextId, 1> contexts_{
            lux::ui::UiContextId{"test.selection"}
        };
    };

    class Receiver final : public lux::object::Object<Receiver>
    {
      public:
        void invoke() { ++count; }
        int count{0};
    };
}

int main()
{
    lux::ui::UISession session;
    TestPane first{session.dispatcher(), lux::ui::PaneId{"first"}, "First"};
    TestPane duplicate{
        session.dispatcher(),
        lux::ui::PaneId{"first"},
        "Duplicate"
    };
    TestPane second{
        session.dispatcher(),
        lux::ui::PaneId{"second"},
        "Second"
    };

    auto first_registration = session.registerPane(first);
    assert(first_registration);
    auto duplicate_registration = session.registerPane(duplicate);
    assert(!duplicate_registration);
    auto second_registration = session.registerPane(second);
    assert(second_registration);

    auto factory_registration = session.registerFactory(lux::ui::PaneFactory{
        lux::ui::PaneTypeId{"test.pane"},
        "Test Pane",
        [](lux::ui::PaneCreateContext context, lux::ui::PaneId id)
            -> std::unique_ptr<lux::ui::Pane>
        {
            return std::make_unique<TestPane>(
                context.dispatcher,
                std::move(id),
                "Created"
            );
        }
    });
    assert(factory_registration);
    auto duplicate_factory = session.registerFactory(lux::ui::PaneFactory{
        lux::ui::PaneTypeId{"test.pane"},
        "Duplicate",
        [](lux::ui::PaneCreateContext, lux::ui::PaneId)
            -> std::unique_ptr<lux::ui::Pane>
        {
            return {};
        }
    });
    assert(!duplicate_factory);
    auto created = session.createPane(
        lux::ui::PaneTypeIdView{"test.pane"},
        lux::ui::PaneId{"created"}
    );
    assert(created);

    int focus_changes = 0;
    bool last_focus = false;
    auto focus_connection = first.observe(
        lux::ui::Pane::focusChanged,
        [&](const lux::ui::PaneFocusChanged& change)
        {
            ++focus_changes;
            last_focus = change.focused;
        }
    );
    assert(focus_connection);

    int visibility_changes = 0;
    auto visibility_connection = first.observe(
        lux::ui::Pane::visibilityChanged,
        [&](const lux::ui::PaneVisibilityChanged&)
        {
            ++visibility_changes;
        }
    );
    assert(visibility_connection);
    first.setVisible(false);
    first.setVisible(true);
    assert(visibility_changes == 2);

    auto& router = session.commandRouter();
    auto contextual = router.bind<&TestPane::contextualDelete>(
        lux::ui::UiCommandId{"delete"},
        lux::ui::UiContextId{"test.selection"},
        first,
        [] { return false; }
    );
    assert(contextual);
    auto duplicate_binding = router.bind<&TestPane::contextualDelete>(
        lux::ui::UiCommandId{"delete"},
        lux::ui::UiContextId{"test.selection"},
        first
    );
    assert(!duplicate_binding);
    auto second_contextual = router.bind<&TestPane::contextualDelete>(
        lux::ui::UiCommandId{"delete"},
        lux::ui::UiContextId{"test.selection"},
        second
    );
    assert(second_contextual);
    auto global = router.bind<&TestPane::globalDelete>(
        lux::ui::UiCommandId{"delete"},
        lux::ui::UiContextId{"lux.ui.global"},
        first
    );
    assert(global);

    assert(session.focusPane(first.id().view()));
    assert(first.focused());
    assert(focus_changes == 1 && last_focus);
    assert(session.focusedContexts().size() == 2);
    assert(session.focusedContexts()[0]
        == lux::ui::UiContextIdView{"test.selection"});
    assert(router.state(lux::ui::UiCommandIdView{"delete"}).found);
    assert(!router.state(lux::ui::UiCommandIdView{"delete"}).enabled);
    assert(router.invoke(lux::ui::UiCommandIdView{"delete"})
        == lux::ui::ECommandDispatchResult::DISABLED);
    assert(first.contextual_count == 0);
    assert(first.global_count == 0);
    contextual->reset();
    assert(router.invoke(lux::ui::UiCommandIdView{"delete"})
        == lux::ui::ECommandDispatchResult::EXECUTED);
    assert(first.global_count == 1);

    assert(session.focusPane(second.id().view()));
    assert(!first.focused());
    assert(focus_changes == 2 && !last_focus);
    assert(router.invoke(lux::ui::UiCommandIdView{"delete"})
        == lux::ui::ECommandDispatchResult::EXECUTED);
    assert(second.contextual_count == 1);
    assert(first.global_count == 1);

    lux::ui::CommandRegistration dead_binding;
    {
        Receiver receiver;
        auto binding = router.bind<&Receiver::invoke>(
            lux::ui::UiCommandId{"temporary"},
            lux::ui::UiContextId{"lux.ui.global"},
            receiver
        );
        assert(binding);
        dead_binding = std::move(*binding);
    }
    assert(router.invoke(lux::ui::UiCommandIdView{"temporary"})
        == lux::ui::ECommandDispatchResult::NOT_FOUND);

    session.beginFrame({800.0F, 600.0F}, 1.0F / 60.0F);
    session.drawPanes();
    assert(session.endFrame() != nullptr);
    const auto layout = session.captureLayout();
    assert(!layout.bytes.empty());
    assert(session.restoreLayout(layout.bytes));

    bool posted = false;
    assert(session.post([&] { posted = true; })
        == lux::object::EPostStatus::POSTED);
    session.beginFrame({800.0F, 600.0F}, 1.0F / 60.0F);
    session.drawPanes();
    static_cast<void>(session.endFrame());
    assert(posted);

    lux::ui::MenuModel menu{{lux::ui::MenuItem{
        lux::ui::EMenuItemKind::COMMAND,
        "Delete",
        lux::ui::CommandPresentation{lux::ui::UiCommandId{"delete"}},
        {}
    }}};
    lux::ui::ToolbarModel toolbar{{lux::ui::ToolbarItem{
        lux::ui::EToolbarItemKind::COMMAND,
        lux::ui::CommandPresentation{lux::ui::UiCommandId{"delete"}}
    }}};
    assert(menu.items.front().presentation.command
        == toolbar.items.front().presentation.command);

    second_registration->reset();
    auto replacement_registration = session.registerPane(duplicate);
    assert(!replacement_registration);
    first_registration->reset();
    replacement_registration = session.registerPane(duplicate);
    assert(replacement_registration);
}
