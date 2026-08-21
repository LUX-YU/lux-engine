#include <lux/engine/function/script/ScriptCallFrame.hpp>

int main()
{
    lux_script_call_frame raw{};
    lux::script::CallFrame frame(raw);
    return &frame.raw() == &raw ? 0 : 1;
}
