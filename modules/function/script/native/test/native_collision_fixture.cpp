#include <lux/engine/function/script/ScriptSemantic.hpp>
#include <lux/engine/function/script/abi/lux_script_abi.h>

#include <cstdint>
#include <string_view>

namespace
{
    struct CollisionEvent final
    {
        std::int32_t body{};
        float impulse{};
    };

    int onCollision(lux_script_call_frame* frame) noexcept
    {
        const bool invalid_frame = !frame || !frame->user_context ||
            frame->arg_count != 1U || !frame->args ||
            !frame->args[0].data;
        if (invalid_frame)
            return 1;
        const auto& collision = *static_cast<const CollisionEvent*>(
            frame->args[0].data
        );
        if (collision.body != 42 || collision.impulse != 3.5F)
            return 2;
        ++*static_cast<std::uint32_t*>(frame->user_context);
        return 0;
    }

    constexpr std::string_view kCollisionName{
        "lux.physics.CollisionEvent"};
    const lux_script_type_desc kCollisionType{
        kCollisionName.data(),
        lux::script::scriptSemanticTypeId(kCollisionName),
        sizeof(CollisionEvent),
        alignof(CollisionEvent),
        LUX_SCRIPT_VK_STRUCT_REF,
        static_cast<std::uint8_t>(
            lux::script::EScriptPassMode::CONST_REF
        ),
        {}};
    const lux_script_function_desc kFunctions[]{
        {
            "on_collision",
            0xC0111510U,
            &kCollisionType,
            1U,
            nullptr,
            0U,
            &onCollision}};
    const lux_script_module_desc kModule{
        "native_collision_fixture",
        LUX_SCRIPT_ABI_VERSION,
        0U,
        0xC0111510C0111510ULL,
        64U,
        64U,
        kFunctions,
        1U,
        0U};
}

extern "C" LUX_SCRIPT_EXPORT const lux_script_module_desc*
lux_script_get_module()
{
    return &kModule;
}
