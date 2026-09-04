#include <lux/engine/simulation/scripting/lua/LuaScriptAbilityProjection.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using namespace lux::simulation::script;

    struct Dispatch final
    {
        void (*call)(void*) noexcept;
    };

    int call(lua_State* state) noexcept
    {
        return detail::invokeLuaAbility<void>(state, [](detail::LuaPreparedAbilityAccess& access) noexcept {
            static_cast<const Dispatch*>(access.dispatch)->call(access.context);
        });
    }

    lux::script::ScriptAbilityErasedCallResult erased(
        void*,
        const void*,
        std::span<const lux::script::ScriptAbilityInputSlot>,
        std::span<lux::script::ScriptAbilityOutputSlot>
    ) noexcept
    {
        return {};
    }

    struct Ability final
    {
        Ability(std::string_view contract, std::string_view name, std::string_view method)
            : methods{{{lux::script::ScriptApiMethodIdView{method}, "call", "Call",
                        lux::script::EScriptApiMethodKind::COMMAND, {}, {}}}},
              description{lux::script::ScriptApiContractIdView{contract}, name, name, 1U,
                  lux::script::scriptAbilitySchemaHash(lux::script::ScriptApiContractIdView{contract},
                      lux::script::EScriptAbilityReceiverKind::PROVIDER_INSTANCE, methods),
                  lux::script::EScriptAbilityReceiverKind::PROVIDER_INSTANCE, methods},
              projections{{{methods.front().id, &call}}},
              bindings{{{methods.front().id, methods.front().kind, {}, {}, &erased, nullptr}}}
        {
        }

        std::array<lux::script::ScriptAbilityMethodDescription, 1U> methods;
        lux::script::ScriptAbilityDescription description;
        std::array<lux::script::lua::ScriptAbilityLuaMethodProjection, 1U> projections;
        std::array<lux::script::ScriptAbilityErasedMethodBinding, 1U> bindings;
    };

    lux::script::ScriptArtifact makeArtifact(const Ability& ability, std::string_view source)
    {
        lux::rdesc::Script description;
        description.module_name = ability.description.name;
        description.body = lux::rdesc::LuaSourceScript{"Object"};
        description.api_requirements.push_back({
            lux::script::ScriptApiContractId{ability.description.id.name()}, ability.description.schema_hash
        });
        description.exports = {{"save", 1U, {}, {}}, {"own", 2U, {}, {}}, {"foreign", 3U, {}, {}}};
        const auto bytes = std::as_bytes(std::span{source.data(), source.size()});
        auto result = lux::script::ScriptArtifact::create(
            std::move(description), std::vector<std::byte>{bytes.begin(), bytes.end()}
        );
        assert(result);
        return std::move(*result);
    }

    lux::asset::AssetId assetId(std::uint8_t number)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.front() = number;
        return lux::asset::AssetId{bytes};
    }
}

int main()
{
    Ability alpha{"lux.test.provenance.alpha", "Alpha", "lux.test.provenance.alpha.call"};
    Ability beta{"lux.test.provenance.beta", "Beta", "lux.test.provenance.beta.call"};
    const std::array contributions{
        lux::script::lua::ScriptAbilityLuaContribution{&alpha.description, alpha.projections},
        lux::script::lua::ScriptAbilityLuaContribution{&beta.description, beta.projections}
    };
    auto backend = LuaScriptBackend::create({
        .instance_capacity = 4U,
        .prepared_call_capacity = 4U,
        .continuation_capacity = 1U,
        .execution_depth_capacity = 4U,
        .ability_catalog_method_capacity = 2U,
        .prepared_ability_capacity = 4U,
        .abilities = contributions
    });
    assert(backend);
    const auto runtime = backend->descriptor();
    const Dispatch dispatch{[](void* context) noexcept { ++*static_cast<int*>(context); }};
    int alpha_calls{};
    int beta_calls{};
    const auto capability = [&](const Ability& ability, int& calls) {
        return PreparedScriptApiCapability{
            lux::script::ScriptApiContractId{ability.description.id.name()}, ability.description.schema_hash,
            &calls, &dispatch, 1U, ability.bindings
        };
    };
    auto alpha_capability = capability(alpha, alpha_calls);
    auto beta_capability = capability(beta, beta_calls);
    auto alpha_artifact = makeArtifact(alpha,
        "return {save=function() table.lux_saved= lux.Alpha.call end,"
        "own=function() lux.Alpha.call() end, foreign=function() table.lux_saved() end}"
    );
    auto beta_artifact = makeArtifact(beta,
        "return {save=function() end, own=function() lux.Beta.call() end,"
        "foreign=function() table.lux_saved() end}"
    );
    const auto create = [&](std::uint8_t asset, const lux::script::ScriptArtifact& artifact,
                            const PreparedScriptApiCapability& prepared) {
        ScriptBackendInstance instance;
        assert(runtime.createInstance(runtime.context,
            {assetId(asset), SimulationScriptScope{}, nullptr, {asset, 1U}, {&prepared, 1U}, {}},
            artifact, instance) == EScriptBackendResult::SUCCESS);
        return instance;
    };
    const auto invoke = [&](ScriptBackendInstance instance, const lux::script::ScriptArtifact& artifact,
                            std::size_t method) {
        ScriptBackendPreparedMethod prepared;
        assert(runtime.prepareMethod(runtime.context, instance, artifact.description().exports[method], prepared) ==
            EScriptBackendResult::SUCCESS);
        lux_script_call_frame frame{nullptr, 0U, 0U, nullptr, 0U, 0U, nullptr, prepared.synchronous.context};
        const auto result = prepared.synchronous.invoke(&frame);
        runtime.releaseMethod(runtime.context, instance, prepared);
        return result;
    };
    auto first = create(1U, alpha_artifact, alpha_capability);
    auto second = create(2U, beta_artifact, beta_capability);
    assert(invoke(first, alpha_artifact, 0U) == 0);
    assert(invoke(second, beta_artifact, 2U) != 0);
    assert(alpha_calls == 0 && beta_calls == 0);
    assert(invoke(second, beta_artifact, 1U) == 0);
    assert(beta_calls == 1);
    auto same_prototype = create(1U, alpha_artifact, beta_capability = capability(alpha, beta_calls));
    assert(invoke(same_prototype, alpha_artifact, 2U) == 0);
    assert(alpha_calls == 0 && beta_calls == 2);
    runtime.destroyInstance(runtime.context, first);
    runtime.destroyInstance(runtime.context, same_prototype);
    auto rebuilt = create(3U, alpha_artifact, alpha_capability);
    assert(invoke(rebuilt, alpha_artifact, 2U) != 0);
    assert(alpha_calls == 0 && beta_calls == 2);
    runtime.destroyInstance(runtime.context, rebuilt);
    runtime.destroyInstance(runtime.context, second);
    assert(backend->stats().prepared_ability_slots == 0U);
}
