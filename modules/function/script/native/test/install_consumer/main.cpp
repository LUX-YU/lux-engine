#include <lux/engine/function/script/native/NativeModule.hpp>

#include <cstdint>
#include <filesystem>

int
main()
{
    auto loaded = lux::script::loadNativeModule(std::filesystem::path{LUX_SCRIPT_NATIVE_FIXTURE});
    if (!loaded)
        return 1;
    const auto* increment = loaded.value().findFunction("Increment");
    if (!increment || !increment->invoke)
        return 2;

    std::int32_t value = 41;
    lux_script_call_frame frame{};
    frame.user_context = &value;
    if (increment->invoke(&frame) != 0 || value != 42)
        return 3;
    return 0;
}
