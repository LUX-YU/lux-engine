#pragma once

#include <lux/engine/ecs/script/systems/ScriptBehavior.hpp>
#include <cstddef>
#include <span>

namespace lux::ecs::detail
{
    struct DirectDispatchResult final
    {
        std::size_t failed_index{0};
        int         result{0};
    };

    /// The successful per-event hot loop. Calls entering this function have
    /// already passed event validation and subscription filtering, therefore
    /// every entry has a non-null final ABI function pointer.
    #if defined(_MSC_VER)
        #define LUX_SCRIPT_FORCE_INLINE __forceinline
    #elif defined(__GNUC__) || defined(__clang__)
        #define LUX_SCRIPT_FORCE_INLINE inline __attribute__((always_inline))
    #else
        #define LUX_SCRIPT_FORCE_INLINE inline
    #endif

    [[nodiscard]] LUX_SCRIPT_FORCE_INLINE
    DirectDispatchResult dispatchBoundCalls(
        std::span<const BoundScriptCall> calls,
        lux_script_call_frame& frame,
        std::size_t begin = 0
    ) noexcept
    {
        const BoundScriptCall* current = calls.data() + begin;
        const BoundScriptCall* const end = calls.data() + calls.size();
        for (; current != end; ++current)
        {
            frame.user_context = current->context;
            const int result = current->invoke(&frame);
            if (result != 0) [[unlikely]]
            {
                return DirectDispatchResult{
                    static_cast<std::size_t>(current - calls.data()),
                    result
                };
            }
        }
        return DirectDispatchResult{calls.size(), 0};
    }

    #undef LUX_SCRIPT_FORCE_INLINE
}
