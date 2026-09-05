#include "InventoryAbility.hpp"
#include "InventoryAbility.ability.generated.hpp"
#include "InventoryAbility.ability.lua.generated.hpp"
#include "InventoryModel.hpp"

#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <vector>

namespace
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;
    using namespace installed_consumer;
    struct InventorySystem final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr auto Description = InventoryDescription;
        using Channel = HookChannel<SimulationBroadcastRoute, std::int32_t>;
        HookPoint<void()> tick;
        Channel& channel;
        Channel::Producer writer;
        ScriptHookEndpoint<void()> hook_endpoint{InventorySystemId, TickHook, tick};
        ScriptEventEndpoint<SimulationBroadcastRoute, std::int32_t> event_endpoint;
        unsigned steps{};
        unsigned reads{};
        unsigned starts{};
        std::int32_t last{};
        explicit InventorySystem(Channel& source) noexcept
            : channel(source), event_endpoint(InventorySystemId, ChangedEvent, channel)
        {
            assert(tick.prepare(1U) == EEndpointMutationError::NONE);
        }
        std::int32_t count(std::int32_t item) noexcept { ++reads; last = item; return item + 10; }
        lux::script::ScriptAbilityStartResult countLater(
            std::int32_t item, lux::script::ScriptAbilityCompletion<std::int32_t> completion) noexcept
        {
            ++starts;
            if (!completion.success(item))
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{-7});
            return {};
        }
    };
    InventorySystem* provider{};
    auto install(SimulationBuilder& builder, SimulationSystemView view) noexcept
        -> lux::cxx::expected<void, SimulationSystemBuildFailure>
    {
        auto channel = builder.createHookChannel<SimulationBroadcastRoute, std::int32_t>(
            view.instanceId(), ChangedEvent, {1U, 1U});
        if (!channel)
            return lux::cxx::unexpected(channel.error());
        auto created = builder.emplaceSystem<InventorySystem>(view.instanceId(), **channel);
        if (!created)
            return lux::cxx::unexpected(created.error());
        provider = *created;
        auto writer = builder.bindHookChannelProducer(view.instanceId(), PrimarySimulationTask, **channel);
        if (!writer)
            return lux::cxx::unexpected(writer.error());
        provider->writer = *writer;
        auto result = builder.publishScriptAbility(
            view.instanceId(), lux::script::bindScriptAbility<InventoryAbility>(*provider));
        if (result)
            result = builder.publishScriptHook(view.instanceId(), provider->hook_endpoint.descriptor());
        if (result)
            result = builder.publishScriptEvent(view.instanceId(), provider->event_endpoint.descriptor());
        if (result)
            result = builder.addSystemTask<InventorySystem>(view.instanceId(), [](InventorySystem& value) noexcept {
                if (++value.steps != 3U)
                    return true;
                auto writer = value.writer.begin();
                return writer.record(9);
            });
        if (!result)
            return result;
        return builder.addSystemHookTask<InventorySystem>(view.instanceId(), TickHook,
            [](InventorySystem& value, const HookInvocation& invocation) noexcept {
                if (value.steps == 1U)
                    static_cast<void>(value.tick.dispatch(invocation));
            });
    }
    struct Source final
    {
        const lux::script::ScriptArtifact* artifact{};
        lux::asset::AssetId asset;
        lux::world::WorldObjectId object;
        ecs::Entity entity;
        static bool resolve(void* context, const lux::asset::AssetId& asset, ResolvedScriptArtifact& output) noexcept
        {
            auto& source = *static_cast<Source*>(context);
            if (asset != source.asset)
                return false;
            output.artifact = source.artifact;
            return true;
        }
        static bool world(void* context, const lux::world::WorldObjectId& object, ecs::Entity& output) noexcept
        {
            auto& source = *static_cast<Source*>(context);
            output = source.entity;
            return object == source.object;
        }
    };
}

