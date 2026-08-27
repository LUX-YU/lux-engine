#pragma once

#include <lux/engine/function/script/abi/lux_script_abi.h>

#include <type_traits>

namespace lux::script
{
    struct BoundScriptCall final
    {
        lux_script_invoke_fn invoke{};
        void* context{};

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return invoke != nullptr;
        }
    };

    static_assert(sizeof(BoundScriptCall) == 2U * sizeof(void*));
    static_assert(std::is_trivially_copyable_v<BoundScriptCall>);
}
