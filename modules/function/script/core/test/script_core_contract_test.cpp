#include <lux/engine/function/script/BoundScriptCall.hpp>
#include <lux/engine/function/script/ScriptCallFrame.hpp>
#include <lux/engine/function/script/ScriptSemantic.hpp>
#include <lux/engine/function/script/ScriptSignature.hpp>

#include <cassert>
#include <cstdint>
#include <type_traits>

int
main()
{
    static_assert(LUX_SCRIPT_ABI_VERSION == 2u);
    static_assert(!std::is_default_constructible_v<lux::script::CallFrame>);
    static_assert(
        lux::script::scriptSemanticTypeId("lux.i32") == lux::script::makeScriptSemanticType<std::int32_t>().type_id);
    static_assert(lux::script::makeScriptSemanticType<std::int32_t>().canonical_name == "lux.i32");

    const std::array parameters{lux::script::makeScriptSemanticType<std::int32_t>()};
    const lux::script::ScriptFunctionSignatureView signature{parameters, {}};
    assert(lux::script::sameScriptSignature(signature, signature));

    lux::script::BoundScriptCall unbound{};
    assert(!unbound);

    std::int32_t value = 42;
    lux_script_value_slot argument{};
    argument.kind = LUX_SCRIPT_VK_INT32;
    argument.size = sizeof(value);
    argument.data = &value;

    lux_script_call_frame raw{};
    raw.args = &argument;
    raw.arg_count = 1;

    lux::script::CallFrame frame(raw);
    assert(&frame.raw() == &raw);
    assert(frame.argCount() == 1);
    assert(frame.returnCount() == 0);
    assert(frame.arg(0).data == &value);
    return 0;
}
