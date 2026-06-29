#pragma once
/**
 * @file ScriptCallFrame.hpp
 * @brief C++ wrapper around @ref lux_script_call_frame.
 */

#include <cstdint>
#include <span>

#include <lux/engine/script/ScriptValue.hpp>

namespace lux::script
{
    /**
     * @brief View over a single invocation frame.
     *
     * The frame is *non-owning*; the caller is responsible for keeping the
     * underlying argument / return storage alive for the duration of the call.
     */
    class CallFrame
    {
    public:
        CallFrame() = default;

        explicit CallFrame(lux_script_call_frame* raw) noexcept : raw_(raw) {}

        lux_script_call_frame*       raw()       noexcept { return raw_; }
        const lux_script_call_frame* raw() const noexcept { return raw_; }

        std::uint32_t argCount() const noexcept
        {
            return raw_ ? raw_->arg_count : 0u;
        }

        std::uint32_t returnCount() const noexcept
        {
            return raw_ ? raw_->return_count : 0u;
        }

        ConstValueView arg(std::uint32_t i) const noexcept
        {
            return make_const_view(raw_->args[i]);
        }

        ValueView ret(std::uint32_t i) noexcept
        {
            return make_view(raw_->returns[i]);
        }

        void* worldContext() const noexcept { return raw_ ? raw_->world_context : nullptr; }
        void* userContext()  const noexcept { return raw_ ? raw_->user_context  : nullptr; }

    private:
        lux_script_call_frame* raw_ = nullptr;
    };
}
