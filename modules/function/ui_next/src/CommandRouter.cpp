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
    } // namespace detail

    namespace
    {
        struct Binding final
        {
            std::uint64_t token{0};
            CommandIndex command;
            UiContextId context;
            lux::object::LuxObject* scope_identity{nullptr};
            lux::object::ObjectWeakRef scope;
            lux::object::ObjectWeakRef receiver;
            void (*invoke)(lux::object::LuxObject*){nullptr};
            bool (*enabled)(lux::object::LuxObject*){nullptr};
            bool (*checked)(lux::object::LuxObject*){nullptr};

            [[nodiscard]] bool endpointsAlive() const noexcept
            {
                return !receiver.expired() &&
                       (scope_identity == nullptr || !scope.expired());
            }
        };
    } // namespace

    struct CommandRouter::Impl final
    {
        std::vector<Command> commands;
        std::vector<Binding> bindings;
        std::vector<Binding*> effective;
        std::vector<UiContextIdView> active_contexts{kGlobalContext};
        lux::object::LuxObject* active_scope{nullptr};
        std::uint64_t next_token{1};

        [[nodiscard]] bool valid(CommandIndex index) const noexcept
        {
            return index.isValid() && index.value < commands.size();
        }

        void rebuild()
        {
            effective.assign(commands.size(), nullptr);
            for (std::size_t command = 0; command < commands.size(); ++command)
            {
                for (const auto context : active_contexts)
                {
                    const auto found = std::ranges::find_if(
                        bindings,
                        [this, command, context](const Binding& binding)
                        {
                            if (binding.command.value != command ||
                                binding.context.view() != context ||
                                !binding.endpointsAlive())
                            {
                                return false;
                            }
                            if (binding.scope_identity == nullptr)
                                return context == kGlobalContext;
                            return binding.scope_identity == active_scope;
                        }
                    );
                    if (found != bindings.end())
                    {
                        effective[command] = std::addressof(*found);
                        break;
                    }
                }
            }
        }

        [[nodiscard]] Binding* selected(CommandIndex index)
        {
            if (!valid(index))
                return nullptr;
            auto* binding = effective[index.value];
            if (binding && !binding->endpointsAlive())
            {
                rebuild();
                binding = effective[index.value];
            }
            return binding;
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
        : control_(std::move(other.control_)), token_(std::exchange(other.token_, 0))
    {
    }

    CommandRegistration& CommandRegistration::operator=(CommandRegistration&& other
    ) noexcept
    {
        if (this != std::addressof(other))
        {
            reset();
            control_ = std::move(other.control_);
            token_ = std::exchange(other.token_, 0);
        }
        return *this;
    }

    CommandRegistration::~CommandRegistration() { reset(); }

    void CommandRegistration::reset() noexcept
    {
        if (token_ == 0)
            return;
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

    CommandRouter::~CommandRouter() { control_->router = nullptr; }

    lux::cxx::expected<CommandIndex, ECommandDefinitionError>
    CommandRouter::defineCommand(Command command_value)
    {
        if (!command_value.id.isValid())
        {
            return lux::cxx::unexpected<ECommandDefinitionError>{
                ECommandDefinitionError::INVALID_ID
            };
        }
        if (findCommand(command_value.id.view()))
        {
            return lux::cxx::unexpected<ECommandDefinitionError>{
                ECommandDefinitionError::DUPLICATE_ID
            };
        }
        const auto index =
            CommandIndex{static_cast<std::uint32_t>(impl_->commands.size())};
        impl_->commands.push_back(std::move(command_value));
        impl_->rebuild();
        return index;
    }

    std::optional<CommandIndex> CommandRouter::findCommand(UiCommandIdView id
    ) const noexcept
    {
        const auto found = std::ranges::find_if(
            impl_->commands,
            [id](const Command& command_value) { return command_value.id.view() == id; }
        );
        if (found == impl_->commands.end())
            return std::nullopt;
        return CommandIndex{
            static_cast<std::uint32_t>(std::distance(impl_->commands.begin(), found))
        };
    }

    const Command* CommandRouter::command(CommandIndex index) const noexcept
    {
        return impl_->valid(index) ? std::addressof(impl_->commands[index.value])
                                   : nullptr;
    }

    lux::cxx::expected<CommandRegistration, ECommandBindingError>
    CommandRouter::bindErased(
        CommandIndex command_index,
        UiContextId context,
        lux::object::LuxObject* activation_scope,
        lux::object::LuxObject& receiver,
        InvokeThunk invoke,
        StateThunk enabled,
        StateThunk checked
    )
    {
        if (!impl_->valid(command_index))
        {
            return lux::cxx::unexpected<ECommandBindingError>{
                ECommandBindingError::INVALID_COMMAND
            };
        }
        if (!context.isValid() ||
            (activation_scope && context.view() == kGlobalContext))
        {
            return lux::cxx::unexpected<ECommandBindingError>{
                ECommandBindingError::INVALID_CONTEXT
            };
        }

        std::erase_if(
            impl_->bindings,
            [](const Binding& binding) { return !binding.endpointsAlive(); }
        );
        const auto duplicate = std::ranges::any_of(
            impl_->bindings,
            [&](const Binding& binding)
            {
                return binding.command == command_index && binding.context == context &&
                       binding.scope_identity == activation_scope;
            }
        );
        if (duplicate)
        {
            return lux::cxx::unexpected<ECommandBindingError>{
                ECommandBindingError::DUPLICATE_BINDING
            };
        }

        const auto token = impl_->next_token++;
        impl_->bindings.push_back(Binding{
            token,
            command_index,
            std::move(context),
            activation_scope,
            activation_scope ? activation_scope->weakRef()
                             : lux::object::ObjectWeakRef{},
            receiver.weakRef(),
            invoke,
            enabled,
            checked
        });
        impl_->rebuild();
        return CommandRegistration{control_, token};
    }

    CommandState CommandRouter::state(CommandIndex command_index) const
    {
        auto* binding = const_cast<Impl*>(impl_.get())->selected(command_index);
        if (!binding)
            return {};
        auto* receiver = binding->receiver.getOnCurrent();
        if (!receiver)
            return {};
        return {
            true,
            !binding->enabled || binding->enabled(receiver),
            binding->checked && binding->checked(receiver)
        };
    }

    ECommandDispatchResult CommandRouter::invoke(CommandIndex command_index)
    {
        auto* binding = impl_->selected(command_index);
        if (!binding)
            return ECommandDispatchResult::NOT_FOUND;
        auto* receiver = binding->receiver.getOnCurrent();
        if (!receiver)
            return ECommandDispatchResult::NOT_FOUND;
        if (binding->enabled && !binding->enabled(receiver))
            return ECommandDispatchResult::DISABLED;
        binding->invoke(receiver);
        return ECommandDispatchResult::EXECUTED;
    }

    void CommandRouter::setActiveContexts(std::span<const UiContextIdView> contexts)
    {
        impl_->active_contexts.assign(contexts.begin(), contexts.end());
        impl_->rebuild();
    }

    std::span<const UiContextIdView> CommandRouter::activeContexts() const noexcept
    {
        return impl_->active_contexts;
    }

    void CommandRouter::setActivationScope(lux::object::LuxObject* scope) noexcept
    {
        impl_->active_scope = scope;
        impl_->rebuild();
    }

    void CommandRouter::unbind(std::uint64_t token) noexcept
    {
        std::erase_if(
            impl_->bindings,
            [token](const Binding& binding) { return binding.token == token; }
        );
        impl_->rebuild();
    }
} // namespace lux::ui
