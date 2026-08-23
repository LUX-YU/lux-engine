#include <lux/engine/ui_next/CommandRouter.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace lux::ui
{
    namespace detail
    {
        struct CommandRouterControl final
        {
            CommandRouter* router{nullptr};
        };
    }

    namespace
    {
        struct Binding final
        {
            std::uint64_t token{0};
            UiCommandId command;
            UiContextId context;
            lux::object::LuxObject* receiver_identity{nullptr};
            lux::object::ObjectWeakRef receiver;
            lux::cxx::move_only_function<void()> invoke;
            lux::cxx::move_only_function<bool()> enabled;
            lux::cxx::move_only_function<bool()> checked;
        };
    }

    struct CommandRouter::Impl final
    {
        std::vector<Binding> bindings;
        std::vector<UiContextIdView> active_contexts{kGlobalContext};
        lux::object::LuxObject* focused_receiver{nullptr};
        std::uint64_t next_token{1};

        [[nodiscard]] Binding* find(UiCommandIdView command)
        {
            for (const auto context : active_contexts)
            {
                if (focused_receiver)
                {
                    const auto focused = std::ranges::find_if(
                        bindings,
                        [this, command, context](const Binding& binding)
                        {
                            return binding.command.view() == command
                                && binding.context.view() == context
                                && binding.receiver_identity == focused_receiver
                                && !binding.receiver.expired();
                        }
                    );
                    if (focused != bindings.end())
                        return std::addressof(*focused);
                }

                // A non-pane global handler remains available when the focused
                // instance has no override. Contextual handlers never leak from
                // one Pane instance into another merely because their Pane type
                // exposes the same Context identity.
                if (context != kGlobalContext) continue;
                const auto global = std::ranges::find_if(
                    bindings,
                    [command, context](const Binding& binding)
                    {
                        return binding.command.view() == command
                            && binding.context.view() == context
                            && !binding.receiver.expired();
                    }
                );
                if (global != bindings.end()) return std::addressof(*global);
            }
            return nullptr;
        }
    };

    CommandRegistration::CommandRegistration(
        std::weak_ptr<detail::CommandRouterControl> control,
        std::uint64_t token
    ) noexcept
        : control_(std::move(control)), token_(token)
    {
    }

    CommandRegistration::CommandRegistration(CommandRegistration&& other) noexcept
        : control_(std::move(other.control_)),
          token_(std::exchange(other.token_, 0))
    {
    }

    CommandRegistration& CommandRegistration::operator=(
        CommandRegistration&& other
    ) noexcept
    {
        if (this != &other)
        {
            reset();
            control_ = std::move(other.control_);
            token_ = std::exchange(other.token_, 0);
        }
        return *this;
    }

    CommandRegistration::~CommandRegistration()
    {
        reset();
    }

    void CommandRegistration::reset() noexcept
    {
        if (token_ == 0) return;
        if (const auto control = control_.lock(); control && control->router)
            control->router->unbind(token_);
        token_ = 0;
        control_.reset();
    }

    CommandRouter::CommandRouter()
        : impl_(std::make_unique<Impl>()),
          control_(std::make_shared<detail::CommandRouterControl>())
    {
        control_->router = this;
    }

    CommandRouter::~CommandRouter()
    {
        control_->router = nullptr;
    }

    lux::cxx::expected<CommandRegistration, ECommandBindingError>
    CommandRouter::bind(
        UiCommandId command,
        UiContextId context,
        lux::object::LuxObject& receiver,
        lux::cxx::move_only_function<void()> invoke,
        lux::cxx::move_only_function<bool()> enabled,
        lux::cxx::move_only_function<bool()> checked
    )
    {
        if (!command.isValid() || !context.isValid())
            return lux::cxx::unexpected(ECommandBindingError::INVALID_ID);
        if (std::ranges::any_of(impl_->bindings, [&](const Binding& binding)
        {
            return binding.command == command
                && binding.context == context
                && binding.receiver_identity == std::addressof(receiver);
        }))
        {
            return lux::cxx::unexpected(
                ECommandBindingError::DUPLICATE_BINDING
            );
        }

        const auto token = impl_->next_token++;
        impl_->bindings.push_back(Binding{
            token,
            std::move(command),
            std::move(context),
            std::addressof(receiver),
            receiver.weakRef(),
            std::move(invoke),
            std::move(enabled),
            std::move(checked)
        });
        return CommandRegistration{control_, token};
    }

    CommandState CommandRouter::state(UiCommandIdView command) const
    {
        auto* binding = impl_->find(command);
        if (!binding) return {};
        return {
            true,
            !binding->enabled || binding->enabled(),
            binding->checked && binding->checked()
        };
    }

    ECommandDispatchResult CommandRouter::invoke(UiCommandIdView command)
    {
        auto* binding = impl_->find(command);
        if (!binding) return ECommandDispatchResult::NOT_FOUND;
        if (binding->enabled && !binding->enabled())
            return ECommandDispatchResult::DISABLED;
        binding->invoke();
        return ECommandDispatchResult::EXECUTED;
    }

    void CommandRouter::setActiveContexts(
        std::span<const UiContextIdView> contexts
    )
    {
        impl_->active_contexts.assign(contexts.begin(), contexts.end());
    }

    std::span<const UiContextIdView> CommandRouter::activeContexts() const
        noexcept
    {
        return impl_->active_contexts;
    }

    void CommandRouter::setFocusedReceiver(
        lux::object::LuxObject* receiver
    ) noexcept
    {
        impl_->focused_receiver = receiver;
    }

    void CommandRouter::unbind(std::uint64_t token) noexcept
    {
        std::erase_if(
            impl_->bindings,
            [token](const Binding& binding) { return binding.token == token; }
        );
    }
}
