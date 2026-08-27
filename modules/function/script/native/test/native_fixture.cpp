#include <lux/engine/function/script/abi/lux_script_abi.h>
#include <lux/engine/function/script/ScriptSemantic.hpp>

#include <cstdint>

namespace
{
    int increment(lux_script_call_frame* frame) noexcept
    {
        if (!frame || !frame->user_context) return 7;
        ++*static_cast<std::int32_t*>(frame->user_context);
        return 0;
    }

    int onUpdate(lux_script_call_frame* frame) noexcept
    {
        if (!frame || !frame->user_context || frame->arg_count != 1
            || !frame->args || !frame->args[0].data)
            return 8;
        *static_cast<float*>(frame->user_context) +=
            *static_cast<const float*>(frame->args[0].data);
        return 0;
    }

    int onPair(lux_script_call_frame* frame) noexcept
    {
        if (!frame || !frame->user_context || frame->arg_count != 2
            || !frame->args || !frame->args[0].data || !frame->args[1].data)
            return 9;
        *static_cast<float*>(frame->user_context) +=
            *static_cast<const float*>(frame->args[0].data)
            + static_cast<float>(
                *static_cast<const std::uint32_t*>(frame->args[1].data)
            );
        return 0;
    }

    const lux_script_type_desc kFloatType{
        "lux.f32",
        lux::script::scriptSemanticTypeId("lux.f32"),
        sizeof(float),
        alignof(float),
        LUX_SCRIPT_VK_FLOAT,
        {}
    };

    const lux_script_type_desc kUint32Type{
        "lux.u32",
        lux::script::scriptSemanticTypeId("lux.u32"),
        sizeof(std::uint32_t),
        alignof(std::uint32_t),
        LUX_SCRIPT_VK_UINT32,
        {}
    };

    const lux_script_type_desc kUpdateArgs[]{kFloatType};
    const lux_script_type_desc kPairArgs[]{kFloatType, kUint32Type};

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
        },
        {
            "OnUpdate",
            2,
            kUpdateArgs,
            1,
            nullptr,
            0,
            &onUpdate
        },
        {
            "OnUpdate",
            3,
            kPairArgs,
            2,
            nullptr,
            0,
            &onPair
        }
    };

    const lux_script_module_desc kModule{
#if defined(LUX_SCRIPT_FIXTURE_SECOND_MODULE)
        "native_fixture_two",
#else
        "native_fixture",
#endif
#if defined(LUX_SCRIPT_FIXTURE_BAD_ABI)
        LUX_SCRIPT_ABI_VERSION - 1,
#else
        LUX_SCRIPT_ABI_VERSION,
#endif
        0,
#if defined(LUX_SCRIPT_FIXTURE_SECOND_MODULE)
        0x4C55584E41544957ULL,
#else
        0x4C55584E41544956ULL,
#endif
        64,
        64,
        kFunctions,
        3,
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
