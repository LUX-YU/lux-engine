#include <lux/engine/function/script/abi/lux_script_abi.h>
#include <lux/engine/core/semantic/SemanticType.hpp>

#include <cstdint>
#include <new>

namespace
{
    struct Frame final { std::uint32_t* object{}; };

    int read(lux_script_call_frame* call) noexcept
    {
        const bool invalid = call == nullptr || call->user_context == nullptr || call->return_count != 1U ||
            call->returns == nullptr || call->returns[0].data == nullptr;
        if (invalid) return 1;
        *static_cast<std::uint32_t*>(call->returns[0].data) = *static_cast<std::uint32_t*>(call->user_context);
        return 0;
    }

    int synchronous(lux_script_call_frame*) noexcept { return 2; }

    int start(lux_script_call_frame* call, const lux_script_step_host*, void* storage,
              lux_script_step_outcome* outcome) noexcept
    {
        const bool invalid = call == nullptr || call->native_instance == nullptr ||
            call->native_instance->state == nullptr || storage == nullptr || outcome == nullptr;
        if (invalid) return 3;
        auto& frame = *new (storage) Frame{static_cast<std::uint32_t*>(call->native_instance->state)};
        ++*frame.object;
        outcome->state = LUX_SCRIPT_STEP_SUSPENDED;
        outcome->waiting_on = {1U, 1U};
        return 0;
    }

    int resume(const lux_script_step_host*, void*, const lux_script_step_resume_packet*,
               lux_script_step_outcome* outcome) noexcept
    {
        outcome->state = LUX_SCRIPT_STEP_COMPLETED;
        return 0;
    }

    void destroy(void* storage) noexcept
    {
        auto& frame = *static_cast<Frame*>(storage);
        ++*frame.object;
        frame.~Frame();
    }

    constexpr lux_script_type_desc Result{
        "lux.u32", lux::semantic::typeId("lux.u32"), 4U, 4U, LUX_SCRIPT_VK_UINT32, LUX_SCRIPT_PASS_VALUE, {}
    };
    constexpr lux_script_step_desc Small{128U, 16U, 128U, &start, &resume, &destroy};
    constexpr lux_script_step_desc Large{LUX_POPULATION_FRAME, 16U, 1024U, &start, &resume, &destroy};
    constexpr lux_script_function_desc Functions[]{
        {"Read", 1U, nullptr, 0U, &Result, 1U, &read, nullptr},
        {"Small", 2U, nullptr, 0U, nullptr, 0U, &synchronous, &Small},
        {"Large", 3U, nullptr, 0U, nullptr, 0U, &synchronous, &Large}
    };
    constexpr lux_script_module_desc Module{
        "population_fixture", LUX_SCRIPT_ABI_VERSION, 0U, 0x504F5055ULL,
        32U, LUX_POPULATION_ALIGN, Functions, LUX_POPULATION_FRAME == 0U ? 1U : 3U, 0U
    };
}

extern "C" LUX_SCRIPT_EXPORT const lux_script_module_desc* lux_script_get_module() { return &Module; }
