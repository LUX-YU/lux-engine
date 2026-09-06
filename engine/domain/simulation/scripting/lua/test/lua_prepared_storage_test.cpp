#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr std::size_t AbilityCount{128U};
    constexpr std::size_t MethodsPerAbility{2U};

    lux::script::ScriptAbilityErasedCallResult invoke(
        void*,
        const void*,
        std::span<const lux::script::ScriptAbilityInputSlot>,
        std::span<lux::script::ScriptAbilityOutputSlot>
    ) noexcept
    {
        return {};
    }

    int project(lua_State*) noexcept
    {
        return 0;
    }

    [[nodiscard]] lux::script::ScriptArtifact artifact(
        const lux::script::ScriptAbilityDescription& ability,
        const lux::script::ScriptEventSourceDescription* event = nullptr
    )
    {
        constexpr std::string_view source = "return { tick = function(self) self.count = 1 end }";
        lux::rdesc::Script description;
        description.module_name = "lux.test.lua.sparse-prepared";
        description.exports.push_back({"tick", lux::script::ScriptSymbolId{1U}, {}, {}});
        description.api_requirements.push_back({
            lux::script::ScriptApiContractId{ability.id.name()},
            ability.schema_hash
        });
        description.body = lux::rdesc::LuaSourceScript{"SparsePrepared"};
        if (event != nullptr)
            description.event_requirements.push_back(*event);
        std::vector<std::byte> payload;
        payload.reserve(source.size());
        for (const auto character : source)
            payload.push_back(static_cast<std::byte>(character));
        auto created = lux::script::ScriptArtifact::create(std::move(description), std::move(payload));
        assert(created);
        return std::move(*created);
    }
}

