#include <lux/engine/description/Script.hpp>
#include <lux/engine/function/script/abi/lux_script_abi.h>

#include <cassert>
#include <cstdint>

int main()
{
    using namespace lux;

    rdesc::Script description;
    description.module_name = "lux.test.script";
    description.exports.push_back({
        "tick",
        7U,
        {rdesc::makeScriptValueType<std::int32_t>()},
        {}});
    description.body = rdesc::NativeModuleScript{
        LUX_SCRIPT_ABI_VERSION,
        91U,
        4U,
        4U,
        {std::byte{1U}, std::byte{2U}}};
    assert(rdesc::validScriptDescription(description));

    auto duplicate = description;
    duplicate.exports.push_back(duplicate.exports.front());
    assert(!rdesc::validScriptDescription(duplicate));

    auto overloaded = description;
    overloaded.exports.push_back(description.exports.front());
    overloaded.exports.back().symbol_id = 8U;
    assert(rdesc::validScriptDescription(overloaded));

    auto invalid_type = description;
    invalid_type.exports.front().args.front().type_id ^= 1U;
    assert(!rdesc::validScriptDescription(invalid_type));

    auto invalid_align = description;
    std::get<rdesc::NativeModuleScript>(invalid_align.body).state_align = 3U;
    assert(!rdesc::validScriptDescription(invalid_align));

    auto invalid_layout = description;
    invalid_layout.exports.front().args.front().alignment = 3U;
    assert(!rdesc::validScriptDescription(invalid_layout));
    return 0;
}
