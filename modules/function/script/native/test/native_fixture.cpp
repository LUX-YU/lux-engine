#include <lux/engine/function/script/abi/lux_script_abi.h>

#include <cstdint>

namespace
{
    int increment(lux_script_call_frame* frame)
    {
        if (!frame || !frame->user_context) return 7;
        ++*static_cast<std::int32_t*>(frame->user_context);
        return 0;
    }

    const lux_script_function_desc kFunctions[]{
        {
#if defined(LUX_SCRIPT_FIXTURE_BAD_ENTRY)
            nullptr,
#else
            "Increment",
#endif
            1,
            nullptr,
            0,
            nullptr,
            0,
            &increment
        }
    };

    const lux_script_module_desc kModule{
        "native_fixture",
#if defined(LUX_SCRIPT_FIXTURE_BAD_ABI)
        LUX_SCRIPT_ABI_VERSION + 1,
#else
        LUX_SCRIPT_ABI_VERSION,
#endif
        0,
        kFunctions,
        1,
        0
    };
}

#if defined(LUX_SCRIPT_FIXTURE_BIND_FAILURE)
extern "C" LUX_SCRIPT_EXPORT int lux_script_bind_host(
    lux_host_resolve_fn,
    void*,
    std::uint32_t
)
{
    return 1;
}
#endif

extern "C" LUX_SCRIPT_EXPORT const lux_script_module_desc*
lux_script_get_module()
{
    return &kModule;
}
