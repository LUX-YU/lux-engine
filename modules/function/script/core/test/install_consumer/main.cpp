#include <lux/engine/function/script/BoundScriptCall.hpp>
#include <lux/engine/function/script/ScriptCallFrame.hpp>
#include <lux/engine/function/script/ScriptSemantic.hpp>

int main()
{
    lux_script_call_frame raw{};
    lux::script::CallFrame frame(raw);
    lux::script::BoundScriptCall call{};
    return &frame.raw() == &raw && !call &&
            lux::script::makeScriptSemanticType<std::uint64_t>().type_id != 0U
        ? 0
        : 1;
}
