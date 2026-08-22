#include <lux/engine/function/script/ScriptCallFrame.hpp>
#include <lux/engine/function/script/ScriptSignature.hpp>

#include <cassert>
#include <cstdint>
#include <type_traits>

int main()
{
    static_assert(LUX_SCRIPT_ABI_VERSION == 1u);
    static_assert(!std::is_default_constructible_v<lux::script::CallFrame>);

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
