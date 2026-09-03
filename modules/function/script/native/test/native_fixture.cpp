#include <lux/engine/function/script/abi/lux_script_abi.h>
#include <lux/engine/core/semantic/SemanticType.hpp>

#include <cstdint>

namespace
{
    int increment(lux_script_call_frame* frame) noexcept
    {
        if (!frame || !frame->user_context)
            return 7;
        ++*static_cast<std::int32_t*>(frame->user_context);
        return 0;
    }

    int onUpdate(lux_script_call_frame* frame) noexcept
    {
        const bool is_missing_frame = frame == nullptr;
        const bool is_missing_context = !is_missing_frame && frame->user_context == nullptr;
        const bool is_wrong_argument_count = !is_missing_frame && frame->arg_count != 1;
        const bool is_missing_arguments = !is_missing_frame && frame->args == nullptr;
        const bool is_missing_argument_data = !is_missing_frame && !is_missing_arguments &&
            frame->args[0].data == nullptr;
        const bool is_invalid_frame = is_missing_frame || is_missing_context || is_wrong_argument_count ||
            is_missing_arguments || is_missing_argument_data;
        if (is_invalid_frame)
            return 8;
        *static_cast<float*>(frame->user_context) += *static_cast<const float*>(frame->args[0].data);
        return 0;
    }

    int onPair(lux_script_call_frame* frame) noexcept
    {
        const bool is_missing_frame = frame == nullptr;
        const bool is_missing_context = !is_missing_frame && frame->user_context == nullptr;
        const bool is_wrong_argument_count = !is_missing_frame && frame->arg_count != 2;
        const bool is_missing_arguments = !is_missing_frame && frame->args == nullptr;
        const bool is_missing_first_data = !is_missing_frame && !is_missing_arguments &&
            frame->args[0].data == nullptr;
        const bool is_missing_second_data = !is_missing_frame && !is_missing_arguments &&
            frame->args[1].data == nullptr;
        const bool is_invalid_frame = is_missing_frame || is_missing_context || is_wrong_argument_count ||
            is_missing_arguments || is_missing_first_data || is_missing_second_data;
        if (is_invalid_frame)
            return 9;
        *static_cast<float*>(frame->user_context) +=
            *static_cast<const float*>(frame->args[0].data) +
            static_cast<float>(*static_cast<const std::uint32_t*>(frame->args[1].data));
        return 0;
    }

    int admitToGameplay(lux_script_call_frame* frame) noexcept
    {
        if (!frame || !frame->user_context || frame->arg_count != 0U)
            return 10;
        *static_cast<float*>(frame->user_context) += 10.0F;
        return 0;
    }

    int leaveGameplay(lux_script_call_frame* frame) noexcept
    {
        const bool invalid_frame = !frame || !frame->user_context || frame->arg_count != 1U || !frame->args ||
            !frame->args[0].data;
        if (invalid_frame)
            return 11;
        const auto reason = *static_cast<const std::uint32_t*>(frame->args[0].data);
        return reason == 2U && *static_cast<const float*>(frame->user_context) >= 10.0F ? 0 : 12;
    }

    const lux_script_type_desc kFloatType{
        "lux.f32",
        lux::semantic::typeId("lux.f32"),
        sizeof(float),
        alignof(float),
        LUX_SCRIPT_VK_FLOAT,
        {}};

    const lux_script_type_desc kConstRefFloatType{
        "lux.f32",
        lux::semantic::typeId("lux.f32"),
        sizeof(float),
        alignof(float),
        LUX_SCRIPT_VK_FLOAT,
        LUX_SCRIPT_PASS_CONST_REF,
        {}};

    const lux_script_type_desc kUint32Type{
        "lux.u32",
        lux::semantic::typeId("lux.u32"),
        sizeof(std::uint32_t),
        alignof(std::uint32_t),
        LUX_SCRIPT_VK_UINT32,
        {}};

    const lux_script_type_desc kEndPlayReasonType{
        "lux.simulation.ScriptEndPlayReason",
        lux::semantic::typeId("lux.simulation.ScriptEndPlayReason"),
        sizeof(std::uint32_t),
        alignof(std::uint32_t),
        LUX_SCRIPT_VK_UINT32,
        {}};

    const lux_script_event_wait_import_desc kBadEventImport{
        0U,
        91U,
        17U,
        1U,
        0U,
        {},
        kUint32Type
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
#if defined(LUX_SCRIPT_FIXTURE_BAD_RETURN)
            &kConstRefFloatType,
            1,
#else
            nullptr,
            0,
#endif
            &increment},
        {"OnUpdate", 2, kUpdateArgs, 1, nullptr, 0, &onUpdate},
        {"OnUpdate", 3, kPairArgs, 2, nullptr, 0, &onPair},
        {"AdmitToGameplay", 4, nullptr, 0, nullptr, 0, &admitToGameplay},
        {"LeaveGameplay", 5, &kEndPlayReasonType, 1, nullptr, 0, &leaveGameplay}};

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
        5,
        0,
        nullptr,
        0U,
        0U,
#if defined(LUX_SCRIPT_FIXTURE_BAD_EVENT_IMPORT)
        &kBadEventImport,
        1U,
#else
        nullptr,
        0U,
#endif
        0U};
}

#if defined(LUX_SCRIPT_FIXTURE_BIND_FAILURE)
extern "C" LUX_SCRIPT_EXPORT int
lux_script_bind_host(lux_host_resolve_fn, void*, std::uint32_t)
{
    return 1;
}
#endif

extern "C" LUX_SCRIPT_EXPORT const lux_script_module_desc*
lux_script_get_module()
{
    return &kModule;
}
