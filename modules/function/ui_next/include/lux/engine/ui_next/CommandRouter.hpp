#pragma once

#include <cstdint>
#include <concepts>
#include <functional>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>
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

    enum class ECommandBindingError
    {
        INVALID_ID,
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
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return token_ != 0;
        }

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

        [[nodiscard]] lux::cxx::expected<
            CommandRegistration,
            ECommandBindingError>
        bind(
            UiCommandId command,
            UiContextId context,
            lux::object::LuxObject& receiver,
            lux::cxx::move_only_function<void()> invoke,
            lux::cxx::move_only_function<bool()> enabled = {},
            lux::cxx::move_only_function<bool()> checked = {}
        );

        template<auto Method, typename Receiver>
        [[nodiscard]] lux::cxx::expected<
            CommandRegistration,
            ECommandBindingError>
        bind(
            UiCommandId command,
            UiContextId context,
            Receiver& receiver,
            lux::cxx::move_only_function<bool()> enabled = {},
            lux::cxx::move_only_function<bool()> checked = {}
        )
        requires std::derived_from<Receiver, lux::object::LuxObject>
            && std::is_invocable_r_v<void, decltype(Method), Receiver&>
        {
            return bind(
                std::move(command),
                std::move(context),
                receiver,
                [&receiver] { std::invoke(Method, receiver); },
                std::move(enabled),
                std::move(checked)
            );
        }

        [[nodiscard]] CommandState state(UiCommandIdView command) const;
        [[nodiscard]] ECommandDispatchResult invoke(UiCommandIdView command);

        void setActiveContexts(std::span<const UiContextIdView> contexts);
        [[nodiscard]] std::span<const UiContextIdView> activeContexts() const
            noexcept;

      private:
        friend class CommandRegistration;
        friend class UISession;
        void unbind(std::uint64_t token) noexcept;
        void setFocusedReceiver(lux::object::LuxObject* receiver) noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;
        std::shared_ptr<detail::CommandRouterControl> control_;
    };
}
