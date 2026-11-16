#pragma once

#include <exception>
#include <lux/cxx/execution/ManualContext.hpp>
#include <lux/engine/render/core/Error.hpp>

namespace lux::render::detail
{
    // Normalize BEFORE the Server handoff. A rejected native start never ran on the worker.
    template <class T> struct NativeScheduleFailure final
    {
        [[nodiscard]] RenderResult<T> operator()(cxx::execution::ScheduleError) const noexcept
        {
            return cxx::unexpected(RenderError{kNativeExecutionClosed});
        }

        [[noreturn]] RenderResult<T> operator()(std::exception_ptr) const noexcept
        {
            std::terminate();
        }
    };

    // Required delivery was rejected: the host closed its return Context too early.
    // Never repair business state from the starting/native thread in this case.
    template <class T> struct NativeDeliveryFailure final
    {
        template <class Error> [[noreturn]] RenderResult<T> operator()(Error&&) const noexcept
        {
            std::terminate();
        }
    };
} // namespace lux::render::detail
