#include <lux/engine/ui_next/CommandRouter.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
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
            void (*invoke)(lux::object::LuxObject*) noexcept{nullptr};
            bool (*enabled)(lux::object::LuxObject*) noexcept{nullptr};
            bool (*checked)(lux::object::LuxObject*) noexcept{nullptr};

            [[nodiscard]] bool endpointsAlive() const noexcept
            {
                return receiver.alive() &&
                       (scope_identity == nullptr || scope.alive());
            }
        };
    } // namespace

    struct CommandRouter::Impl final
    {
        std::vector<Command> commands;
        std::vector<Binding> bindings;
        std::vector<Binding*> effective;
        std::vector<UiContextId> active_context_ids{
            UiContextId{kGlobalContext.name()}
        };
        std::vector<UiContextIdView> active_contexts;
        std::vector<std::pair<UiContextIdView, std::size_t>> context_ranks;
        std::vector<std::size_t> selected_ranks;
        lux::object::LuxObject* active_scope_identity{nullptr};
        lux::object::ObjectWeakRef active_scope;
        std::uint64_t next_token{1};
#if defined(LUX_UI_NEXT_TEST_DIAGNOSTICS)
        std::uint64_t rebuild_count{0};
        std::uint64_t rebuild_elapsed_ns{0};
        std::uint64_t storage_growth_count{0};
#endif
        bool dirty{true};

        Impl() { rebuildContextViews(); }

        void rebuildContextViews()
        {
#if defined(LUX_UI_NEXT_TEST_DIAGNOSTICS)
            const auto view_capacity = active_contexts.capacity();
            const auto rank_capacity = context_ranks.capacity();
#endif
            active_contexts.clear();
            context_ranks.clear();
            active_contexts.reserve(active_context_ids.size());
            context_ranks.reserve(active_context_ids.size());
            for (std::size_t index = 0; index < active_context_ids.size(); ++index)
            {
                const auto view = active_context_ids[index].view();
                active_contexts.push_back(view);
                context_ranks.emplace_back(view, index);
            }
#if defined(LUX_UI_NEXT_TEST_DIAGNOSTICS)
            storage_growth_count += active_contexts.capacity() != view_capacity;
            storage_growth_count += context_ranks.capacity() != rank_capacity;
#endif
        }

        [[nodiscard]] bool sameContexts(
            std::span<const UiContextIdView> contexts
        ) const noexcept
        {
            if (active_context_ids.size() != contexts.size())
                return false;
            for (std::size_t index = 0; index < contexts.size(); ++index)
            {
                if (active_context_ids[index].view() != contexts[index])
                    return false;
            }
            return true;
        }

        void setContexts(std::span<const UiContextIdView> contexts)
        {
#if defined(LUX_UI_NEXT_TEST_DIAGNOSTICS)
            const auto id_capacity = active_context_ids.capacity();
#endif
            active_context_ids.clear();
            active_context_ids.reserve(contexts.size());
            for (const auto context : contexts)
                active_context_ids.emplace_back(context.name());
#if defined(LUX_UI_NEXT_TEST_DIAGNOSTICS)
            storage_growth_count += active_context_ids.capacity() != id_capacity;
#endif
            rebuildContextViews();
        }

        [[nodiscard]] bool valid(CommandIndex index) const noexcept
        {
            return index.isValid() && index.value < commands.size();
        }

        [[nodiscard]] std::size_t contextRank(UiContextIdView context) const noexcept
        {
            const auto found = std::ranges::find_if(
                context_ranks,
                [context](const auto& ranked) { return ranked.first == context; }
            );
            if (found == context_ranks.end())
                return (std::numeric_limits<std::size_t>::max)();
            return found->second;
        }

        void rebuild()
        {
            if (!dirty)
                return;
#if defined(LUX_UI_NEXT_TEST_DIAGNOSTICS)
            const auto begin = std::chrono::steady_clock::now();
#endif
            std::erase_if(
                bindings,
                [](const Binding& binding) { return !binding.endpointsAlive(); }
            );

#if defined(LUX_UI_NEXT_TEST_DIAGNOSTICS)
            const auto effective_capacity = effective.capacity();
            const auto rank_capacity = selected_ranks.capacity();
#endif
            effective.assign(commands.size(), nullptr);
            selected_ranks.assign(
                commands.size(),
                (std::numeric_limits<std::size_t>::max)()
            );
#if defined(LUX_UI_NEXT_TEST_DIAGNOSTICS)
            storage_growth_count += effective.capacity() != effective_capacity;
            storage_growth_count += selected_ranks.capacity() != rank_capacity;
#endif
            for (auto& binding : bindings)
            {
                if (binding.command.value >= commands.size())
                    continue;
                if (binding.scope_identity != nullptr &&
                    binding.scope_identity != active_scope_identity)
                {
                    continue;
                }
                const auto rank = contextRank(binding.context.view());
                if (rank == (std::numeric_limits<std::size_t>::max)() ||
                    rank >= selected_ranks[binding.command.value])
                {
                    continue;
                }
                selected_ranks[binding.command.value] = rank;
                effective[binding.command.value] = std::addressof(binding);
            }
            dirty = false;
#if defined(LUX_UI_NEXT_TEST_DIAGNOSTICS)
            ++rebuild_count;
            rebuild_elapsed_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - begin
                ).count()
            );