int main()
{
    std::ifstream input(LUX_LUA_ARTIFACT, std::ios::binary);
    assert(input);
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    assert(size > 0);
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    assert(input && bytes.size() >= 56U);

    std::array<std::uint8_t, 16U> id_bytes{};
    for (std::size_t index{}; index < id_bytes.size(); ++index)
        id_bytes[index] = std::to_integer<std::uint8_t>(bytes[40U + index]);
    const auto decoded = lux::asset::TAssetSerDeser<lux::script::ScriptArtifactAsset>::decode(
        lux::asset::AssetId{id_bytes},
        lux::cxx::SharedBytes<>::copyOf(bytes),
        {bytes.size(), (std::numeric_limits<std::size_t>::max)(), 0U}
    );
    assert(decoded);
    const auto& description = (*decoded)->data().description();
    using Traits = lux::script::ScriptAbilityTraits<installed_consumer::InventoryAbility>;
    assert(description.api_requirements.size() == 1U);
    assert(description.api_requirements.front().contract.name() == Traits::Description.id.name());
    assert(description.api_requirements.front().expected_schema_hash == Traits::Description.schema_hash);
    assert(description.lifecycle.begin_play == 1U);
    assert(description.lifecycle.end_play == 2U);
    const auto& lua = std::get<lux::rdesc::LuaSourceScript>(description.body);
    assert(lua.suspension_capable_exports == std::vector<lux::script::ScriptSymbolId>{3U});
    assert(description.event_requirements.size() == 1U);
    assert(description.event_requirements.front().system_name == "Inventory");
    assert(description.event_requirements.front().event_name == "changed");
    assert(description.event_requirements.front().system_id == 51U);
    assert(description.event_requirements.front().event_id == 52U);
    const auto contribution = lux::script::lua::makeScriptAbilityLuaContribution<
        installed_consumer::InventoryAbility
    >();
    auto model = inventoryDescription();
    assert(model);
    ecs::Registry registry;
    SimulationSystemRegistry registrations;
    assert(registrations.add({lux::system::systemTypeId(InventoryDescription.type.canonical_name),
        lux::cxx::typeToken<InventorySystem>(), &InventoryDescription, InventorySystem::Access.spec(), {}, &install}));
    auto simulation = Simulation::create(
        registry, std::make_shared<SimulationDescription>(std::move(*model)), registrations);
    assert(simulation && provider);
    auto event = projectScriptEventSource(simulation->description().findEvent(InventorySystemId, ChangedEvent),
        provider->event_endpoint.descriptor());
    assert(event);
    const auto event_source = *event;
    auto backend = lux::simulation::script::LuaScriptBackend::create({
        .instance_capacity = 1U,
        .prepared_call_capacity = 4U,
        .continuation_capacity = 1U,
        .execution_depth_capacity = 4U,
        .ability_catalog_method_capacity = Traits::Description.methods.size(),
        .prepared_ability_capacity = Traits::Description.methods.size(),
        .abilities = {&contribution, 1U},
        .event_catalog_capacity = 1U,
        .prepared_event_capacity = 1U,
        .events = {&event_source, 1U},
        .prepared_ability_blocks = std::array{
            lux::simulation::script::LuaPreparedBlockClass{
                (Traits::Description.methods.size()) / ((1U) == 0U ? 1U : (1U)),
                1U
            }
        },
        .prepared_ability_storage_bytes =
            128U * (Traits::Description.methods.size()) + 4096U,
        .prepared_event_blocks = std::array{
            lux::simulation::script::LuaPreparedBlockClass{
                (1U) / ((1U) == 0U ? 1U : (1U)),
                1U
            }
        },
        .prepared_event_storage_bytes =
            128U * (1U) + 4096U
    });
    assert(backend);
    Source source{&(*decoded)->data(), (*decoded)->id(),
        lux::world::WorldObjectId{uuids::uuid{id_bytes}}, registry.create()};
    ScriptSystemDescriptionBuilder mounts;
    assert(mounts.addMount({ScriptMountId{1U}, source.asset, EntityScriptMount{source.object}, true,
        {{3U, HookScriptTarget{InventorySystemId, TickHook}}}}));
    auto scripts = std::move(mounts).build(simulation->description());
    assert(scripts);
    const auto backend_descriptor = backend->descriptor();
    auto runtime = ScriptSystem::create(simulation->description(), *scripts, registry, simulation->clock(),
        {8U, 1U, 2U, 2U, 2U, 2U, 32U, 2U, 2U, 2U, 2U, 2U},
        {&source, &Source::resolve}, {&source, &Source::world}, simulation->scriptApiCapabilities(),
        {&backend_descriptor, 1U}, simulation->scriptHookEndpoints(), simulation->scriptEventEndpoints());
    assert(runtime && runtime->prepare());
    assert(provider->reads == 1U && provider->last == 0);
    auto connection = simulation->bindHookCallbacks({&*runtime,
        [](void* context, const SimulationClockSnapshot&, bool) noexcept {
            auto& runtime = *static_cast<ScriptSystem*>(context);
            runtime.beginStableAdmission();
            return static_cast<bool>(runtime.processLifecycle());
        },
        [](void* context, const SimulationClockSnapshot&, bool) noexcept {
            return static_cast<bool>(static_cast<ScriptSystem*>(context)->executeStablePoint());
        }, nullptr});
    auto executor = lux::task::TaskExecutor::create({4U, 8U});
    assert(connection && executor);
    assert(simulation->execute(*executor, SimulationDuration{1}));
    assert(provider->starts == 1U && provider->reads == 1U && runtime->activeContinuationCount() == 1U);
    assert(simulation->execute(*executor, SimulationDuration{1}));
    assert(runtime->stats().active_event_waiters == 1U && provider->reads == 1U);
    assert(simulation->execute(*executor, SimulationDuration{1}));
    assert(provider->reads == 2U && provider->last == 16 && runtime->activeContinuationCount() == 0U);
    connection->reset();
    assert(runtime->shutdown());
    const auto stop_reason = static_cast<std::int32_t>(EScriptEndPlayReason::RUNTIME_STOPPED);
    assert(provider->reads == 3U && provider->last == -stop_reason);
    return 0;
}
