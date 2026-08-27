#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/ui/Command.hpp>

namespace lux::ui
{
    class UISession;

    namespace detail
    {
        struct CommandRouterControl;
#if defined(LUX_UI_TEST_DIAGNOSTICS)
        struct CommandRouterDiagnosticsAccess;
#endif
    } // namespace detail

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

    /** Live registration; reset and destruction belong to the router owner thread.
     */
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
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return token_ != 0;
        }

    private:
        friend class CommandRouter;

        CommandRegistration(std::weak_ptr<detail::CommandRouterControl> control, std::uint64_t token) noexcept;

        std::weak_ptr<detail::CommandRouterControl> control_;
        std::uint64_t token_{0};
    };

    /**
     * Thread-confined command state. A standalone router provides global
     * bindings only; UISession exclusively owns contextual route mutation.
     */
    class LUX_FUNCTION_PUBLIC CommandRouter final
    {
    public:
        CommandRouter();
        ~CommandRouter();
        CommandRouter(const CommandRouter&) = delete;
        CommandRouter& operator=(const CommandRouter&) = delete;

        [[nodiscard]] lux::cxx::expected<CommandHandle, ECommandDefinitionError> defineCommand(Command command);

        [[nodiscard]] std::optional<CommandHandle> findCommand(UiCommandIdView id) const noexcept;

        /** Borrowed until the next non-const CommandRouter operation. */
        [[nodiscard]] std::string_view label(CommandHandle command) const noexcept;

        template <auto Invoke, auto Enabled = nullptr, auto Checked = nullptr, class Receiver>
        [[nodiscard]] lux::cxx::expected<CommandRegistration, ECommandBindingError>
        bind(CommandHandle command, UiContextId context, lux::object::LuxObject& activation_scope, Receiver& receiver)
            requires std::derived_from<Receiver, lux::object::LuxObject>
        {
            static_assert(
                std::is_member_function_pointer_v<decltype(Invoke)>,
                "Command handlers must be receiver member functions");
            static_assert(
                std::is_nothrow_invocable_r_v<void, decltype(Invoke), Receiver&>,
                "Command handlers must be noexcept");
            if constexpr (!std::is_same_v<decltype(Enabled), std::nullptr_t>)
            {
                static_assert(
                    std::is_member_function_pointer_v<decltype(Enabled)>,
                    "Command state handlers must be receiver member functions");
                static_assert(
                    std::is_nothrow_invocable_r_v<bool, decltype(Enabled), const Receiver&>,
                    "Command enabled handlers must be noexcept");
            }
            if constexpr (!std::is_same_v<decltype(Checked), std::nullptr_t>)
            {
                static_assert(
                    std::is_member_function_pointer_v<decltype(Checked)>,
                    "Command state handlers must be receiver member functions");
                static_assert(
                    std::is_nothrow_invocable_r_v<bool, decltype(Checked), const Receiver&>,
                    "Command checked handlers must be noexcept");
            }

            return bindErased(
                command,
                std::move(context),
                std::addressof(activation_scope),
                receiver,
                [](lux::object::LuxObject* object) noexcept { std::invoke(Invoke, *static_cast<Receiver*>(object)); },
                makeStateThunk<Receiver, Enabled>(),
                makeStateThunk<Receiver, Checked>()
            );
        }

        template <auto Invoke, auto Enabled = nullptr, auto Checked = nullptr, class Receiver>
        [[nodiscard]] lux::cxx::expected<CommandRegistration, ECommandBindingError>
        bindGlobal(CommandHandle command, Receiver& receiver)
            requires std::derived_from<Receiver, lux::object::LuxObject>
        {
            static_assert(
                std::is_member_function_pointer_v<decltype(Invoke)>,
                "Command handlers must be receiver member functions");
            static_assert(
                std::is_nothrow_invocable_r_v<void, decltype(Invoke), Receiver&>,
                "Command handlers must be noexcept");
            if constexpr (!std::is_same_v<decltype(Enabled), std::nullptr_t>)
            {
                static_assert(
                    std::is_member_function_pointer_v<decltype(Enabled)>,
                    "Command state handlers must be receiver member functions");
                static_assert(
                    std::is_nothrow_invocable_r_v<bool, decltype(Enabled), const Receiver&>,
                    "Command enabled handlers must be noexcept");
            }
            if constexpr (!std::is_same_v<decltype(Checked), std::nullptr_t>)
            {
                static_assert(
                    std::is_member_function_pointer_v<decltype(Checked)>,
                    "Command state handlers must be receiver member functions");
                static_assert(
                    std::is_nothrow_invocable_r_v<bool, decltype(Checked), const Receiver&>,
                    "Command checked handlers must be noexcept");
            }

            return bindErased(
                command,
                UiContextId{kGlobalContext.name()},
                nullptr,
                receiver,
                [](lux::object::LuxObject* object) noexcept { std::invoke(Invoke, *static_cast<Receiver*>(object)); },
                makeStateThunk<Receiver, Enabled>(),
                makeStateThunk<Receiver, Checked>()
            );
        }

        [[nodiscard]] CommandState state(CommandHandle command) const;
        [[nodiscard]] ECommandDispatchResult invoke(CommandHandle command);

    private:
        using InvokeThunk = void (*)(lux::object::LuxObject*) noexcept;
        using StateThunk = bool (*)(const lux::object::LuxObject*) noexcept;

        template <class Receiver, auto Method> [[nodiscard]] static consteval StateThunk makeStateThunk()
        {
            if constexpr (std::is_same_v<decltype(Method), std::nullptr_t>)
            {
                return nullptr;
            }
            else
            {
                return [](const lux::object::LuxObject* object) noexcept {
                    return std::invoke(Method, *static_cast<const Receiver*>(object));
                };
            }
        }

        friend class CommandRegistration;
        friend class UISession;
#if defined(LUX_UI_TEST_DIAGNOSTICS)
        friend struct detail::CommandRouterDiagnosticsAccess;
#endif
        void updateRoute(lux::object::LuxObject* activation_scope, std::span<const UiContextIdView> contexts);
        [[nodiscard]] lux::cxx::expected<CommandRegistration, ECommandBindingError> bindErased(
            CommandHandle command,
            UiContextId context,
            lux::object::LuxObject* activation_scope,
            lux::object::LuxObject& receiver,
            InvokeThunk invoke,
            StateThunk enabled,
            StateThunk checked
        );
        void unbind(std::uint64_t token) noexcept;
#if defined(LUX_UI_TEST_DIAGNOSTICS)
        [[nodiscard]] std::uint64_t rebuildCountForTest() const noexcept;
        [[nodiscard]] std::uint64_t rebuildElapsedForTest() const noexcept;
        [[nodiscard]] std::uint64_t storageGrowthCountForTest() const noexcept;
        [[nodiscard]] std::span<const UiContextIdView> activeContextsForTest() const noexcept;
#endif

        struct Impl;
        std::unique_ptr<Impl> impl_;
        std::shared_ptr<detail::CommandRouterControl> control_;
    };
} // namespace lux::ui
