#include <lux/engine/function/script/abi/lux_script_abi.h>

#include <cstdint>

namespace
{
    consteval std::uint64_t stableId(const char* text)
    {
        std::uint64_t value{14695981039346656037ULL};
        while (*text != '\0')
        {
            value ^= static_cast<std::uint8_t>(*text++);
            value *= 1099511628211ULL;
        }
        return value == 0U ? 1U : value;
    }

    struct StepFrame final
    {
        std::uint32_t pc{};
        std::uint32_t* state{};
    };

    int unavailableSync(lux_script_call_frame*) noexcept
    {
        return 91;
    }

    int readState(lux_script_call_frame* frame) noexcept
    {
        if (frame == nullptr || frame->user_context == nullptr || frame->return_count != 1U ||
            frame->returns == nullptr || frame->returns[0].data == nullptr)
        {
            return 96;
        }
        *static_cast<std::uint32_t*>(frame->returns[0].data) =
            *static_cast<const std::uint32_t*>(frame->user_context);
        return 0;
    }

    int startStep(
        lux_script_call_frame* call,
        const lux_script_step_host* host,
        void* frame_value,
        lux_script_step_outcome* outcome
    ) noexcept
    {
        if (call == nullptr || call->native_instance == nullptr || call->native_instance->state == nullptr ||
            host == nullptr || host->start_async == nullptr || frame_value == nullptr || outcome == nullptr)
        {
            return 92;
        }
        auto& frame = *static_cast<StepFrame*>(frame_value);
        frame.state = static_cast<std::uint32_t*>(call->native_instance->state);
        lux_script_async_token waiting{};
        const auto status = host->start_async(host->context, 0U, nullptr, 0U, &waiting);
        if (status != 0)
        {
            outcome->state = LUX_SCRIPT_STEP_FAILED;
            outcome->status = status;
            return 0;
        }
        frame.pc = 1U;
        outcome->state = LUX_SCRIPT_STEP_SUSPENDED;
        outcome->waiting_on = waiting;
        return 0;
    }

    int resumeStep(
        const lux_script_step_host*,
        void* frame_value,
        const lux_script_step_resume_packet* packet,
        lux_script_step_outcome* outcome
    ) noexcept
    {
        if (frame_value == nullptr || packet == nullptr || outcome == nullptr)
            return 93;
        auto& frame = *static_cast<StepFrame*>(frame_value);
        if (frame.pc != 1U || frame.state == nullptr)
            return 94;
        if (packet->state != LUX_SCRIPT_RESUME_READY)
        {
            outcome->state = LUX_SCRIPT_STEP_FAILED;
            outcome->status = packet->status == 0 ? 95 : packet->status;
            return 0;
        }
        ++*frame.state;
        frame.pc = 2U;
        outcome->state = LUX_SCRIPT_STEP_COMPLETED;
        return 0;
    }

    void destroyStep(void*) noexcept
    {
    }

    const lux_script_ability_import_desc kImports[]{
        {
            "lux.test.native_async",
            stableId("lux.test.native_async"),
            "lux.test.native_async.wait",
            stableId("lux.test.native_async.wait"),
            0xA55A55A5ULL,
            1U,
            2U,
            {},
            nullptr,
            0U,
            nullptr,
            0U
        }
    };

    const lux_script_step_desc kStep{
        sizeof(StepFrame),
        alignof(StepFrame),
        0x535445504652414DULL,
        &startStep,
        &resumeStep,
        &destroyStep
    };

    const lux_script_type_desc kUint32{
        "lux.u32",
        stableId("lux.u32"),
        sizeof(std::uint32_t),
        alignof(std::uint32_t),
        LUX_SCRIPT_VK_UINT32,
        LUX_SCRIPT_PASS_VALUE,
        {}
    };

    const lux_script_function_desc kFunctions[]{
        {"AsyncUpdate", 6U, nullptr, 0U, nullptr, 0U, &unavailableSync, &kStep},
        {"ReadState", 7U, nullptr, 0U, &kUint32, 1U, &readState, nullptr}
    };

    const lux_script_module_desc kModule{
        "native_step_fixture",
        LUX_SCRIPT_ABI_VERSION,
        0U,
        0x4C55585354455033ULL,
        sizeof(std::uint32_t),
        alignof(std::uint32_t),
        kFunctions,
        2U,
        0U,
        kImports,
        1U,
        0U
    };
}

extern "C" LUX_SCRIPT_EXPORT const lux_script_module_desc* lux_script_get_module()
{
    return &kModule;
}
