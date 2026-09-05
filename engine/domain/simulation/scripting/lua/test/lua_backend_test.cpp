#include "LuaUnsupportedIntegerAbility.hpp"
#include "LuaUnsupportedIntegerAbility.ability.generated.hpp"
#include "LuaUnsupportedIntegerAbility.ability.lua.generated.hpp"

#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>

#include <lua.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    inline constexpr std::array<lux::script::ScriptAbilityParameterDescription, 0U> kNoParameters{};
    inline constexpr std::array<lux::script::ScriptAbilityValueDescription, 0U> kNoResults{};
    inline constexpr std::array kNameMethods{
        lux::script::ScriptAbilityMethodDescription{
            lux::script::ScriptApiMethodIdView{"lux.test.lua_name.call"},
            "call",
            "Call",
            lux::script::EScriptApiMethodKind::QUERY,
            kNoParameters,
            kNoResults
        }
    };

    int noOpAbility(lua_State*) noexcept
    {
        return 0;
    }

    inline constexpr std::array kNameLuaMethods{
        lux::script::lua::ScriptAbilityLuaMethodProjection{
            lux::script::ScriptApiMethodIdView{"lux.test.lua_name.call"},
            &noOpAbility
        }
    };

    [[nodiscard]] constexpr lux::script::ScriptAbilityDescription nameDescription(
        std::string_view contract,
        std::string_view name,
        std::string_view display
    ) noexcept
    {
        const lux::script::ScriptApiContractIdView id{contract};
        return {
            id,
            name,
            display,
            1U,
            lux::script::scriptAbilitySchemaHash(
                id,
                lux::script::EScriptAbilityReceiverKind::NONE,
                kNameMethods
            ),
            lux::script::EScriptAbilityReceiverKind::NONE,
            kNameMethods
        };
    }

    struct CollisionEvent final
    {
        std::int32_t body{};
        float impulse{};
    };

    bool pushCollisionEvent(
        void*,
        void* opaque_state,
        const void* opaque_value
    ) noexcept
    {
        auto* state = static_cast<lua_State*>(opaque_state);
        const auto& value = *static_cast<const CollisionEvent*>(opaque_value);
        lua_createtable(state, 0, 2);
        lua_pushinteger(state, value.body);
        lua_setfield(state, -2, "body");
        lua_pushnumber(state, value.impulse);
        lua_setfield(state, -2, "impulse");
        return true;
    }

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
                lux::semantic::typeId("lux.i32"),
                &values[index]};
        }
        return_slot = lux_script_value_slot{
            LUX_SCRIPT_VK_INT32,
            {},
            sizeof(result),
            lux::semantic::typeId("lux.i32"),
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
    using namespace lux::simulation::script;

    const auto missing_capacity = LuaScriptBackend::create({});
    assert(!missing_capacity);
    assert(missing_capacity.error() == ELuaScriptBindingBackendError::INVALID_CAPACITY);
    const auto invalid_policy = LuaScriptBackend::create({
        .instance_capacity = 1U,
        .prepared_call_capacity = 1U,
        .continuation_capacity = 1U,
        .execution_depth_capacity = 1U,
        .ability_catalog_method_capacity = 1U,
        .execution_policy = static_cast<lux::script::lua::ELuaExecutionPolicy>(0xFFU)
    });
    assert(!invalid_policy);
    assert(invalid_policy.error() == ELuaScriptBindingBackendError::VM_CONFIGURATION_FAILURE);

    constexpr auto physics_name = nameDescription(
        "lux.test.lua_name.physics",
        "PhysicsQuery",
        "Shared Display"
    );
    constexpr auto inventory_name = nameDescription(
        "lux.test.lua_name.inventory",
        "Inventory",
        "Shared Display"
    );
    constexpr auto physics_display_changed = nameDescription(
        "lux.test.lua_name.physics",
        "PhysicsQuery",
        "3D Physics Query"
    );
    static_assert(physics_display_changed.schema_hash == physics_name.schema_hash);
    const std::array display_duplicates{
        lux::script::lua::ScriptAbilityLuaContribution{&physics_name, kNameLuaMethods},
        lux::script::lua::ScriptAbilityLuaContribution{&inventory_name, kNameLuaMethods}
    };
    const auto display_backend = LuaScriptBackend::create({
        .instance_capacity = 1U,
        .prepared_call_capacity = 1U,
        .continuation_capacity = 1U,
        .execution_depth_capacity = 4U,
        .ability_catalog_method_capacity = 2U,
        .prepared_ability_capacity = 2U,
        .abilities = display_duplicates,
        .prepared_ability_blocks = std::array{
            lux::simulation::script::LuaPreparedBlockClass{
                (2U) / ((1U) == 0U ? 1U : (1U)),
                1U
            }
        },
        .prepared_ability_storage_bytes =
            128U * (2U) + 4096U
    });
    assert(display_backend);

    constexpr auto duplicate_name = nameDescription(
        "lux.test.lua_name.duplicate",
        "PhysicsQuery",
        "Different Display"
    );
    const std::array duplicate_names{
        lux::script::lua::ScriptAbilityLuaContribution{&physics_name, kNameLuaMethods},
        lux::script::lua::ScriptAbilityLuaContribution{&duplicate_name, kNameLuaMethods}
    };
    const auto duplicate_name_backend = LuaScriptBackend::create({
        .instance_capacity = 1U,
        .prepared_call_capacity = 1U,
        .continuation_capacity = 1U,
        .execution_depth_capacity = 4U,
        .ability_catalog_method_capacity = 2U,
        .prepared_ability_capacity = 2U,
        .abilities = duplicate_names,
        .prepared_ability_blocks = std::array{
            lux::simulation::script::LuaPreparedBlockClass{
                (2U) / ((1U) == 0U ? 1U : (1U)),
                1U
            }
        },
        .prepared_ability_storage_bytes =
            128U * (2U) + 4096U
    });
    assert(!duplicate_name_backend);
    assert(duplicate_name_backend.error() == ELuaScriptBindingBackendError::DUPLICATE_ABILITY_NAME);

    constexpr auto reserved_name = nameDescription("lux.test.lua_name.reserved", "end", "End");
    const auto reserved_contribution = lux::script::lua::ScriptAbilityLuaContribution{
        &reserved_name,
        kNameLuaMethods
    };
    const auto reserved_name_backend = LuaScriptBackend::create({
        .instance_capacity = 1U,
        .prepared_call_capacity = 1U,
        .continuation_capacity = 1U,
        .execution_depth_capacity = 4U,
        .ability_catalog_method_capacity = 1U,
        .prepared_ability_capacity = 1U,
        .abilities = {&reserved_contribution, 1U},
        .prepared_ability_blocks = std::array{
            lux::simulation::script::LuaPreparedBlockClass{
                (1U) / ((1U) == 0U ? 1U : (1U)),
                1U
            }
        },
        .prepared_ability_storage_bytes =
            128U * (1U) + 4096U
    });
    assert(!reserved_name_backend);
    assert(reserved_name_backend.error() == ELuaScriptBindingBackendError::INVALID_ABILITY_CONTRIBUTION);

    const auto unsupported_integer = lux::script::lua::makeScriptAbilityLuaContribution<
        test::LuaUnsupportedIntegerAbility
    >();
    const auto unsupported_integer_backend = LuaScriptBackend::create({
        .instance_capacity = 1U,
        .prepared_call_capacity = 1U,
        .continuation_capacity = 1U,
        .execution_depth_capacity = 4U,
        .ability_catalog_method_capacity = 1U,
        .prepared_ability_capacity = 1U,
        .abilities = {&unsupported_integer, 1U},
        .prepared_ability_blocks = std::array{
            lux::simulation::script::LuaPreparedBlockClass{
                (1U) / ((1U) == 0U ? 1U : (1U)),
                1U
            }
        },
        .prepared_ability_storage_bytes =
            128U * (1U) + 4096U
    });
    assert(!unsupported_integer_backend);
    assert(unsupported_integer_backend.error() == ELuaScriptBindingBackendError::UNSUPPORTED_ABILITY_TYPE);

    const auto* i32_layout = lux::semantic::builtinLayout(
        lux::semantic::typeId("lux.i32")
    );
    assert(i32_layout);
    LuaComponentBinding health_binding{
        "health",
        0x4845414C5448ULL,
        i32_layout->type_id,
        std::string{i32_layout->canonical_name},
        i32_layout->abi_kind,
        i32_layout->size,
        i32_layout->alignment};
    const std::array duplicate_components{
        health_binding,
        health_binding};
    const auto duplicate_backend = LuaScriptBackend::create(
        {
            .instance_capacity = 1U,
            .prepared_call_capacity = 1U,
            .continuation_capacity = 1U,
            .execution_depth_capacity = 4U,
            .ability_catalog_method_capacity = 1U,
            .components = duplicate_components
        }
    );
    assert(!duplicate_backend);
    assert(
        duplicate_backend.error() ==
        ELuaScriptBindingBackendError::DUPLICATE_COMPONENT_NAME
    );
    auto invalid_binding = health_binding;
    ++invalid_binding.size;
    const auto invalid_backend = LuaScriptBackend::create(
        {
            .instance_capacity = 1U,
            .prepared_call_capacity = 1U,
            .continuation_capacity = 1U,
            .execution_depth_capacity = 4U,
            .ability_catalog_method_capacity = 1U,
            .components = std::span{&invalid_binding, 1U}
        }
    );
    assert(!invalid_backend);
    assert(
        invalid_backend.error() ==
        ELuaScriptBindingBackendError::INVALID_COMPONENT_CONTRACT
    );
    const auto* i64_layout = lux::semantic::builtinLayout(lux::semantic::typeId("lux.i64"));
    assert(i64_layout);
    const LuaComponentBinding i64_binding{
        "wide_integer",
        0x57494445494E54ULL,
        i64_layout->type_id,
        std::string{i64_layout->canonical_name},
        i64_layout->abi_kind,
        i64_layout->size,
        i64_layout->alignment
    };
    const auto i64_component_backend = LuaScriptBackend::create(
        {
            .instance_capacity = 1U,
            .prepared_call_capacity = 1U,
            .continuation_capacity = 1U,
            .execution_depth_capacity = 4U,
            .ability_catalog_method_capacity = 1U,
            .components = std::span{&i64_binding, 1U}
        }
    );
    assert(!i64_component_backend);
    assert(i64_component_backend.error() == ELuaScriptBindingBackendError::INVALID_COMPONENT_CONTRACT);
    auto contract_backend_result = LuaScriptBackend::create(
        {
            .instance_capacity = 1U,
            .prepared_call_capacity = 1U,
            .continuation_capacity = 1U,
            .execution_depth_capacity = 4U,
            .ability_catalog_method_capacity = 1U,
            .components = std::span{&health_binding, 1U}
        }
    );
    assert(contract_backend_result);
    auto contract_backend = std::move(*contract_backend_result);
    health_binding.name.clear();
    health_binding.canonical_name.clear();

    const LuaRecordMarshaller collision_marshaller{
        lux::semantic::typeId("lux.physics.CollisionEvent"),
        "lux.physics.CollisionEvent",
        sizeof(CollisionEvent),
        alignof(CollisionEvent),
        nullptr,
        &pushCollisionEvent};
    auto created_backend = LuaScriptBackend::create(
        {
            .instance_capacity = 4U,
            .prepared_call_capacity = 12U,
            .continuation_capacity = 4U,
            .execution_depth_capacity = 8U,
            .ability_catalog_method_capacity = 1U,
            .record_marshallers = std::span{&collision_marshaller, 1U}
        }
    );
    assert(created_backend);
    auto backend = std::move(*created_backend);
    assert(backend);

    lux::rdesc::Script description;
    description.module_name = "lua.binding.fixture";
    description.body = lux::rdesc::LuaSourceScript{"fixture"};
    const auto i32 = lux::rdesc::makeScriptValueType<std::int32_t>();
    const auto u32 = lux::rdesc::makeScriptValueType<std::uint32_t>();
    const auto f32 = lux::rdesc::makeScriptValueType<float>();
    const auto f64 = lux::rdesc::makeScriptValueType<double>();
    const auto boolean = lux::rdesc::makeScriptValueType<bool>();
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
    const lux::rdesc::ScriptFunction escape_host{
        "escape_host",
        15U,
        {},
        {}};
    const lux::rdesc::ScriptFunction probe_escaped_host{
        "probe_escaped_host",
        16U,
        {},
        {boolean}};
    const lux::rdesc::ScriptFunction on_collision{
        "on_collision",
        17U,
        {{
            "lux.physics.CollisionEvent",
            lux::semantic::typeId(
                "lux.physics.CollisionEvent"
            ),
            lux::semantic::EValuePass::CONST_REF,
            LUX_SCRIPT_VK_STRUCT_REF,
            sizeof(CollisionEvent),
            alignof(CollisionEvent)}},
        {}};
    const lux::rdesc::ScriptFunction collision_count{
        "collision_count",
        18U,
        {},
        {i32}};
    const lux::rdesc::ScriptFunction begin_lifecycle{
        "admit_to_gameplay",
        19U,
        {},
        {}};
    const lux::rdesc::ScriptFunction end_lifecycle{
        "leave_gameplay",
        20U,
        {lux::rdesc::makeScriptValueType<EScriptEndPlayReason>()},
        {}};
    const lux::rdesc::ScriptFunction portable_scalars{
        "portable_scalars",
        21U,
        {boolean, i32, u32, f32, f64},
        {boolean, i32, u32, f32, f64}};
    description.exports.push_back(function);
    description.exports.push_back(bad_return);
    description.exports.push_back(escape_host);
    description.exports.push_back(probe_escaped_host);
    description.exports.push_back(on_collision);
    description.exports.push_back(collision_count);
    description.exports.push_back(begin_lifecycle);
    description.exports.push_back(end_lifecycle);
    description.exports.push_back(portable_scalars);
    description.lifecycle = {begin_lifecycle.symbol_id, end_lifecycle.symbol_id};
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
            end,
            on_collision = function(self, event)
                if event.body ~= 42 or event.impulse ~= 3.5 then
                    error("structured event mismatch")
                end
                self.collisions = (self.collisions or 0) + 1
            end,
            collision_count = function(self)
                return self.collisions or 0
            end,
            admit_to_gameplay = function(self)
                if self.begun then
                    error("duplicate lifecycle admission")
                end
                self.begun = true
            end,
            leave_gameplay = function(self, reason)
                if not self.begun or self.count < 1 or reason ~= 2 then
                    error("lifecycle state mismatch")
                end
                self.ended = true
            end,
            portable_scalars = function(self, boolean_value, i32_value, u32_value, f32_value, f64_value)
                if boolean_value ~= false or i32_value ~= -2147483648 or u32_value ~= 4294967295 or
                    f32_value ~= -12.5 or f64_value ~= 1234.125 then
                    error("portable scalar input mismatch")
                end
                return boolean_value, i32_value, u32_value, f32_value, f64_value
            end
        }
    )lua";
    std::vector<std::byte> payload;
    payload.reserve(source.size());
    for (const auto value : source)
        payload.push_back(static_cast<std::byte>(value));
    auto asset_result = lux::script::ScriptArtifact::create(std::move(description), std::move(payload));
    assert(asset_result);
    auto asset = std::move(*asset_result);

    std::array<std::uint8_t, 16U> id_bytes{};
    id_bytes[0] = 7U;
    const auto id = lux::asset::AssetId{id_bytes};
    auto contract_descriptor = contract_backend.descriptor();
    ScriptBackendInstance rejected_contract_instance;
    assert(contract_descriptor.createInstance(
        contract_descriptor.context,
        ScriptInstanceCreateContext{id, SimulationScriptScope{}, nullptr},
        asset,
        rejected_contract_instance
    ) == EScriptBackendResult::HOST_COMPONENT_CONTRACT_MISMATCH);
    auto descriptor = backend.descriptor();
    ScriptBackendInstance first_instance;
    ScriptBackendInstance second_instance;
    ecs::Registry registry;
    const auto first_entity = registry.create();
    const auto second_entity = registry.create();
    ScriptBehavior first_behavior;
    ScriptBehavior second_behavior;
    assert(descriptor.createInstance(
        descriptor.context,
        ScriptInstanceCreateContext{
            id,
            EntityScriptScope{first_entity},
            &first_behavior},
        asset,
        first_instance
    ) == EScriptBackendResult::SUCCESS);
    assert(descriptor.createInstance(
        descriptor.context,
        ScriptInstanceCreateContext{
            id,
            EntityScriptScope{second_entity},
            &second_behavior},
        asset,
        second_instance
    ) == EScriptBackendResult::SUCCESS);

    ScriptBackendPreparedMethod first_begin;
    ScriptBackendPreparedMethod second_begin;
    ScriptBackendPreparedMethod first_end;
    ScriptBackendPreparedMethod second_end;
    assert(descriptor.prepareMethod(
        descriptor.context,
        first_instance,
        begin_lifecycle,
        first_begin
    ) == EScriptBackendResult::SUCCESS);
    assert(descriptor.prepareMethod(
        descriptor.context,
        second_instance,
        begin_lifecycle,
        second_begin
    ) == EScriptBackendResult::SUCCESS);
    assert(descriptor.prepareMethod(
        descriptor.context,
        first_instance,
        end_lifecycle,
        first_end
    ) == EScriptBackendResult::SUCCESS);
    assert(descriptor.prepareMethod(
        descriptor.context,
        second_instance,
        end_lifecycle,
        second_end
    ) == EScriptBackendResult::SUCCESS);
    lux_script_call_frame first_begin_frame{
        nullptr, 0U, 0U, nullptr, 0U, 0U, nullptr, first_begin.synchronous.context};
    lux_script_call_frame second_begin_frame{
        nullptr, 0U, 0U, nullptr, 0U, 0U, nullptr, second_begin.synchronous.context};
    assert(first_begin.synchronous.invoke(&first_begin_frame) == 0);
    assert(second_begin.synchronous.invoke(&second_begin_frame) == 0);

    ScriptBackendPreparedMethod scalar_call;
    assert(descriptor.prepareMethod(
        descriptor.context,
        first_instance,
        portable_scalars,
        scalar_call
    ) == EScriptBackendResult::SUCCESS);
    bool bool_input{};
    std::int32_t i32_input{(std::numeric_limits<std::int32_t>::min)()};
    std::uint32_t u32_input{(std::numeric_limits<std::uint32_t>::max)()};
    float f32_input{-12.5F};
    double f64_input{1234.125};
    std::array<lux_script_value_slot, 5U> scalar_arguments{
        lux_script_value_slot{LUX_SCRIPT_VK_BOOL, {}, sizeof(bool_input), boolean.type_id, &bool_input},
        lux_script_value_slot{LUX_SCRIPT_VK_INT32, {}, sizeof(i32_input), i32.type_id, &i32_input},
        lux_script_value_slot{LUX_SCRIPT_VK_UINT32, {}, sizeof(u32_input), u32.type_id, &u32_input},
        lux_script_value_slot{LUX_SCRIPT_VK_FLOAT, {}, sizeof(f32_input), f32.type_id, &f32_input},
        lux_script_value_slot{LUX_SCRIPT_VK_DOUBLE, {}, sizeof(f64_input), f64.type_id, &f64_input}
    };
    bool bool_output{true};
    std::int32_t i32_output{};
    std::uint32_t u32_output{};
    float f32_output{};
    double f64_output{};
    std::array<lux_script_value_slot, 5U> scalar_results{
        lux_script_value_slot{LUX_SCRIPT_VK_BOOL, {}, sizeof(bool_output), boolean.type_id, &bool_output},
        lux_script_value_slot{LUX_SCRIPT_VK_INT32, {}, sizeof(i32_output), i32.type_id, &i32_output},
        lux_script_value_slot{LUX_SCRIPT_VK_UINT32, {}, sizeof(u32_output), u32.type_id, &u32_output},
        lux_script_value_slot{LUX_SCRIPT_VK_FLOAT, {}, sizeof(f32_output), f32.type_id, &f32_output},
        lux_script_value_slot{LUX_SCRIPT_VK_DOUBLE, {}, sizeof(f64_output), f64.type_id, &f64_output}
    };
    lux_script_call_frame scalar_frame{
        scalar_arguments.data(),
        static_cast<std::uint32_t>(scalar_arguments.size()),
        0U,
        scalar_results.data(),
        static_cast<std::uint32_t>(scalar_results.size()),
        0U,
        nullptr,
        scalar_call.synchronous.context
    };
    assert(scalar_call.synchronous.invoke(&scalar_frame) == 0);
    assert(!bool_output);
    assert(i32_output == i32_input && u32_output == u32_input);
    assert(f32_output == f32_input && f64_output == f64_input);

    ScriptBackendPreparedMethod first;
    ScriptBackendPreparedMethod second;
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

    ScriptBackendPreparedMethod collision_call;
    ScriptBackendPreparedMethod collision_count_call;
    assert(descriptor.prepareMethod(
        descriptor.context,
        second_instance,
        on_collision,
        collision_call
    ) == EScriptBackendResult::SUCCESS);
    assert(descriptor.prepareMethod(
        descriptor.context,
        second_instance,
        collision_count,
        collision_count_call
    ) == EScriptBackendResult::SUCCESS);
    const CollisionEvent collision{42, 3.5F};
    lux_script_value_slot collision_slot{
        LUX_SCRIPT_VK_STRUCT_REF,
        {},
        sizeof(collision),
        lux::semantic::typeId("lux.physics.CollisionEvent"),
        const_cast<CollisionEvent*>(std::addressof(collision))};
    lux_script_call_frame collision_frame{
        &collision_slot,
        1U,
        0U,
        nullptr,
        0U,
        0U,
        nullptr,
        collision_call.synchronous.context};
    assert(collision_call.synchronous.invoke(&collision_frame) == 0);
    std::int32_t collision_count_value{};
    lux_script_value_slot collision_count_slot{
        LUX_SCRIPT_VK_INT32,
        {},
        sizeof(collision_count_value),
        lux::semantic::typeId("lux.i32"),
        &collision_count_value};
    lux_script_call_frame collision_count_frame{
        nullptr,
        0U,
        0U,
        &collision_count_slot,
        1U,
        0U,
        nullptr,
        collision_count_call.synchronous.context};
    assert(collision_count_call.synchronous.invoke(&collision_count_frame) == 0);
    assert(collision_count_value == 1);

    ScriptBackendPreparedMethod bad_return_call;
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
        lux::semantic::typeId("lux.i32"),
        &bad_result};
    lux_script_call_frame bad_return_frame{
        nullptr, 0U, 0U, &bad_result_slot, 1U, 0U,
        nullptr, bad_return_call.synchronous.context};
    assert(bad_return_call.synchronous.invoke(&bad_return_frame) != 0);

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
    frame.user_context = first.synchronous.context;
    assert(first.synchronous.invoke(&frame) == 0);
    assert(result == 1);
    frame = makeFrame(
        1,
        2,
        arguments,
        values,
        return_slot,
        result
    );
    frame.user_context = first.synchronous.context;
    assert(first.synchronous.invoke(&frame) == 0);
    assert(result == 2);
    frame = makeFrame(
        1,
        1,
        arguments,
        values,
        return_slot,
        result
    );
    frame.user_context = second.synchronous.context;
    assert(second.synchronous.invoke(&frame) == 0);
    assert(result == 1);

    frame = makeFrame(
        1,
        99,
        arguments,
        values,
        return_slot,
        result
    );
    frame.user_context = second.synchronous.context;
    assert(second.synchronous.invoke(&frame) != 0);

    const EScriptEndPlayReason end_reason{EScriptEndPlayReason::RUNTIME_STOPPED};
    lux_script_value_slot end_reason_slot{
        LUX_SCRIPT_VK_UINT32,
        {},
        sizeof(end_reason),
        lux::semantic::typeId("lux.simulation.ScriptEndPlayReason"),
        const_cast<EScriptEndPlayReason*>(std::addressof(end_reason))};
    lux_script_call_frame first_end_frame{
        &end_reason_slot, 1U, 0U, nullptr, 0U, 0U, nullptr, first_end.synchronous.context};
    lux_script_call_frame second_end_frame{
        &end_reason_slot, 1U, 0U, nullptr, 0U, 0U, nullptr, second_end.synchronous.context};
    assert(first_end.synchronous.invoke(&first_end_frame) == 0);
    assert(second_end.synchronous.invoke(&second_end_frame) == 0);

    auto unsupported = function;
    unsupported.symbol_id = 12U;
    unsupported.args = {{
        "lux.test.Record",
        lux::semantic::typeId("lux.test.Record"),
        lux::semantic::EValuePass::CONST_REF}};
    unsupported.returns.clear();
    ScriptBackendPreparedMethod rejected;
    assert(descriptor.prepareMethod(
        descriptor.context,
        first_instance,
        unsupported,
        rejected
    ) == EScriptBackendResult::UNSUPPORTED_MARSHAL_TYPE);

    auto u64 = function;
    u64.symbol_id = 14U;
    u64.args = {lux::rdesc::makeScriptValueType<std::uint64_t>()};
    u64.returns.clear();
    assert(descriptor.prepareMethod(
        descriptor.context,
        first_instance,
        u64,
        rejected
    ) == EScriptBackendResult::UNSUPPORTED_MARSHAL_TYPE);
    auto i64 = function;
    i64.symbol_id = 22U;
    i64.args = {lux::rdesc::makeScriptValueType<std::int64_t>()};
    i64.returns.clear();
    assert(descriptor.prepareMethod(
        descriptor.context,
        first_instance,
        i64,
        rejected
    ) == EScriptBackendResult::UNSUPPORTED_MARSHAL_TYPE);

    ScriptBackendPreparedMethod escape_call;
    ScriptBackendPreparedMethod probe_call;
    assert(descriptor.prepareMethod(
        descriptor.context,
        first_instance,
        escape_host,
        escape_call
    ) == EScriptBackendResult::SUCCESS);
    assert(descriptor.prepareMethod(
        descriptor.context,
        second_instance,
        probe_escaped_host,
        probe_call
    ) == EScriptBackendResult::SUCCESS);
    lux_script_call_frame escape_frame{
        nullptr, 0U, 0U, nullptr, 0U, 0U, nullptr, escape_call.synchronous.context};
    assert(escape_call.synchronous.invoke(&escape_frame) == 0);

    ScriptBackendPreparedMethod exhausted_call;
    assert(descriptor.prepareMethod(
        descriptor.context,
        first_instance,
        function,
        exhausted_call
    ) == EScriptBackendResult::CAPACITY_EXCEEDED);
    const auto recycled_call_context = bad_return_call.synchronous.context;
    descriptor.releaseMethod(
        descriptor.context,
        first_instance,
        bad_return_call
    );
    assert(descriptor.prepareMethod(
        descriptor.context,
        first_instance,
        function,
        exhausted_call
    ) == EScriptBackendResult::SUCCESS);
    assert(exhausted_call.synchronous.context == recycled_call_context);
    descriptor.releaseMethod(descriptor.context, first_instance, exhausted_call);
    descriptor.releaseMethod(descriptor.context, first_instance, first_end);
    descriptor.releaseMethod(descriptor.context, first_instance, first_begin);
    descriptor.releaseMethod(descriptor.context, first_instance, first);
    descriptor.releaseMethod(descriptor.context, first_instance, scalar_call);
    descriptor.releaseMethod(descriptor.context, first_instance, escape_call);
    descriptor.destroyInstance(descriptor.context, first_instance);

    bool escaped_is_dead{};
    lux_script_value_slot escaped_result_slot{
        LUX_SCRIPT_VK_BOOL,
        {},
        sizeof(escaped_is_dead),
        lux::semantic::typeId("lux.bool"),
        &escaped_is_dead};
    lux_script_call_frame probe_frame{
        nullptr,
        0U,
        0U,
        &escaped_result_slot,
        1U,
        0U,
        nullptr,
        probe_call.synchronous.context};
    assert(probe_call.synchronous.invoke(&probe_frame) == 0);
    assert(escaped_is_dead);

    descriptor.releaseMethod(descriptor.context, second_instance, second);
    descriptor.releaseMethod(descriptor.context, second_instance, second_end);
    descriptor.releaseMethod(descriptor.context, second_instance, second_begin);
    descriptor.releaseMethod(descriptor.context, second_instance, probe_call);
    descriptor.releaseMethod(
        descriptor.context,
        second_instance,
        collision_call
    );
    descriptor.releaseMethod(
        descriptor.context,
        second_instance,
        collision_count_call
    );
    descriptor.destroyInstance(descriptor.context, second_instance);
    ScriptBackendInstance recycled_instance;
    assert(descriptor.createInstance(
        descriptor.context,
        ScriptInstanceCreateContext{
            id,
            EntityScriptScope{second_entity},
            &second_behavior},
        asset,
        recycled_instance
    ) == EScriptBackendResult::SUCCESS);
    assert(recycled_instance.value == second_instance.value);
    descriptor.destroyInstance(descriptor.context, recycled_instance);
}
