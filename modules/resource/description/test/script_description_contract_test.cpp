#include <lux/engine/description/Script.hpp>
#include <lux/engine/function/script/abi/lux_script_abi.h>

#include <cassert>
#include <cstdint>

int main()
{
    using namespace lux;

    const auto i32 = script::makeScriptSemanticType<std::int32_t>();
    rdesc::Script description;
    description.module_name = "lux.test.script";
    description.exports.push_back({
        "tick",
        7U,
        {{std::string(i32.canonical_name), i32.type_id, i32.pass}},
        {}});
    description.default_bindings.push_back({
        7U,
        rdesc::EScriptBindingKind::HOOK,
        "lux.test.system",
        {},
        "before_step"});
    description.body = rdesc::NativeModuleScript{
        LUX_SCRIPT_ABI_VERSION,
        91U,
        4U,
        {std::byte{1U}, std::byte{2U}}};
    assert(rdesc::validScriptDescription(description));

    auto duplicate = description;
    duplicate.exports.push_back(duplicate.exports.front());
    assert(!rdesc::validScriptDescription(duplicate));

    auto missing_symbol = description;
    missing_symbol.default_bindings.front().function = 999U;
    assert(!rdesc::validScriptDescription(missing_symbol));

    auto invalid_type = description;
    invalid_type.exports.front().args.front().type_id ^= 1U;
    assert(!rdesc::validScriptDescription(invalid_type));
    return 0;
}
