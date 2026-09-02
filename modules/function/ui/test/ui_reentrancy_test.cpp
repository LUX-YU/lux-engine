#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include <lux/engine/ui/UI.hpp>

#include <lux/engine/ui/detail/CommandRouterDiagnostics.hpp>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace
{
    class ReentrantPane final : public lux::object::Object<ReentrantPane, lux::ui::Pane>
    {
    public:
        ReentrantPane(lux::object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id)
            : Object(std::move(dispatcher), std::move(id), lux::ui::PaneTypeId{"reentrant.pane"}, "Reentrant Pane")
        {
        }

        lux::ui::PaneRegistration* reset_during_draw{nullptr};
        lux::ui::UISession* registration_session{nullptr};
        ReentrantPane* pane_to_register{nullptr};
        lux::ui::PaneRegistration* late_registration{nullptr};
        bool hide_during_draw{false};
        int draw_count{0};

    protected:
        void draw(lux::ui::Frame& frame, lux::ui::PaneDrawContext&) override
        {
            ++draw_count;
            if (reset_during_draw)
            {
                auto* registration = std::exchange(reset_during_draw, nullptr);
                registration->reset();
            }
            if (registration_session && pane_to_register && late_registration)
            {
                auto result = registration_session->registerPane(*pane_to_register);
                assert(result);
                *late_registration = std::move(*result);
                registration_session = nullptr;
                pane_to_register = nullptr;
                late_registration = nullptr;
            }
            if (hide_during_draw)
            {
                hide_during_draw = false;
                setVisible(false);
            }
            frame.text("reentrant");
        }
    };

    void drawFrame(lux::ui::UISession& session)
    {
        auto frame = session.beginFrame({{640.0F, 360.0F}, 1.0F / 60.0F});
        frame.drawPanes();
        frame.finish();
    }

    class CommandReceiver final : public lux::object::Object<CommandReceiver>
    {
    public:
        void invoke() noexcept
        {
            ++invoke_count;
        }
        void noop() noexcept
        {
        }

        [[nodiscard]] bool enabledAndReset() const noexcept
        {
            registration_to_reset->reset();
            return true;
        }

        [[nodiscard]] bool checked() const noexcept
        {
            ++checked_count;
            return true;
        }

        [[nodiscard]] bool enabledAndReallocate() const noexcept
        {
            for (int index = 0; index < 32; ++index)
            {
                auto command = router->defineCommand(
                    {lux::ui::UiCommandId{"generated." + std::to_string(next_generated++)}, "Generated"}
                );
                assert(command);
                auto registration =
                    router->bindGlobal<&CommandReceiver::noop>(*command, const_cast<CommandReceiver&>(*this));
                assert(registration);
                generated_registrations.push_back(std::move(*registration));
            }
            registration_to_reset->reset();
            return true;
        }

        lux::ui::CommandRouter* router{nullptr};
        lux::ui::CommandRegistration* registration_to_reset{nullptr};
        mutable std::vector<lux::ui::CommandRegistration> generated_registrations;
        mutable int next_generated{0};
        int invoke_count{0};
        mutable int checked_count{0};
    };

    class SelfDestroyingReceiver final : public lux::object::Object<SelfDestroyingReceiver>
    {
    public:
        void invoke() noexcept
        {
            invoked = true;
        }

        [[nodiscard]] bool destroyDuringEnabled() const noexcept
        {
            auto* owner_value = owner;
            owner = nullptr;
            owner_value->reset();
            return true;
        }

        mutable std::unique_ptr<SelfDestroyingReceiver>* owner{nullptr};
        bool invoked{false};
    };
} // namespace

