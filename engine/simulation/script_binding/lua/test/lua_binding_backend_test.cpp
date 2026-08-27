#include <lux/engine/simulation/LuaScriptBindingBackend.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace
{
    lux_script_call_frame makeFrame(
        std::int32_t delta,
        std::int32_t expected,
        std::array<lux_script_value_slot, 2U>& arguments,
        std::array<std::int32_t, 2U>& values,
        lux_script_value_slot& return_slot,
        std::int32_t& result
    )
    {
        values = {delta, expected};
        for (std::size_t index{}; index < arguments.size(); ++index)
        {
            arguments[index] = lux_script_value_slot{
                LUX_SCRIPT_VK_INT32,
                {},
                sizeof(std::int32_t),
                lux::script::scriptSemanticTypeId("lux.i32"),
                &values[index]};
        }
        return_slot = lux_script_value_slot{
            LUX_SCRIPT_VK_INT32,
            {},
            sizeof(result),
            lux::script::scriptSemanticTypeId("lux.i32"),
            &result};
        return {
            arguments.data(),
            static_cast<std::uint32_t>(arguments.size()),
            0U,
            &return_slot,
            1U,
            0U,
            nullptr,
            nullptr};
    }
}

int main()
{
    using namespace lux::simulation;

    LuaScriptBindingBackend backend{4U};
    assert(backend);
    assert(backend.cachedTracebackCount() == 1U);

    lux::asset::ScriptAssetContent asset;
    asset.description.module_name = "lua.binding.fixture";
    asset.description.model = lux::rdesc::EScriptModel::ENTITY_BEHAVIOR;
    asset.description.body = lux::rdesc::LuaSourceScript{"fixture"};
    const lux::rdesc::ScriptValueType i32{
        "lux.i32",
        lux::script::scriptSemanticTypeId("lux.i32"),
        lux::script::EScriptPassMode::VALUE};
    const lux::rdesc::ScriptFunction function{
        "tick",
        11U,
        {i32, i32},
        {i32}};
    const lux::rdesc::ScriptFunction bad_return{
        "bad_return",
        13U,
        {},
        {i32}};
    asset.description.exports.push_back(function);
    asset.description.exports.push_back(bad_return);
    constexpr std::string_view source = R"lua(
        return {
            count = 0,
            tick = function(self, delta, expected)
                self.count = self.count + delta
                if self.count ~= expected then
                    error("instance state mismatch")
                end
                return self.count
            end,
            bad_return = function(self)
                return "not-an-integer"
            end
        }
    )lua";
    asset.payload.reserve(source.size());
    for (const auto value : source)
        asset.payload.push_back(static_cast<std::byte>(value));
    assert(lux::rdesc::validScriptDescription(asset.description));

    std::array<std::uint8_t, 16U> id_bytes{};
    id_bytes[0] = 7U;
    const auto id = lux::asset::AssetId{id_bytes};
    auto descriptor = backend.descriptor();
    ScriptBackendInstance first_instance;
    ScriptBackendInstance second_instance;
    assert(descriptor.createInstance(
        descriptor.context,
        ScriptInstanceCreateContext{id, ScriptMountId{1U}},
        asset,
        first_instance
    ) == EScriptBackendResult::SUCCESS);
    assert(descriptor.createInstance(
        descriptor.context,
        ScriptInstanceCreateContext{id, ScriptMountId{2U}},
        asset,
        second_instance
    ) == EScriptBackendResult::SUCCESS);

    lux::script::BoundScriptCall first;
    lux::script::BoundScriptCall second;
    assert(descriptor.prepareMethod(
        descriptor.context,
        first_instance,
        function,
        first
    ) == EScriptBackendResult::SUCCESS);
    assert(descriptor.prepareMethod(
        descriptor.context,
        second_instance,
        function,
        second
    ) == EScriptBackendResult::SUCCESS);
    assert(backend.loadedInstanceCount() == 2U);
    assert(backend.chunkLoadCount() == 1U);
    assert(backend.preparedReferenceCount() == 2U);

    lux::script::BoundScriptCall bad_return_call;
    assert(descriptor.prepareMethod(
        descriptor.context,
        first_instance,
        bad_return,
        bad_return_call
    ) == EScriptBackendResult::SUCCESS);
    std::int32_t bad_result{};
    lux_script_value_slot bad_result_slot{
        LUX_SCRIPT_VK_INT32,
        {},
        sizeof(bad_result),
        lux::script::scriptSemanticTypeId("lux.i32"),
        &bad_result};
    lux_script_call_frame bad_return_frame{
        nullptr, 0U, 0U, &bad_result_slot, 1U, 0U,
        nullptr, bad_return_call.context};
    assert(bad_return_call.invoke(&bad_return_frame) != 0);

    std::array<lux_script_value_slot, 2U> arguments{};
    std::array<std::int32_t, 2U> values{};
    lux_script_value_slot return_slot{};
    std::int32_t result{};
    auto frame = makeFrame(
        1,
        1,
        arguments,
        values,
        return_slot,
        result
    );
    frame.user_context = first.context;
    assert(first.invoke(&frame) == 0);
    assert(result == 1);
    frame = makeFrame(
        1,
        2,
        arguments,
        values,
        return_slot,
        result
    );
    frame.user_context = first.context;
    assert(first.invoke(&frame) == 0);
    assert(result == 2);
    frame = makeFrame(
        1,
        1,
        arguments,
        values,
        return_slot,
        result
    );
    frame.user_context = second.context;
    assert(second.invoke(&frame) == 0);
    assert(result == 1);

    frame = makeFrame(
        1,
        99,
        arguments,
        values,
        return_slot,
        result
    );
    frame.user_context = second.context;
    assert(second.invoke(&frame) != 0);

    auto unsupported = function;
    unsupported.symbol_id = 12U;
    unsupported.args = {{
        "lux.test.Record",
        lux::script::scriptSemanticTypeId("lux.test.Record"),
        lux::script::EScriptPassMode::CONST_REF}};
    unsupported.returns.clear();
    lux::script::BoundScriptCall rejected;
    assert(descriptor.prepareMethod(
        descriptor.context,
        first_instance,
        unsupported,
        rejected
    ) == EScriptBackendResult::UNSUPPORTED_MARSHAL_TYPE);

    auto u64 = function;
    u64.symbol_id = 14U;
    u64.args = {{
        "lux.u64",
        lux::script::scriptSemanticTypeId("lux.u64"),
        lux::script::EScriptPassMode::VALUE}};
    u64.returns.clear();
    assert(descriptor.prepareMethod(
        descriptor.context,
        first_instance,
        u64,
        rejected
    ) == EScriptBackendResult::UNSUPPORTED_MARSHAL_TYPE);

    descriptor.releaseMethod(
        descriptor.context,
        first_instance,
        bad_return_call
    );
    descriptor.releaseMethod(descriptor.context, first_instance, first);
    descriptor.releaseMethod(descriptor.context, second_instance, second);
    assert(backend.preparedReferenceCount() == 0U);
    descriptor.destroyInstance(descriptor.context, first_instance);
    descriptor.destroyInstance(descriptor.context, second_instance);
    assert(backend.loadedInstanceCount() == 0U);
    assert(backend.cachedTracebackCount() == 1U);
}
