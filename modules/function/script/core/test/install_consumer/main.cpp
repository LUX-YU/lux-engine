#include <lux/engine/function/script/ScriptRuntime.hpp>

int main()
{
    lux::script::ScriptRuntime runtime;
    const auto result = runtime.registerBackend({});
    return !result
        && result.error().code == lux::script::EScriptError::INVALID_ARGUMENT
        ? 0
        : 1;
}
