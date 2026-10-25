#pragma once

#include <lux/engine/core/execution/RunLoop.hpp>
#include <lux/engine/render/extensions/CommandMethod.hpp>
#include <stdexec/execution.hpp>

namespace lux::render
{
    // Synchronous handlers fill the provided Response; asynchronous handlers consume Request
    // and produce exactly Response. Completion publication is the Server's responsibility.
    template <class C, class H, auto Method>
    concept AsyncCommandHandlerFor =
        RenderCommand<C> && std::is_nothrow_invocable_v<decltype(Method), H&, typename C::Request&&> &&
        std::same_as<
            decltype(Method),
            std::invoke_result_t<decltype(Method), H&, typename C::Request&&> (H::*)(typename C::Request&&) noexcept> &&
        stdexec::sender<std::invoke_result_t<decltype(Method), H&, typename C::Request&&>> &&
        std::same_as<
            stdexec::value_types_of_t<
                std::invoke_result_t<decltype(Method), H&, typename C::Request&&>,
                stdexec::env<stdexec::prop<stdexec::get_scheduler_t, execution::RunLoop::Scheduler>>,
                std::tuple,
                std::variant>,
            std::variant<std::tuple<typename C::Response>>>;

} // namespace lux::render
