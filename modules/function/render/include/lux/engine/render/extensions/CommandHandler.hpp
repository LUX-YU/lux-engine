#pragma once

#include <lux/cxx/execution/ManualContext.hpp>
#include <lux/engine/render/extensions/CommandMethod.hpp>
#include <stdexec/execution.hpp>

namespace lux::render
{
    // Synchronous handlers fill the provided Response; asynchronous handlers consume Request
    // and produce exactly Response. Completion publication is the Server's responsibility.
    template <class C, class H, auto Method>
    concept AsyncCommandHandlerFor =
        RenderCommand<C> && requires { typename extension_detail::CommandInvocation<C, Method>::Handler; } &&
        std::same_as<H, typename extension_detail::CommandInvocation<C, Method>::Handler> &&
        (!extension_detail::CommandInvocation<C, Method>::synchronous) &&
        stdexec::sender<typename extension_detail::CommandInvocation<C, Method>::Result> &&
        std::same_as<
            stdexec::value_types_of_t<
                typename extension_detail::CommandInvocation<C, Method>::Result,
                stdexec::env<stdexec::prop<stdexec::get_scheduler_t, cxx::execution::ManualContext::Scheduler>>,
                std::tuple,
                std::variant>,
            std::variant<std::tuple<typename C::Response>>>;

} // namespace lux::render
