#include <lux/engine/simulation/ScriptBindingSession.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationStepInfo.hpp>
#include <lux/engine/simulation/SystemEventBuffer.hpp>
#include <lux/engine/simulation/ecs/EcsSnapshot.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <vector>

namespace
{
    using namespace lux::simulation;

    struct Pulse final
    {
        std::int32_t value{};
    };

    inline constexpr std::array kHooks{
        makeSystemHookPoint<void(const SimulationStepInfo&)>(
            "after",
            ESystemHookCardinality::MULTI
        ),
        makeSystemHookPoint<void()>(
            "single",
            ESystemHookCardinality::SINGLE
        )};
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

    constexpr lux::script::ScriptSymbolId kGlobalHook{11U};
    constexpr lux::script::ScriptSymbolId kGlobalEvent{12U};
    constexpr lux::script::ScriptSymbolId kEntityHook{21U};
    constexpr lux::script::ScriptSymbolId kEntityEvent{22U};
    constexpr lux::script::ScriptSymbolId kConstruct{23U};
    constexpr lux::script::ScriptSymbolId kStart{24U};
    constexpr lux::script::ScriptSymbolId kStop{25U};

    struct Backend;

    struct Instance final
    {
        Backend* backend{};
        ScriptMountId mount;
        ecs::Entity self{ecs::NullEntity};
        ScriptInstanceHostContext* host{};
    };

    struct Method final
    {
        Instance* instance{};
        lux::script::ScriptSymbolId symbol{};
    };

    struct Backend final
    {
        std::vector<std::int64_t> log;
        std::size_t creates{};
        std::size_t prepares{};
        std::size_t releases{};
        std::size_t destroys{};
    };

    std::int64_t logValue(
        const Method& method,
        std::int32_t value = 0
    ) noexcept
    {
        return static_cast<std::int64_t>(method.instance->mount.value) *
                100000LL +
            static_cast<std::int64_t>(method.symbol) * 100LL + value;
    }

    int invoke(lux_script_call_frame* frame) noexcept
    {
        if (!frame || !frame->user_context)
            return -1;
        auto& method = *static_cast<Method*>(frame->user_context);
        std::int32_t value{};
        if ((method.symbol == kGlobalEvent ||
             method.symbol == kEntityEvent) &&
            frame->arg_count == 1U)
        {
            value = static_cast<const Pulse*>(frame->args[0].data)->value;
        }
        else if (method.symbol == kStop && frame->arg_count == 1U)
        {
            value = static_cast<std::int32_t>(
                *static_cast<const std::uint32_t*>(frame->args[0].data)
            );
        }
        if (method.instance->self != ecs::NullEntity)
        {
            assert(method.instance->host->attached());
            assert(method.instance->host->self() == method.instance->self);
        }
        method.instance->backend->log.push_back(logValue(method, value));
        return 0;
    }

    EScriptBackendResult createInstance(
        void* opaque,
        const ScriptInstanceCreateContext& context,
        const lux::asset::ScriptAssetContent&,
        ScriptBackendInstance& result
    ) noexcept
    {
        auto& backend = *static_cast<Backend*>(opaque);
        assert(context.host);
        assert(!context.host->attached());
        auto* instance = new (std::nothrow) Instance{
            &backend,
            context.mount,
            context.self,
            context.host};
        if (!instance)
            return EScriptBackendResult::ALLOCATION_FAILURE;
        ++backend.creates;
        result.value = instance;
        return EScriptBackendResult::SUCCESS;
    }

    EScriptBackendResult prepareMethod(
        void* opaque,
        ScriptBackendInstance instance,
        const lux::rdesc::ScriptFunction& function,
        lux::script::BoundScriptCall& result
    ) noexcept
    {
        auto& backend = *static_cast<Backend*>(opaque);
        auto* method = new (std::nothrow) Method{
            static_cast<Instance*>(instance.value),
            function.symbol_id};
        if (!method)
            return EScriptBackendResult::ALLOCATION_FAILURE;
        ++backend.prepares;
        result = lux::script::BoundScriptCall{&invoke, method};
        return EScriptBackendResult::SUCCESS;
    }

