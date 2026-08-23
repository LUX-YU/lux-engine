#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/ui_next/Command.hpp>

namespace lux::ui
{
    class UISession;

    namespace detail
    {
        struct CommandRouterControl;
    }

    enum class ECommandDefinitionError
    {
        INVALID_ID,
        DUPLICATE_ID
    };

    enum class ECommandBindingError
    {
        INVALID_COMMAND,
        INVALID_CONTEXT,
        DUPLICATE_BINDING
    };

    struct CommandState final
    {
        bool found{false};
        bool enabled{false};
        bool checked{false};
    };

    class LUX_FUNCTION_PUBLIC CommandRegistration final
    {
    public:
        CommandRegistration() noexcept = default;
        CommandRegistration(const CommandRegistration&) = delete;
        CommandRegistration& operator=(const CommandRegistration&) = delete;
        CommandRegistration(CommandRegistration&& other) noexcept;
        CommandRegistration& operator=(CommandRegistration&& other) noexcept;
        ~CommandRegistration();

        void reset() noexcept;
        [[nodiscard]] explicit operator bool() const noexcept { return token_ != 0; }

    private:
        friend class CommandRouter;
        CommandRegistration(
            std::weak_ptr<detail::CommandRouterControl> control,
            std::uint64_t token
        ) noexcept;

        std::weak_ptr<detail::CommandRouterControl> control_;
        std::uint64_t token_{0};
    };

    class LUX_FUNCTION_PUBLIC CommandRouter final
    {
    public:
        CommandRouter();
        ~CommandRouter();
        CommandRouter(const CommandRouter&) = delete;
        CommandRouter& operator=(const CommandRouter&) = delete;

        [[nodiscard]] lux::cxx::expected<CommandIndex, ECommandDefinitionError>
        defineCommand(Command command);
        [[nodiscard]] std::optional<CommandIndex> findCommand(UiCommandIdView id
        ) const noexcept;
        [[nodiscard]] const Command* command(CommandIndex index) const noexcept;

        template <
            auto Invoke,
            auto Enabled = nullptr,
            auto Checked = nullptr,
            class Receiver>
        [[nodiscard]] lux::cxx::expected<CommandRegistration, ECommandBindingError>
        bind(
            CommandIndex command_index,
            UiContextId context,
            lux::object::LuxObject& activation_scope,
            Receiver& receiver
        )
            requires std::derived_from<Receiver, lux::object::LuxObject>
        {
            static_assert(
                std::is_member_function_pointer_v<decltype(Invoke)>,
                "Command handlers must be receiver member functions"
            );
            static_assert(std::is_invocable_r_v<void, decltype(Invoke), Receiver&>);
            if constexpr (!std::is_same_v<decltype(Enabled), std::nullptr_t>)
            {
                static_assert(
                    std::is_member_function_pointer_v<decltype(Enabled)>,
                    "Command state handlers must be receiver member functions"
                );
                static_assert(std::
                                  is_invocable_r_v<bool, decltype(Enabled), Receiver&>);
            }
            if constexpr (!std::is_same_v<decltype(Checked), std::nullptr_t>)
            {
                static_assert(
                    std::is_member_function_pointer_v<decltype(Checked)>,
                    "Command state handlers must be receiver member functions"
                );
                static_assert(std::
                                  is_invocable_r_v<bool, decltype(Checked), Receiver&>);
            }

            return bindErased(
                command_index,
                std::move(context),
                std::addressof(activation_scope),
                receiver,
                [](lux::object::LuxObject* object)
                { std::invoke(Invoke, *static_cast<Receiver*>(object)); },
                makeStateThunk<Receiver, Enabled>(),
                makeStateThunk<Receiver, Checked>()
            );
        }

        template <
            auto Invoke,
            auto Enabled = nullptr,
            auto Checked = nullptr,
            class Receiver>
        [[nodiscard]] lux::cxx::expected<CommandRegistration, ECommandBindingError>
        bindGlobal(CommandIndex command_index, Receiver& receiver)
            requires std::derived_from<Receiver, lux::object::LuxObject>
        {
            static_assert(
                std::is_member_function_pointer_v<decltype(Invoke)>,
                "Command handlers must be receiver member functions"
            );
            static_assert(std::is_invocable_r_v<void, decltype(Invoke), Receiver&>);
            if constexpr (!std::is_same_v<decltype(Enabled), std::nullptr_t>)
            {
                static_assert(
                    std::is_member_function_pointer_v<decltype(Enabled)>,
                    "Command state handlers must be receiver member functions"
                );
                static_assert(std::
                                  is_invocable_r_v<bool, decltype(Enabled), Receiver&>);
            }
            if constexpr (!std::is_same_v<decltype(Checked), std::nullptr_t>)
            {
                static_assert(
                    std::is_member_function_pointer_v<decltype(Checked)>,
                    "Command state handlers must be receiver member functions"
                );
                static_assert(std::
                                  is_invocable_r_v<bool, decltype(Checked), Receiver&>);
            }

            return bindErased(
                command_index,
                UiContextId{kGlobalContext.name()},
                nullptr,
                receiver,
                [](lux::object::LuxObject* object)
                { std::invoke(Invoke, *static_cast<Receiver*>(object)); },
                makeStateThunk<Receiver, Enabled>(),
                makeStateThunk<Receiver, Checked>()
            );
        }

        [[nodiscard]] CommandState state(CommandIndex command) const;
        [[nodiscard]] ECommandDispatchResult invoke(CommandIndex command);

        void setActiveContexts(std::span<const UiContextIdView> contexts);
        [[nodiscard]] std::span<const UiContextIdView> activeContexts() const noexcept;

    private:
        using InvokeThunk = void (*)(lux::object::LuxObject*);
        using StateThunk = bool (*)(lux::object::LuxObject*);

        template <class Receiver, auto Method>
        [[nodiscard]] static consteval StateThunk makeStateThunk()
        {
            if constexpr (std::is_same_v<decltype(Method), std::nullptr_t>)
            {
                return nullptr;
            }
            else
            {
                return [](lux::object::LuxObject* object)
                { return std::invoke(Method, *static_cast<Receiver*>(object)); };
            }
        }

        friend class CommandRegistration;
        friend class UISession;
        [[nodiscard]] lux::cxx::expected<CommandRegistration, ECommandBindingError>
        bindErased(
            CommandIndex command,
            UiContextId context,
            lux::object::LuxObject* activation_scope,
            lux::object::LuxObject& receiver,
            InvokeThunk invoke,
            StateThunk enabled,
            StateThunk checked
        );
        void unbind(std::uint64_t token) noexcept;
        void setActivationScope(lux::object::LuxObject* scope) noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;
        std::shared_ptr<detail::CommandRouterControl> control_;
    };
} // namespace lux::ui
