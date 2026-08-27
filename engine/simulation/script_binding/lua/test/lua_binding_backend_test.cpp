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
        std::array<lux_script_value_slot, 2U>& slots,
        std::array<std::int32_t, 2U>& values
    )
    {
        values = {delta, expected};
        for (std::size_t index{}; index < slots.size(); ++index)
        {
            slots[index] = lux_script_value_slot{
                LUX_SCRIPT_VK_INT32,
                {},
                sizeof(std::int32_t),
                lux::script::scriptSemanticTypeId("i32"),
                &values[index]};
        }
        return lux_script_call_frame{
            slots.data(),
            static_cast<std::uint32_t>(slots.size()),
            0U,
            nullptr,
            0U,
            0U,
            nullptr,
            nullptr};
    }
}

int main()
{
    lux::simulation::LuaScriptBindingBackend backend{4U};
    assert(backend);
    assert(backend.cachedTracebackCount() == 1U);

    lux::asset::ScriptAssetContent asset;
    asset.description.schema_version = lux::rdesc::Script::kSchemaVersion;
    asset.description.module_name = "lua.binding.fixture";
    asset.description.body = lux::rdesc::LuaSourceScript{"fixture"};
    const lux::rdesc::ScriptValueType i32{
        "i32",
        lux::script::scriptSemanticTypeId("i32"),
        lux::script::EScriptPassMode::VALUE};
    const lux::rdesc::ScriptFunction function{
        "tick",
        11U,
        {i32, i32},
        {}};
    asset.description.exports.push_back(function);
    constexpr std::string_view source = R"lua(
        local count = 0
        return {
            tick = function(delta, expected)
                count = count + delta
                if count ~= expected then error("instance state mismatch") end
                return 0
            end
        }
    )lua";
    asset.payload.reserve(source.size());
    for (const auto value : source)
        asset.payload.push_back(static_cast<std::byte>(value));

    std::array<std::uint8_t, 16U> id_bytes{};
    id_bytes[0] = 7U;
    const auto id = lux::asset::AssetId{id_bytes};
    const lux::simulation::ScriptPrepareContext first_key{
        id,
        lux::simulation::ecs::NullEntity,
        0U,
        0U};
    const lux::simulation::ScriptPrepareContext first_second_binding{
        id,
        lux::simulation::ecs::NullEntity,
        0U,
        1U};
    const lux::simulation::ScriptPrepareContext second_key{
        id,
        lux::simulation::ecs::NullEntity,
        1U,
        0U};

    auto descriptor = backend.descriptor();
    lux::script::BoundScriptCall first;
    lux::script::BoundScriptCall first_again;
    lux::script::BoundScriptCall second;
    assert(descriptor.prepare(
        descriptor.context, first_key, asset, function, first
    ) == lux::simulation::EScriptBackendPrepareResult::SUCCESS);
    assert(descriptor.prepare(
        descriptor.context,
        first_second_binding,
        asset,
        function,
        first_again
    ) == lux::simulation::EScriptBackendPrepareResult::SUCCESS);
    assert(descriptor.prepare(
        descriptor.context, second_key, asset, function, second
    ) == lux::simulation::EScriptBackendPrepareResult::SUCCESS);
    assert(backend.loadedInstanceCount() == 2U);
    assert(backend.chunkLoadCount() == 2U);
    assert(backend.preparedReferenceCount() == 3U);

    std::array<lux_script_value_slot, 2U> slots{};
    std::array<std::int32_t, 2U> values{};
    auto frame = makeFrame(1, 1, slots, values);
    frame.user_context = first.context;
    assert(first.invoke(&frame) == 0);
    frame = makeFrame(1, 2, slots, values);
    frame.user_context = first_again.context;
    assert(first_again.invoke(&frame) == 0);
    frame = makeFrame(1, 1, slots, values);
    frame.user_context = second.context;
    assert(second.invoke(&frame) == 0);

    frame = makeFrame(1, 99, slots, values);
    frame.user_context = second.context;
    assert(second.invoke(&frame) != 0);
    assert(backend.cachedTracebackCount() == 1U);

    descriptor.release(descriptor.context, first);
    descriptor.release(descriptor.context, first_again);
    descriptor.release(descriptor.context, second);
    assert(backend.preparedReferenceCount() == 0U);
    return 0;
}
