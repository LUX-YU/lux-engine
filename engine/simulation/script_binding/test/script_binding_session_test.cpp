#include <lux/engine/simulation/ScriptBindingSession.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationStepInfo.hpp>
#include <lux/engine/simulation/ecs/EcsSnapshot.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{
    using namespace lux::simulation;

    struct Pulse final
    {
        std::int32_t value{};
    };

    inline constexpr std::array kHooks{
        makeSystemHookPoint<void(const SimulationStepInfo&)>("after")};
    inline constexpr std::array kEvents{
        makeSystemEvent<Pulse>(
            "global-pulse",
            kHooks[0],
            ESystemEventTarget::GLOBAL,
            "lux.event.Pulse",
            1U
        ),
        makeSystemEvent<Pulse>(
            "entity-pulse",
            kHooks[0],
            ESystemEventTarget::ENTITY_TARGETED,
            "lux.event.Pulse",
            1U
        )};
    inline constexpr SystemDescription kSystem{
        .canonical_name = "lux.test.binding",
        .version = 1U,
        .hooks = kHooks,
        .events = kEvents};

    struct CallContext final
    {
        std::int32_t id{};
        std::vector<std::int32_t>* log{};
    };

    struct Backend final
    {
        std::array<CallContext, 3U> calls;
        std::size_t releases{};
    };

    int invoke(lux_script_call_frame* frame) noexcept
    {
        auto* context = static_cast<CallContext*>(frame->user_context);
        std::int32_t value{};
        if (context->id != 1 && frame->arg_count == 1U)
            value = *static_cast<const std::int32_t*>(frame->args[0].data);
        context->log->push_back(context->id * 100 + value);
        return 0;
    }

    bool prepareCall(
        void* opaque,
        const ScriptPrepareContext&,
        const lux::asset::ScriptAssetContent&,
        const lux::rdesc::ScriptFunction& function,
        lux::script::BoundScriptCall& result
    ) noexcept
    {
        auto& backend = *static_cast<Backend*>(opaque);
        if (function.symbol_id == 0U || function.symbol_id > backend.calls.size())
            return false;
        result = lux::script::BoundScriptCall{
            &invoke,
            &backend.calls[function.symbol_id - 1U]};
        return true;
    }

    void releaseCall(void* opaque, lux::script::BoundScriptCall) noexcept
    {
        ++static_cast<Backend*>(opaque)->releases;
    }

    struct Assets final
    {
        lux::asset::AssetId id;
        lux::asset::ScriptAssetContent content;
        std::size_t resolves{};
    };

    bool resolveAsset(
        void* opaque,
        const lux::asset::AssetId& id,
        ResolvedScriptAsset& result
    ) noexcept
    {
        auto& assets = *static_cast<Assets*>(opaque);
        ++assets.resolves;
        if (id != assets.id)
            return false;
        result.asset = &assets.content;
        return true;
    }

    lux::rdesc::ScriptValueType valueType(
        std::string canonical_name,
        lux::script::EScriptPassMode pass
    )
    {
        return lux::rdesc::ScriptValueType{
            canonical_name,
            lux::script::scriptSemanticTypeId(canonical_name),
            pass};
    }
}

