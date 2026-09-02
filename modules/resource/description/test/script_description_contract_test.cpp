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
    description.api_requirements.push_back({
        script::ScriptApiContractId{"lux.test.Ability"},
        0x1234U
    });
    description.body = rdesc::NativeModuleScript{
        LUX_SCRIPT_ABI_VERSION,
        91U,
        4U,
        4U,
        {std::byte{1U}, std::byte{2U}}};
    assert(rdesc::validScriptDescription(description));

    auto lifecycle = description;
    lifecycle.lifecycle.begin_play = 7U;
    assert(rdesc::validScriptDescription(lifecycle));

    auto missing_lifecycle = description;
    missing_lifecycle.lifecycle.begin_play = 99U;
    assert(!rdesc::validScriptDescription(missing_lifecycle));

    auto duplicate_lifecycle = description;
    duplicate_lifecycle.lifecycle = {7U, 7U};
    assert(!rdesc::validScriptDescription(duplicate_lifecycle));

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

    auto invalid_requirement = description;
    invalid_requirement.api_requirements.front().expected_schema_hash = 0U;
    assert(!rdesc::validScriptDescription(invalid_requirement));

    auto duplicate_requirement = description;
    duplicate_requirement.api_requirements.push_back(duplicate_requirement.api_requirements.front());
    assert(!rdesc::validScriptDescription(duplicate_requirement));
    return 0;
}
