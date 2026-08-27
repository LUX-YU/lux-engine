#include <lux/engine/simulation/LuaScriptBindingBackend.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

int
main()
{
    using namespace lux::simulation;

    const auto* i32_layout = lux::script::scriptBuiltinLayout(lux::script::scriptSemanticTypeId("lux.i32"));
    assert(i32_layout);
    LuaComponentBinding health_binding{
        "health",
        0x4845414C5448ULL,
        i32_layout->type_id,
        std::string{i32_layout->canonical_name},
        i32_layout->abi_kind,
        i32_layout->size,
        i32_layout->alignment};
    const std::array duplicate_components{health_binding, health_binding};
    const auto duplicate_backend = LuaScriptBindingBackend::create(1U, duplicate_components);
    assert(!duplicate_backend);
    assert(duplicate_backend.error() == ELuaScriptBindingBackendError::DUPLICATE_COMPONENT_NAME);
    auto invalid_binding = health_binding;
    ++invalid_binding.size;
    const auto invalid_backend = LuaScriptBindingBackend::create(1U, std::span{&invalid_binding, 1U});
    assert(!invalid_backend);
    assert(invalid_backend.error() == ELuaScriptBindingBackendError::INVALID_COMPONENT_CONTRACT);
    auto contract_backend_result = LuaScriptBindingBackend::create(1U, std::span{&health_binding, 1U});
    assert(contract_backend_result);
    auto contract_backend = std::move(*contract_backend_result);
    health_binding.name.clear();
    health_binding.canonical_name.clear();

    auto created_backend = LuaScriptBindingBackend::create(4U);
    assert(created_backend);
    auto backend = std::move(*created_backend);
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
    const lux::rdesc::ScriptValueType boolean{
        "lux.bool",
        lux::script::scriptSemanticTypeId("lux.bool"),
        lux::script::EScriptPassMode::VALUE};
    const lux::rdesc::ScriptFunction function{"tick", 11U, {i32, i32}, {i32}};
    const lux::rdesc::ScriptFunction bad_return{"bad_return", 13U, {}, {i32}};
    const lux::rdesc::ScriptFunction escape_host{"escape_host", 15U, {}, {}};
    const lux::rdesc::ScriptFunction probe_escaped_host{"probe_escaped_host", 16U, {}, {boolean}};
    asset.description.exports.push_back(function);
    asset.description.exports.push_back(bad_return);
    asset.description.exports.push_back(escape_host);
    asset.description.exports.push_back(probe_escaped_host);
    constexpr std::string_view source = R"lua(
        local escaped_get = nil
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
            end,
            escape_host = function(self)
                escaped_get = self.get_component
            end,
            probe_escaped_host = function(self)
                return escaped_get(nil, "health") == nil
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
    auto contract_descriptor = contract_backend.descriptor();
    ScriptBackendInstance rejected_contract_instance;
    assert(
        contract_descriptor.createInstance(
            contract_descriptor.context,
            ScriptInstanceCreateContext{id, ScriptMountId{100U}},
            asset,
            rejected_contract_instance) == EScriptBackendResult::HOST_COMPONENT_CONTRACT_MISMATCH);
    assert(contract_backend.loadedInstanceCount() == 0U);
    auto descriptor = backend.descriptor();
    ScriptBackendInstance first_instance;
    ScriptBackendInstance second_instance;
    assert(
        descriptor.createInstance(
            descriptor.context,
            ScriptInstanceCreateContext{id, ScriptMountId{1U}},
            asset,
            first_instance) == EScriptBackendResult::SUCCESS);
    assert(
        descriptor.createInstance(
            descriptor.context,
            ScriptInstanceCreateContext{id, ScriptMountId{2U}},
            asset,
            second_instance) == EScriptBackendResult::SUCCESS);

    lux::script::BoundScriptCall first;
    lux::script::BoundScriptCall second;
    assert(
        descriptor.prepareMethod(descriptor.context, first_instance, function, first) == EScriptBackendResult::SUCCESS);
    assert(
        descriptor.prepareMethod(descriptor.context, second_instance, function, second) ==
        EScriptBackendResult::SUCCESS);
    assert(backend.loadedInstanceCount() == 2U);
    assert(backend.chunkLoadCount() == 1U);
    assert(backend.preparedReferenceCount() == 2U);

    lux::script::BoundScriptCall bad_return_call;
    assert(
        descriptor.prepareMethod(descriptor.context, first_instance, bad_return, bad_return_call) ==
        EScriptBackendResult::SUCCESS);
    std::int32_t bad_result{};
    lux_script_value_slot bad_result_slot{
        LUX_SCRIPT_VK_INT32,
        {},
        sizeof(bad_result),
        lux::script::scriptSemanticTypeId("lux.i32"),
        &bad_result};
    lux_script_call_frame bad_return_frame{nullptr, 0U, 0U, &bad_result_slot, 1U, 0U, nullptr, bad_return_call.context};
    assert(bad_return_call.invoke(&bad_return_frame) != 0);

    std::array<lux_script_value_slot, 2U> arguments{};
    std::array<std::int32_t, 2U> values{};
    lux_script_value_slot return_slot{};
    std::int32_t result{};
    auto frame = makeFrame(1, 1, arguments, values, return_slot, result);
    frame.user_context = first.context;
    assert(first.invoke(&frame) == 0);
    assert(result == 1);
    frame = makeFrame(1, 2, arguments, values, return_slot, result);
    frame.user_context = first.context;
    assert(first.invoke(&frame) == 0);
    assert(result == 2);
    frame = makeFrame(1, 1, arguments, values, return_slot, result);
    frame.user_context = second.context;
    assert(second.invoke(&frame) == 0);
    assert(result == 1);

    frame = makeFrame(1, 99, arguments, values, return_slot, result);
    frame.user_context = second.context;
    assert(second.invoke(&frame) != 0);

    auto unsupported = function;
    unsupported.symbol_id = 12U;
    unsupported.args = {
        {"lux.test.Record",
         lux::script::scriptSemanticTypeId("lux.test.Record"),
         lux::script::EScriptPassMode::CONST_REF}};
    unsupported.returns.clear();
    lux::script::BoundScriptCall rejected;
    assert(
        descriptor.prepareMethod(descriptor.context, first_instance, unsupported, rejected) ==
        EScriptBackendResult::UNSUPPORTED_MARSHAL_TYPE);

    auto u64 = function;
    u64.symbol_id = 14U;
    u64.args = {{"lux.u64", lux::script::scriptSemanticTypeId("lux.u64"), lux::script::EScriptPassMode::VALUE}};
    u64.returns.clear();
    assert(
        descriptor.prepareMethod(descriptor.context, first_instance, u64, rejected) ==
        EScriptBackendResult::UNSUPPORTED_MARSHAL_TYPE);

    lux::script::BoundScriptCall escape_call;
    lux::script::BoundScriptCall probe_call;
    assert(
        descriptor.prepareMethod(descriptor.context, first_instance, escape_host, escape_call) ==
        EScriptBackendResult::SUCCESS);
    assert(
        descriptor.prepareMethod(descriptor.context, second_instance, probe_escaped_host, probe_call) ==
        EScriptBackendResult::SUCCESS);
    lux_script_call_frame escape_frame{nullptr, 0U, 0U, nullptr, 0U, 0U, nullptr, escape_call.context};
    assert(escape_call.invoke(&escape_frame) == 0);

    descriptor.releaseMethod(descriptor.context, first_instance, bad_return_call);
    descriptor.releaseMethod(descriptor.context, first_instance, first);
    descriptor.releaseMethod(descriptor.context, first_instance, escape_call);
    descriptor.destroyInstance(descriptor.context, first_instance);

    bool escaped_is_dead{};
    lux_script_value_slot escaped_result_slot{
        LUX_SCRIPT_VK_BOOL,
        {},
        sizeof(escaped_is_dead),
        lux::script::scriptSemanticTypeId("lux.bool"),
        &escaped_is_dead};
    lux_script_call_frame probe_frame{nullptr, 0U, 0U, &escaped_result_slot, 1U, 0U, nullptr, probe_call.context};
    assert(probe_call.invoke(&probe_frame) == 0);
    assert(escaped_is_dead);

    descriptor.releaseMethod(descriptor.context, second_instance, second);
    descriptor.releaseMethod(descriptor.context, second_instance, probe_call);
    assert(backend.preparedReferenceCount() == 0U);
    descriptor.destroyInstance(descriptor.context, second_instance);
    assert(backend.loadedInstanceCount() == 0U);
    assert(backend.cachedTracebackCount() == 1U);
}