int main()
{
    using namespace lux::simulation;

    std::array<std::uint8_t, 16U> id_bytes{};
    id_bytes[0] = 0x42U;
    Assets assets;
    assets.id = lux::asset::AssetId{id_bytes};
    assets.content.description.schema_version =
        lux::rdesc::Script::kSchemaVersion;
    assets.content.description.module_name = "binding.fixture";
    assets.content.description.body = lux::rdesc::CppBehaviorScript{"fixture"};
    assets.content.description.exports = {
        {"after", 1U,
         {valueType(
             "lux.simulation.SimulationStepInfo",
             lux::script::EScriptPassMode::CONST_REF)}, {}},
        {"global", 2U,
         {valueType("lux.event.Pulse", lux::script::EScriptPassMode::VALUE)}, {}},
        {"entity", 3U,
         {valueType("lux.event.Pulse", lux::script::EScriptPassMode::VALUE)}, {}}};
    assert(lux::rdesc::validScriptDescription(assets.content.description));

    SimulationDescriptionBuilder builder;
    assert(builder.addSystem("fixture", kSystem));
    assert(builder.addGlobalScriptMount(ScriptMountDescription{
        assets.id,
        EScriptBindingSetMode::EXPLICIT,
        {
            {1U, lux::rdesc::EScriptBindingKind::HOOK,
             "lux.test.binding", "fixture", "after"},
            {2U, lux::rdesc::EScriptBindingKind::EVENT,
             "lux.test.binding", "fixture", "global-pulse"},
        }}));
    auto description = std::move(builder).build();
    assert(description);

    ecs::Registry registry;
    const auto entity = registry.create();
    registry.emplace<ScriptMountFacts>(entity, ScriptMountFacts{{
        ScriptMountDescription{
            assets.id,
            EScriptBindingSetMode::EXPLICIT,
            {{3U, lux::rdesc::EScriptBindingKind::EVENT,
              "lux.test.binding", "fixture", "entity-pulse"}}}
    }});

    std::vector<std::int32_t> log;
    Backend backend{{
        CallContext{1, &log},
        CallContext{2, &log},
        CallContext{3, &log}}};
    const ScriptBackendDescriptor backend_descriptor{
        lux::rdesc::Script::Kind::CPP_BEHAVIOR,
        &backend,
        &prepareCall,
        &releaseCall};
    const ScriptAssetResolver resolver{&assets, &resolveAsset};
    auto created = ScriptBindingSession::create(
        std::move(*description),
        registry,
        ScriptBindingCapacities{16U, 64U, 2U, 8U, 8U},
        resolver,
        std::span{&backend_descriptor, 1U}
    );
    assert(created);
    auto session = std::move(*created);
    const auto prepared = session.prepare();
    assert(prepared);
    assert(session.preparedCallCount() == 3U);
    assert(assets.resolves == 2U);

    const auto after = session.hookSlot("fixture", "after");
    const auto global_event = session.eventSlot("fixture", "global-pulse");
    const auto entity_event = session.eventSlot("fixture", "entity-pulse");
    assert(after && global_event && entity_event);

    auto make_frame = [](Pulse& pulse, lux_script_value_slot& slot)
    {
        slot = lux_script_value_slot{
            LUX_SCRIPT_VK_STRUCT_REF,
            {},
            sizeof(Pulse),
            lux::script::scriptSemanticTypeId("lux.event.Pulse"),
            &pulse};
        return lux_script_call_frame{&slot, 1U, 0U, nullptr, 0U, 0U, nullptr, nullptr};
    };

    session.beginUpdate();
    Pulse first{10};
    Pulse second{11};
    Pulse third{20};
    lux_script_value_slot first_slot{};
    lux_script_value_slot second_slot{};
    lux_script_value_slot third_slot{};
    auto first_frame = make_frame(first, first_slot);
    auto second_frame = make_frame(second, second_slot);
    auto third_frame = make_frame(third, third_slot);
    assert(session.writer(1U).emit(global_event, third_frame));
    assert(session.writer(0U).emit(global_event, first_frame));
    assert(session.writer(0U).emit(entity_event, entity, second_frame));

    SimulationStepInfo step{1.0F / 60.0F, 7U};
    lux_script_value_slot step_slot{
        LUX_SCRIPT_VK_STRUCT_REF,
        {},
        sizeof(step),
        lux::script::scriptSemanticTypeId(
            "lux.simulation.SimulationStepInfo"
        ),
        &step};
    lux_script_call_frame hook_frame{
        &step_slot, 1U, 0U, nullptr, 0U, 0U, nullptr, nullptr};
    const auto dispatched = session.dispatchHook(after, hook_frame);
    assert(dispatched.status == 0);
    assert(dispatched.calls == 4U);
    assert((log == std::vector<std::int32_t>{210, 311, 220, 100}));
    assert(session.hotPathNameLookupCount() == 0U);
    assert(session.hotPathAssetLookupCount() == 0U);
    assert(session.hotPathSceneScanCount() == 0U);

    const auto mount_schema = ecs::makeComponentSchema<ScriptMountFacts>(
        ecs::componentSchemaId("lux.simulation.ScriptMountFacts")
    );
    auto schemas = ecs::ComponentSchemaSet::build({mount_schema});
    assert(schemas);
    const std::array snapshot_bindings{
        ecs::bindComponentSnapshot<ScriptMountFacts>(mount_schema)};
    const ecs::ComponentSnapshotContribution snapshot_contribution{
        {}, snapshot_bindings};
    auto snapshot_components = ecs::ComponentSnapshotSet::build(
        *schemas,
        std::span{&snapshot_contribution, 1U}
    );
    assert(snapshot_components);
    auto snapshot = ecs::EcsSnapshot::capture(registry, *snapshot_components);
    assert(snapshot);
    auto restored = snapshot->instantiate();
    assert(restored);
    assert((*restored)->get<ScriptMountFacts>(entity) ==
        registry.get<ScriptMountFacts>(entity));

    // A mount mutation is observed but cannot change live calls until the
    // explicitly quiescent apply point.
    registry.patch<ScriptMountFacts>(entity, [](auto& value)
    {
        value.mounts.clear();
    });
    assert(session.preparedCallCount() == 3U);
    assert(session.applyQuiescentMutations());
    assert(session.preparedCallCount() == 2U);
    assert(backend.releases == 3U);
}