int main(int argc, char**)
{
    std::deque<std::string> names;
    std::vector<std::array<lux::script::ScriptAbilityMethodDescription, MethodsPerAbility>> methods;
    std::vector<std::array<lux::script::lua::ScriptAbilityLuaMethodProjection, MethodsPerAbility>> projections;
    std::vector<lux::script::ScriptAbilityDescription> abilities;
    std::vector<lux::script::lua::ScriptAbilityLuaContribution> contributions;
    methods.reserve(AbilityCount);
    projections.reserve(AbilityCount);
    abilities.reserve(AbilityCount);
    contributions.reserve(AbilityCount);
    for (std::size_t ability_index{}; ability_index < AbilityCount; ++ability_index)
    {
        names.push_back("lux.test.lua.catalog." + std::to_string(ability_index));
        const auto& contract = names.back();
        names.push_back("Catalog" + std::to_string(ability_index));
        const auto& code_name = names.back();
        std::array<lux::script::ScriptAbilityMethodDescription, MethodsPerAbility> ability_methods;
        for (std::size_t method_index{}; method_index < MethodsPerAbility; ++method_index)
        {
            names.push_back(contract + ".method_" + std::to_string(method_index));
            const auto& method_id = names.back();
            names.push_back("method" + std::to_string(method_index));
            const auto& method_name = names.back();
            ability_methods[method_index] = {
                lux::script::ScriptApiMethodIdView{method_id},
                method_name,
                method_name,
                lux::script::EScriptApiMethodKind::QUERY,
                {},
                {}
            };
        }
        methods.push_back(ability_methods);
        const auto schema_hash = lux::script::scriptAbilitySchemaHash(
            lux::script::ScriptApiContractIdView{contract},
            lux::script::EScriptAbilityReceiverKind::PROVIDER_INSTANCE,
            methods.back()
        );
        abilities.push_back({
            lux::script::ScriptApiContractIdView{contract},
            code_name,
            code_name,
            1U,
            schema_hash,
            lux::script::EScriptAbilityReceiverKind::PROVIDER_INSTANCE,
            methods.back()
        });
        std::array<lux::script::lua::ScriptAbilityLuaMethodProjection, MethodsPerAbility> ability_projections;
        for (std::size_t method_index{}; method_index < MethodsPerAbility; ++method_index)
            ability_projections[method_index] = {methods.back()[method_index].id, &project};
        projections.push_back(ability_projections);
        contributions.push_back({std::addressof(abilities.back()), projections.back()});
    }

    auto backend = lux::simulation::script::LuaScriptBackend::create({
        .instance_capacity = 100000U,
        .prepared_call_capacity = 1U,
        .continuation_capacity = 1U,
        .execution_depth_capacity = 4U,
        .ability_catalog_method_capacity = AbilityCount * MethodsPerAbility,
        .prepared_ability_capacity = 4U,
        .abilities = contributions,
        .prepared_ability_blocks = std::array{
            lux::simulation::script::LuaPreparedBlockClass{
                MethodsPerAbility,
                2U
            }
        },
        .prepared_ability_storage_bytes =
            64U * 1024U * 1024U
    });
    assert(backend);
    const auto initial_stats = backend->stats();
    assert(initial_stats.prepared_ability_slots == 0U);
    assert(initial_stats.prepared_binding_bytes < 1024U * 1024U);

    std::array<lux::script::ScriptAbilityErasedMethodBinding, MethodsPerAbility> runtime_methods;
    for (std::size_t index{}; index < runtime_methods.size(); ++index)
    {
        runtime_methods[index] = {
            methods.front()[index].id,
            lux::script::EScriptApiMethodKind::QUERY,
            {},
            {},
            &invoke,
            nullptr
        };
    }
    int provider{};
    const lux::simulation::script::PreparedScriptApiCapability capability{
        lux::script::ScriptApiContractId{abilities.front().id.name()},
        abilities.front().schema_hash,
        std::addressof(provider),
        std::addressof(provider),
        1U,
        runtime_methods
    };
    auto script = artifact(abilities.front());
    const auto required = lux::simulation::script::describeLuaPreparedRequirements(script.description(), contributions);
    assert(required && required->ability_methods == MethodsPerAbility && required->event_sources == 0U);
    lux::simulation::script::ScriptBackendInstance instance;
    auto descriptor = backend->descriptor();
    const auto created = descriptor.createInstance(
        descriptor.context,
        {
            {},
            lux::simulation::script::SimulationScriptScope{},
            nullptr,
            {1U, 1U},
            std::span{&capability, 1U},
            {}
        },
        script,
        instance
    );
    assert(created == lux::simulation::script::EScriptBackendResult::SUCCESS);
    const auto prepared_stats = backend->stats();
    assert(prepared_stats.prepared_ability_slots == MethodsPerAbility);
    assert(prepared_stats.prepared_ability_high_water == MethodsPerAbility);
    assert(prepared_stats.prepared_binding_bytes == initial_stats.prepared_binding_bytes);
    descriptor.destroyInstance(descriptor.context, instance);
    assert(backend->stats().prepared_ability_slots == 0U);

    // The large structural population is opt-in, not repeated in every normal CTest profile or race loop.
    if (argc > 1)
    {
        using namespace lux::simulation::script;
        const lux::script::ScriptEventSourceDescription event{
            "MemoryFixture", "ready", 1U, 1U, lux::script::EScriptEventRoute::SIMULATION_BROADCAST,
            {"lux.i32", lux::semantic::typeId("lux.i32"), LUX_SCRIPT_VK_INT32, 4U, 4U},
            1U, 1U, 1U, 1U, 1U
        };
        auto content = artifact(abilities.front(), &event);
        const auto required_population = describeLuaPreparedRequirements(content.description(), contributions);
        assert(required_population && required_population->ability_methods == MethodsPerAbility &&
            required_population->event_sources == 1U);
        for (const std::size_t population : {10000U, 50000U, 100000U})
        {
            auto storage_backend = LuaScriptBackend::create(
                {.instance_capacity = population,
                 .prepared_call_capacity = 1U,
                 .continuation_capacity = 1U,
                 .execution_depth_capacity = 4U,
                 .ability_catalog_method_capacity = AbilityCount * MethodsPerAbility,
                 .prepared_ability_capacity = population * required_population->ability_methods,
                 .abilities = contributions,
                 .event_catalog_capacity = 1U,
                 .prepared_event_capacity = population * required_population->event_sources,
                 .events = {&event, 1U},
                 .prepared_ability_blocks =
                     std::array{LuaPreparedBlockClass{required_population->ability_methods, population}},
                 .prepared_ability_storage_bytes = 64U * 1024U * 1024U,
                 .prepared_event_blocks =
                     std::array{LuaPreparedBlockClass{required_population->event_sources, population}},
                 .prepared_event_storage_bytes = 64U * 1024U * 1024U});
            assert(storage_backend);
            const auto runtime = storage_backend->descriptor();
            const auto initial_bytes = storage_backend->stats().prepared_binding_bytes;
            const PreparedScriptEventAdmission prepared_event{&event, {}, {}, {}};
            std::vector<ScriptBackendInstance> instances(population);
            const auto create = [&](std::size_t index, std::uint32_t generation) {
                const auto result = runtime.createInstance(runtime.context,
                    {{}, SimulationScriptScope{}, nullptr,
                        {static_cast<std::uint32_t>(index + 1U), generation}, {&capability, 1U}, {&prepared_event, 1U}},
                    content, instances[index]);
                assert(result == EScriptBackendResult::SUCCESS);
            };
            for (std::size_t index{}; index < population; ++index)
                create(index, 1U);
            for (std::size_t index{}; index < population; index += 2U)
            {
                const auto before = storage_backend->stats().prepared_release_steps;
                runtime.destroyInstance(runtime.context, instances[index]);
                assert(storage_backend->stats().prepared_release_steps - before <= 12U);
            }
            for (std::size_t index{}; index < population; index += 2U)
                create(index, 2U);
            std::vector<std::size_t> order(population);
            std::iota(order.begin(), order.end(), 0U);
            std::mt19937 random{1592598566U};
            std::shuffle(order.begin(), order.end(), random);
            for (const auto index : order)
            {
                const auto before = storage_backend->stats().prepared_release_steps;
                runtime.destroyInstance(runtime.context, instances[index]);
                assert(storage_backend->stats().prepared_release_steps - before <= 12U);
            }
            const auto final = storage_backend->stats();
            assert(final.prepared_ability_slots == 0U && final.prepared_event_slots == 0U);
            assert(final.prepared_binding_bytes == initial_bytes);
            std::printf("lua_instances=%zu catalog_methods=%zu prepared_bytes=%zu acquire=%llu release=%llu\n",
                population, AbilityCount * MethodsPerAbility, final.prepared_binding_bytes,
                static_cast<unsigned long long>(final.prepared_acquire_steps),
                static_cast<unsigned long long>(final.prepared_release_steps));
        }
    }
    return 0;
}
