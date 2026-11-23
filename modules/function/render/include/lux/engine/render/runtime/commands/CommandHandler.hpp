#pragma once

#include <lux/cxx/execution/ManualContext.hpp>
#include <lux/engine/render/runtime/commands/CommandMethod.hpp>
#include <stdexec/execution.hpp>
#include <exception>

namespace lux::render
{
    namespace extension_detail
    {
        using CommandSenderEnvironment =
            stdexec::env<stdexec::prop<stdexec::get_scheduler_t, cxx::execution::ManualContext::Scheduler>>;

        template <class E>
        concept CommandSenderError = std::same_as<std::remove_cvref_t<E>, RenderError> ||
            std::same_as<std::remove_cvref_t<E>, cxx::execution::ScheduleError> ||
            std::same_as<std::remove_cvref_t<E>, std::exception_ptr>;

        template <class... Errors> using CommandSenderErrors = std::bool_constant<(CommandSenderError<Errors> && ...)>;

        template <class Sender, class Response>
        concept CommandSenderFor = stdexec::sender_in<Sender, CommandSenderEnvironment> &&
            std::same_as<stdexec::value_types_of_t<Sender, CommandSenderEnvironment, std::tuple, std::variant>,
                std::variant<std::tuple<Response>>> &&
            stdexec::error_types_of_t<Sender, CommandSenderEnvironment, CommandSenderErrors>::value;
    } // namespace extension_detail

    // Synchronous handlers fill the provided Response; asynchronous handlers consume Request
    // and produce exactly Response. Completion publication is the Server's responsibility.
    template <class C, class H, auto Method>
    concept AsyncCommandHandlerFor =
        CommandMethodFor<C, Method> &&
        std::same_as<H, typename extension_detail::CommandInvocation<C, Method>::Handler> &&
        (!extension_detail::CommandInvocation<C, Method>::synchronous) &&
        extension_detail::CommandSenderFor<
            typename extension_detail::CommandInvocation<C, Method>::Result, typename C::Response>;

} // namespace lux::render