    void releaseMethod(
        void* opaque,
        ScriptBackendInstance,
        lux::script::BoundScriptCall call
    ) noexcept
    {
        ++static_cast<Backend*>(opaque)->releases;
        delete static_cast<Method*>(call.context);
    }

    void destroyInstance(
        void* opaque,
        ScriptBackendInstance instance
    ) noexcept
    {
        ++static_cast<Backend*>(opaque)->destroys;
        delete static_cast<Instance*>(instance.value);
    }

    struct Assets final
    {
        struct Entry final
        {
            lux::asset::AssetId id;
            lux::asset::ScriptAssetContent content;
        };
        std::array<Entry, 3U> entries;
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
        for (auto& entry : assets.entries)
        {
            if (entry.id == id)
            {
                result.asset = &entry.content;
                return true;
            }
        }
        return false;
    }

    lux::asset::AssetId assetId(std::uint8_t first)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = first;
        return lux::asset::AssetId{bytes};
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

    ScriptBindingDescription hookBinding(
        lux::script::ScriptSymbolId function,
        std::string hook
    )
    {
        return {
            function,
            SystemHookBindingTarget{
                systemTypeId(kSystem.canonical_name),
                "fixture",
                std::move(hook)}};
    }

    ScriptBindingDescription eventBinding(
        lux::script::ScriptSymbolId function,
        std::string event
    )
    {
        return {
            function,
            SystemEventBindingTarget{
                systemTypeId(kSystem.canonical_name),
                "fixture",
                std::move(event)}};
    }

    ScriptBindingDescription lifecycleBinding(
        lux::script::ScriptSymbolId function,
        EBehaviorLifecyclePoint point
    )
    {
        return {function, BehaviorLifecycleBindingTarget{point}};
    }

    lux_script_call_frame pulseFrame(
        const Pulse& pulse,
        lux_script_value_slot& slot
    )
    {
        slot = lux_script_value_slot{
            LUX_SCRIPT_VK_STRUCT_REF,
            {},
            sizeof(Pulse),
            lux::script::scriptSemanticTypeId("lux.event.Pulse"),
            const_cast<Pulse*>(std::addressof(pulse))};
        return {&slot, 1U, 0U, nullptr, 0U, 0U, nullptr, nullptr};
    }
}

