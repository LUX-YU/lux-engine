#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
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

    [[nodiscard]] lux::script::ScriptArtifact artifact(
        const lux::script::ScriptAbilityDescription& ability
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
        std::vector<std::byte> payload;
        payload.reserve(source.size());
        for (const auto character : source)
            payload.push_back(static_cast<std::byte>(character));
        auto created = lux::script::ScriptArtifact::create(std::move(description), std::move(payload));
        assert(created);
        return std::move(*created);
    }
}

int main()
{
    std::deque<std::string> names;
    std::vector<std::array<lux::script::ScriptAbilityMethodDescription, MethodsPerAbility>> methods;
    std::vector<lux::script::ScriptAbilityDescription> abilities;
    std::vector<lux::script::lua::ScriptAbilityLuaContribution> contributions;
    methods.reserve(AbilityCount);
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
        contributions.push_back({std::addressof(abilities.back())});
    }

    auto backend = lux::simulation::script::LuaScriptBackend::create({
        .instance_capacity = 100000U,
        .prepared_call_capacity = 1U,
        .continuation_capacity = 1U,
        .execution_depth_capacity = 4U,
        .ability_catalog_method_capacity = AbilityCount * MethodsPerAbility,
        .prepared_ability_capacity = 4U,
        .abilities = contributions
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
    return 0;
}