#endif
        }

        [[nodiscard]] Binding* selected(CommandIndex index)
        {
            if (!valid(index))
                return nullptr;
            if (active_scope_identity != nullptr && active_scope.expired())
            {
                active_scope_identity = nullptr;
                active_scope = {};
                dirty = true;
            }
            rebuild();
            auto* binding = effective[index.value];
            if (binding && !binding->endpointsAlive())
            {
                dirty = true;
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
#if defined(LUX_UI_NEXT_TEST_DIAGNOSTICS)
        const auto capacity = impl_->commands.capacity();
#endif
        impl_->commands.push_back(std::move(command_value));
#if defined(LUX_UI_NEXT_TEST_DIAGNOSTICS)
        impl_->storage_growth_count += impl_->commands.capacity() != capacity;
#endif
        impl_->dirty = true;
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
#if defined(LUX_UI_NEXT_TEST_DIAGNOSTICS)
        const auto capacity = impl_->bindings.capacity();
#endif
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
#if defined(LUX_UI_NEXT_TEST_DIAGNOSTICS)
        impl_->storage_growth_count += impl_->bindings.capacity() != capacity;
#endif
        impl_->dirty = true;
        return CommandRegistration{control_, token};
    }

    CommandState CommandRouter::state(CommandIndex command_index) const
    {
        auto* mutable_impl = const_cast<Impl*>(impl_.get());
        auto* binding = mutable_impl->selected(command_index);
        if (!binding)
            return {};

        const auto receiver_ref = binding->receiver;
        const auto enabled = binding->enabled;
        const auto checked = binding->checked;
        auto* receiver = receiver_ref.getOnCurrent();
        if (!receiver)
            return {};

        const bool enabled_value = !enabled || enabled(receiver);
        receiver = receiver_ref.getOnCurrent();
        if (!receiver)
        {
            mutable_impl->dirty = true;
            return {};
        }

        const bool checked_value = checked && checked(receiver);
        if (!receiver_ref.getOnCurrent())
        {
            mutable_impl->dirty = true;
            return {};
        }
        return {true, enabled_value, checked_value};
    }

    ECommandDispatchResult CommandRouter::invoke(CommandIndex command_index)
    {
        auto* binding = impl_->selected(command_index);
        if (!binding)
            return ECommandDispatchResult::NOT_FOUND;

        const auto receiver_ref = binding->receiver;
        const auto invoke = binding->invoke;
        const auto enabled = binding->enabled;
        auto* receiver = receiver_ref.getOnCurrent();
        if (!receiver)
            return ECommandDispatchResult::NOT_FOUND;
        const bool enabled_value = !enabled || enabled(receiver);
        receiver = receiver_ref.getOnCurrent();
        if (!receiver)
        {
            impl_->dirty = true;
            return ECommandDispatchResult::NOT_FOUND;
        }
        if (!enabled_value)
            return ECommandDispatchResult::DISABLED;
        invoke(receiver);
        return ECommandDispatchResult::EXECUTED;
    }

    void CommandRouter::updateRoute(
        lux::object::LuxObject* activation_scope,
        std::span<const UiContextIdView> contexts
    )
    {
        const bool same_contexts = impl_->sameContexts(contexts);
        const bool same_scope =
            impl_->active_scope_identity == activation_scope &&
            (activation_scope == nullptr || impl_->active_scope.alive());
        if (same_scope && same_contexts && !impl_->dirty)
            return;
        if (!same_contexts)
            impl_->setContexts(contexts);
        impl_->active_scope_identity = activation_scope;
        impl_->active_scope = activation_scope ? activation_scope->weakRef()
                                                : lux::object::ObjectWeakRef{};
        impl_->dirty = true;
        impl_->rebuild();
    }

    std::span<const UiContextIdView> CommandRouter::activeContexts() const noexcept
    {
        return impl_->active_contexts;
    }

    void CommandRouter::unbind(std::uint64_t token) noexcept
    {
        std::erase_if(
            impl_->bindings,
            [token](const Binding& binding) { return binding.token == token; }
        );
        impl_->dirty = true;
    }

#if defined(LUX_UI_NEXT_TEST_DIAGNOSTICS)
    std::uint64_t CommandRouter::rebuildCountForTest() const noexcept
    {
        return impl_->rebuild_count;
    }

    std::uint64_t CommandRouter::rebuildElapsedForTest() const noexcept
    {
        return impl_->rebuild_elapsed_ns;
    }

    std::uint64_t CommandRouter::storageGrowthCountForTest() const noexcept
    {
        return impl_->storage_growth_count;
    }
#endif
} // namespace lux::ui