int main()
{
    using namespace lux::simulation;

    Assets assets;
    assets.entries[0].id = assetId(0x42U);
    auto& global = assets.entries[0].content.description;
    global.module_name = "binding.global.fixture";
    global.model = lux::rdesc::EScriptModel::GLOBAL_MODULE;
    global.body = lux::rdesc::CppStaticScript{"global-fixture"};
    global.exports = {
        {"after", kGlobalHook,
         {valueType(
             "lux.simulation.SimulationStepInfo",
             lux::script::EScriptPassMode::CONST_REF)}, {}},
        {"pulse", kGlobalEvent,
         {valueType(
             "lux.event.Pulse",
             lux::script::EScriptPassMode::CONST_REF)}, {}}};

    assets.entries[1].id = assetId(0x43U);
    auto& behavior = assets.entries[1].content.description;
    behavior.module_name = "binding.behavior.fixture";
    behavior.model = lux::rdesc::EScriptModel::ENTITY_BEHAVIOR;
    behavior.body = lux::rdesc::CppStaticScript{"behavior-fixture"};
    behavior.exports = {
        {"after", kEntityHook,
         {valueType(
             "lux.simulation.SimulationStepInfo",
             lux::script::EScriptPassMode::CONST_REF)}, {}},
        {"pulse", kEntityEvent,
         {valueType(
             "lux.event.Pulse",
             lux::script::EScriptPassMode::CONST_REF)}, {}},
        {"construct", kConstruct, {}, {}},
        {"start", kStart, {}, {}},
        {"stop", kStop,
         {valueType(
             std::string{BehaviorStopReasonCanonicalName},
             lux::script::EScriptPassMode::VALUE)}, {}}};
    assert(lux::rdesc::validScriptDescription(global));
    assert(lux::rdesc::validScriptDescription(behavior));

    assets.entries[2].id = assetId(0x44U);
    auto& python = assets.entries[2].content.description;
    python.module_name = "binding.python.fixture";
    python.model = lux::rdesc::EScriptModel::ENTITY_BEHAVIOR;
    python.body = lux::rdesc::PythonSourceScript{"PythonBehavior"};
    python.exports = {{
        "after",
        31U,
        {valueType(
            "lux.simulation.SimulationStepInfo",
            lux::script::EScriptPassMode::CONST_REF)},
        {}}};
    assert(lux::rdesc::validScriptDescription(python));

    {
        SimulationDescriptionBuilder python_builder;
        assert(python_builder.addSystem("fixture", kSystem));
        auto python_description = std::move(python_builder).build();
        assert(python_description);
        ecs::Registry python_registry;
        const auto python_entity = python_registry.create();
        python_registry.emplace<ScriptComponent>(
            python_entity,
            ScriptComponent{{{
                ScriptMountId{30U},
                assets.entries[2].id,
                {hookBinding(31U, "after")}}}}
        );
        auto python_session = ScriptBindingSession::create(
            std::move(*python_description),
            python_registry,
            ScriptBindingCapacities{1U, 1U, 4U, 4U, 4U, 4U, 4U},
            ScriptAssetResolver{&assets, &resolveAsset},
            {}
        );
        assert(python_session);
        const auto prepared = python_session->prepare();
        assert(!prepared);
        assert(prepared.error() == EScriptBindingError::BACKEND_NOT_AVAILABLE);
    }

    const ScriptMountDescription global_mount{
        ScriptMountId{10U},
        assets.entries[0].id,
        {
            hookBinding(kGlobalHook, "after"),
            eventBinding(kGlobalEvent, "global-pulse"),
        }};
    const ScriptMountDescription entity_mount{
        ScriptMountId{20U},
        assets.entries[1].id,
        {
            lifecycleBinding(kConstruct, EBehaviorLifecyclePoint::CONSTRUCT),
            lifecycleBinding(kStart, EBehaviorLifecyclePoint::START),
            lifecycleBinding(kStop, EBehaviorLifecyclePoint::STOP),
            hookBinding(kEntityHook, "after"),
            eventBinding(kEntityEvent, "entity-pulse"),
        }};

    SimulationDescriptionBuilder builder;
    assert(builder.addSystem("fixture", kSystem));
    assert(builder.addGlobalScriptMount(global_mount));
    auto description = std::move(builder).build();
    assert(description);

    ecs::Registry registry;
    const auto entity = registry.create();
    registry.emplace<ScriptComponent>(
        entity,
        ScriptComponent{{entity_mount}}
    );
    const auto second_entity = registry.create();
    auto second_mount = entity_mount;
    second_mount.id = ScriptMountId{21U};
    registry.emplace<ScriptComponent>(
        second_entity,
        ScriptComponent{{second_mount}}
    );

    Backend backend;
    const ScriptBackendDescriptor backend_descriptor{
        lux::rdesc::Script::Kind::CPP_STATIC,
        &backend,
        &createInstance,
        &prepareMethod,
        &releaseMethod,
        &destroyInstance};
    const ScriptAssetResolver resolver{&assets, &resolveAsset};

    {
        SimulationDescriptionBuilder capacity_builder;
        assert(capacity_builder.addSystem("fixture", kSystem));
        auto capacity_description = std::move(capacity_builder).build();
        assert(capacity_description);
        ecs::Registry capacity_registry;
        const auto capacity_entity = capacity_registry.create();
        auto capacity_mount = entity_mount;
        capacity_mount.bindings = {
            hookBinding(kEntityHook, "after"),
            eventBinding(kEntityEvent, "entity-pulse")};
        capacity_registry.emplace<ScriptComponent>(
            capacity_entity,
            ScriptComponent{{capacity_mount}}
        );
        Backend capacity_backend;
        const ScriptBackendDescriptor capacity_backend_descriptor{
            lux::rdesc::Script::Kind::CPP_STATIC,
            &capacity_backend,
            &createInstance,
            &prepareMethod,
            &releaseMethod,
            &destroyInstance};
        auto capacity_session = ScriptBindingSession::create(
            std::move(*capacity_description),
            capacity_registry,
            ScriptBindingCapacities{1U, 1U, 4U, 4U, 4U, 4U, 4U},
            resolver,
            std::span{&capacity_backend_descriptor, 1U}
        );
        assert(capacity_session);
        const auto capacity_prepare = capacity_session->prepare();
        assert(!capacity_prepare);
        assert(
            capacity_prepare.error() ==
            EScriptBindingError::CAPACITY_EXCEEDED
        );
        assert(capacity_backend.creates == 0U);
        assert(capacity_backend.prepares == 0U);
        assert(capacity_backend.releases == 0U);
        assert(capacity_backend.destroys == 0U);
    }

    const std::array duplicate_backends{
        backend_descriptor,
        backend_descriptor};
    auto duplicate = ScriptBindingSession::create(
        SimulationDescription{},
        registry,
        ScriptBindingCapacities{16U, 64U, 64U, 64U, 64U, 16U, 16U},
        resolver,
        duplicate_backends
    );
    assert(!duplicate);
    assert(duplicate.error() == EScriptBindingError::DUPLICATE_BACKEND_KIND);

    auto created = ScriptBindingSession::create(
        std::move(*description),
        registry,
        ScriptBindingCapacities{16U, 64U, 64U, 64U, 64U, 16U, 16U},
        resolver,
        std::span{&backend_descriptor, 1U}
    );
    assert(created);
    auto session = std::move(*created);
    assert(session.prepare());
    assert(session.instanceCount() == 3U);
    assert(session.preparedMethodCount() == 12U);
    assert(backend.creates == 3U);
    assert(backend.log.size() == 4U);
    assert((backend.log == std::vector<std::int64_t>{
        2002300LL,
        2102300LL,
        2002400LL,
        2102400LL}));

    const auto after = session.hookSlot("fixture", "after");
    const auto global_event = session.eventSlot("fixture", "global-pulse");
    const auto entity_event = session.eventSlot("fixture", "entity-pulse");
    assert(after && global_event && entity_event);

    Pulse global_pulse{10};
    Pulse entity_pulse{11};
    lux_script_value_slot global_slot{};
    lux_script_value_slot entity_slot{};
    auto global_frame = pulseFrame(global_pulse, global_slot);
    auto entity_frame = pulseFrame(entity_pulse, entity_slot);
    assert(session.dispatchEvent(
        global_event,
        ecs::NullEntity,
        global_frame
    ).calls == 1U);
    assert(session.dispatchEvent(
        entity_event,
        entity,
        entity_frame
    ).calls == 1U);

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
    assert(session.dispatchHook(after, hook_frame).calls == 1U);
    assert(session.dispatchHook(after, entity, hook_frame).calls == 1U);

    SystemEventBuffer<Pulse> worker_events;
    assert(worker_events.prepare(2U, 1U));
    auto producer_zero = worker_events.writer(0U);
    auto producer_one = worker_events.writer(1U);
    assert(producer_zero && producer_one);
    std::optional<SystemEventBuffer<Pulse>::Writer> zero_writer{
        std::move(*producer_zero)};
    std::optional<SystemEventBuffer<Pulse>::Writer> one_writer{
        std::move(*producer_one)};
    const auto log_before_workers = backend.log.size();
    lux::task::TaskGraphBuilder task_builder;
    auto zero_task = task_builder.add([&]() noexcept
    {
        Pulse worker_payload{30};
        assert(zero_writer->emit(entity, worker_payload));
        zero_writer.reset();
    });
    auto one_task = task_builder.add([&]() noexcept
    {
        Pulse worker_payload{40};
        assert(one_writer->emit(entity, worker_payload));
        one_writer.reset();
    });
    assert(zero_task && one_task);
    auto safe_task = task_builder.add(
        lux::task::on(lux::task::ETaskAffinity::CALLER_THREAD),
        lux::task::dependsOn(*zero_task),
        lux::task::dependsOn(*one_task),
        [&]() noexcept
        {
            assert(worker_events.drain(
                [&](ecs::Entity target, const Pulse& payload) noexcept
                {
                    lux_script_value_slot payload_slot{};
                    auto payload_frame = pulseFrame(
                        payload,
                        payload_slot
                    );
                    assert(session.dispatchEvent(
                        entity_event,
                        target,
                        payload_frame
                    ).calls == 1U);
                }
            ));
            assert(session.dispatchHook(after, entity, hook_frame).calls == 1U);
        }
    );
    assert(safe_task);
    auto worker_graph = std::move(task_builder).build();
    assert(worker_graph);
    lux::task::TaskExecutor worker_executor({2U, worker_graph->taskCount()});
    assert(backend.log.size() == log_before_workers);
    assert(worker_executor.execute(*worker_graph));
    assert(backend.log.size() == log_before_workers + 3U);
    assert(backend.log[log_before_workers] == 2002230LL);
    assert(backend.log[log_before_workers + 1U] == 2002240LL);
    assert(backend.log[log_before_workers + 2U] == 2002100LL);
    const auto cold_asset_resolutions =
        session.instrumentation().asset_resolutions;
    const auto cold_target_resolutions =
        session.instrumentation().target_resolutions;
    assert(session.dispatchHook(after, entity, hook_frame).calls == 1U);
    assert(
        session.instrumentation().asset_resolutions == cold_asset_resolutions
    );
    assert(
        session.instrumentation().target_resolutions == cold_target_resolutions
    );

    const auto mount_schema = ecs::makeComponentSchema<ScriptComponent>(
        ecs::componentSchemaId(ScriptComponentCanonicalName),
        ScriptComponentSchemaVersion
    );
    auto schemas = ecs::ComponentSchemaSet::build({mount_schema});
    assert(schemas);
    const std::array snapshot_bindings{
        ecs::bindComponentSnapshot<ScriptComponent>(mount_schema)};
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
    assert((*restored)->get<ScriptComponent>(entity) ==
        registry.get<ScriptComponent>(entity));

    const auto creates_before_edit = backend.creates;
    registry.patch<ScriptComponent>(entity, [](auto& component)
    {
        std::swap(
            component.mounts[0].bindings[3],
            component.mounts[0].bindings[4]
        );
    });
    assert(session.applyQuiescentMutations());
    assert(backend.creates == creates_before_edit);

    registry.patch<ScriptComponent>(entity, [](auto& component)
    {
        component.mounts.clear();
    });
    assert(session.dispatchHook(after, entity, hook_frame).calls == 0U);
    assert(session.applyQuiescentMutations());
    assert(session.instanceCount() == 2U);
    assert(backend.log.back() == 2002500LL);

    registry.destroy(second_entity);
    const auto reused = registry.create();
    assert(ecs::entityBits(reused) != ecs::entityBits(second_entity));
    assert(session.dispatchHook(after, reused, hook_frame).calls == 0U);
    auto reused_mount = entity_mount;
    reused_mount.id = ScriptMountId{22U};
    registry.emplace<ScriptComponent>(
        reused,
        ScriptComponent{{reused_mount}}
    );
    assert(session.dispatchHook(after, reused, hook_frame).calls == 0U);
    assert(session.applyQuiescentMutations());
    assert(session.dispatchHook(after, reused, hook_frame).calls == 1U);

    const auto invalid_entity = registry.create();
    auto invalid_mount = entity_mount;
    invalid_mount.id = ScriptMountId{23U};
    invalid_mount.bindings = {hookBinding(kStart, "single")};
    registry.emplace<ScriptComponent>(
        invalid_entity,
        ScriptComponent{{invalid_mount}}
    );
    const auto invalid_apply = session.applyQuiescentMutations();
    assert(!invalid_apply);
    assert(invalid_apply.error() == EScriptBindingError::CARDINALITY_MISMATCH);
    registry.destroy(invalid_entity);

    assert(session.shutdown());
    assert(session.instanceCount() == 0U);
    assert(backend.creates == backend.destroys);
    assert(backend.prepares == backend.releases);
}