int
main()
{
    {
        lux::ui::UISession session;
        ReentrantPane first{session.dispatcherRef(), lux::ui::PaneId{"first"}};
        ReentrantPane second{session.dispatcherRef(), lux::ui::PaneId{"second"}};
        auto first_registration = session.registerPane(first);
        auto second_registration = session.registerPane(second);
        assert(first_registration && second_registration);
        first.reset_during_draw = std::addressof(*first_registration);
        drawFrame(session);
        assert(first.draw_count == 1);
        assert(second.draw_count == 1);
        drawFrame(session);
        assert(first.draw_count == 1);
        assert(second.draw_count == 2);
    }

    {
        lux::ui::UISession session;
        ReentrantPane driver{session.dispatcherRef(), lux::ui::PaneId{"driver"}};
        ReentrantPane late{session.dispatcherRef(), lux::ui::PaneId{"late"}};
        auto driver_registration = session.registerPane(driver);
        assert(driver_registration);
        lux::ui::PaneRegistration late_registration;
        driver.registration_session = &session;
        driver.pane_to_register = &late;
        driver.late_registration = &late_registration;
        drawFrame(session);
        assert(driver.draw_count == 1 && late.draw_count == 0);
        drawFrame(session);
        assert(late.draw_count == 1);
    }

    {
        lux::ui::UISession session;
        ReentrantPane pane{session.dispatcherRef(), lux::ui::PaneId{"visibility"}};
        auto registration = session.registerPane(pane);
        assert(registration);
        auto connection = pane.observeScoped<lux::ui::Pane::visibilityChanged>(
            [&](const lux::ui::PaneVisibilityChanged&) noexcept { registration->reset(); }
        );
        static_cast<void>(connection);
        pane.hide_during_draw = true;
        drawFrame(session);
        assert(pane.draw_count == 1);
        drawFrame(session);
        assert(pane.draw_count == 1);
    }

    {
        lux::ui::UISession session;
        ReentrantPane first{session.dispatcherRef(), lux::ui::PaneId{"focus.first"}};
        ReentrantPane second{session.dispatcherRef(), lux::ui::PaneId{"focus.second"}};
        auto first_registration = session.registerPane(first);
        auto second_registration = session.registerPane(second);
        assert(first_registration && second_registration);
        drawFrame(session);
        auto* target = session.focusedPane() == &first ? &second : &first;
        auto* target_registration =
            target == &first ? std::addressof(*first_registration) : std::addressof(*second_registration);
        bool focused_seen = false;
        auto connection =
            target->observeScoped<lux::ui::Pane::focusChanged>([&](const lux::ui::PaneFocusChanged& change) noexcept {
                if (change.focused)
                {
                    focused_seen = true;
                    target_registration->reset();
                }
            }
            );
        static_cast<void>(connection);
        assert(session.requestFocus(target->id().view()));
        for (int frame = 0; frame < 3 && !focused_seen; ++frame)
            drawFrame(session);
        assert(focused_seen);
        assert(session.focusedPane() == nullptr);
        assert(session.focusedContexts().size() == 1);
        assert(session.focusedContexts().front() == lux::ui::kGlobalContext);
    }

    {
        lux::ui::UISession session;
        lux::ui::PaneRegistration stale_registration;
        {
            auto pane = std::make_unique<ReentrantPane>(session.dispatcherRef(), lux::ui::PaneId{"replaceable"});
            auto registration = session.registerPane(*pane);
            assert(registration);
            stale_registration = std::move(*registration);
        }
        ReentrantPane replacement{session.dispatcherRef(), lux::ui::PaneId{"replaceable"}};
        auto replacement_registration = session.registerPane(replacement);
        assert(replacement_registration);
        drawFrame(session);
        assert(replacement.draw_count == 1);
        stale_registration.reset();
    }

    {
        lux::ui::UISession session;
        lux::ui::PaneFactoryRegistration first_factory;
        lux::ui::PaneFactoryRegistration replacement_factory;
        int first_calls = 0;
        int replacement_calls = 0;
        auto registered = session.registerFactory(lux::ui::PaneFactory{
            lux::ui::PaneTypeId{"replaceable.factory"},
            "First",
            [&](lux::object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id) -> std::unique_ptr<lux::ui::Pane> {
                ++first_calls;
                first_factory.reset();
                auto replacement = session.registerFactory(lux::ui::PaneFactory{
                    lux::ui::PaneTypeId{"replaceable.factory"},
                    "Replacement",
                    [&](lux::object::ObjectDispatcherRef next_dispatcher,
                        lux::ui::PaneId next_id) -> std::unique_ptr<lux::ui::Pane> {
                        ++replacement_calls;
                        return std::make_unique<ReentrantPane>(std::move(next_dispatcher), std::move(next_id));
                    }}
                );
                assert(replacement);
                replacement_factory = std::move(*replacement);
                return std::make_unique<ReentrantPane>(std::move(dispatcher), std::move(id));
            }}
        );
        assert(registered);
        first_factory = std::move(*registered);
        assert(session.createPane(lux::ui::PaneTypeIdView{"replaceable.factory"}, lux::ui::PaneId{"factory.first"}));
        assert(first_calls == 1 && replacement_calls == 0);
        assert(session.createPane(lux::ui::PaneTypeIdView{"replaceable.factory"}, lux::ui::PaneId{"factory.second"}));
        assert(first_calls == 1 && replacement_calls == 1);
    }

    {
        lux::ui::UISession session;
        auto pane = std::make_unique<ReentrantPane>(session.dispatcherRef(), lux::ui::PaneId{"deferred.destroy"});
        auto registration = session.registerPane(*pane);
        assert(registration);
        bool destroy_requested = false;
        auto visibility = pane->observeScoped<lux::ui::Pane::visibilityChanged>(
            [&](const lux::ui::PaneVisibilityChanged& change) noexcept { destroy_requested = !change.visible; }
        );
        pane->setVisible(false);
        assert(destroy_requested);
        pane.reset();
        assert(!visibility.connected());
        registration->reset();
    }

    {
        lux::ui::CommandRouter router;
        CommandReceiver receiver;
        receiver.router = &router;

        auto state_command = router.defineCommand({lux::ui::UiCommandId{"state.reentrant"}, "State"});
        assert(state_command);
        auto state_registration =
            router.bindGlobal<&CommandReceiver::invoke, &CommandReceiver::enabledAndReset, &CommandReceiver::checked>(
                *state_command,
                receiver
            );
        assert(state_registration);
        receiver.registration_to_reset = std::addressof(*state_registration);
        const auto state = router.state(*state_command);
        assert(state.found && state.enabled && state.checked);
        assert(receiver.checked_count == 1);

        auto invoke_command = router.defineCommand({lux::ui::UiCommandId{"invoke.reentrant"}, "Invoke"});
        assert(invoke_command);
        auto invoke_registration = router.bindGlobal<&CommandReceiver::invoke, &CommandReceiver::enabledAndReallocate>(
            *invoke_command,
            receiver
        );
        assert(invoke_registration);
        receiver.registration_to_reset = std::addressof(*invoke_registration);
        assert(router.invoke(*invoke_command) == lux::ui::ECommandDispatchResult::EXECUTED);
        assert(receiver.invoke_count == 1);

        std::array<lux::ui::UiContextIdView, 2> temporary_views;
        {
            std::string temporary_name = "temporary.context";
            temporary_views = {lux::ui::UiContextIdView{temporary_name}, lux::ui::kGlobalContext};
            lux::ui::detail::CommandRouterDiagnosticsAccess::updateRoute(router, nullptr, temporary_views);
        }
        assert(
            lux::ui::detail::CommandRouterDiagnosticsAccess::activeContexts(router).front().name() ==
            "temporary.context");
    }

    {
        lux::ui::CommandRouter router;
        auto command = router.defineCommand({lux::ui::UiCommandId{"destroy.receiver"}, "Destroy"});
        assert(command);
        auto receiver = std::make_unique<SelfDestroyingReceiver>();
        receiver->owner = &receiver;
        auto registration =
            router.bindGlobal<&SelfDestroyingReceiver::invoke, &SelfDestroyingReceiver::destroyDuringEnabled>(
                *command,
                *receiver
            );
        assert(registration);
        assert(router.invoke(*command) == lux::ui::ECommandDispatchResult::NOT_FOUND);
        assert(!receiver);
    }
}
