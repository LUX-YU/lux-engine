#include "Behavior.hpp"
#include "Behavior.CommonBehavior.script.generated.hpp"
#include "CounterAbility.ability.generated.hpp"
#include "CounterAbility.ability.lua.generated.hpp"
#include "CounterAbility.ability.native.generated.hpp"
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/ScriptBindingAuthoring.hpp>
#include <lux/engine/simulation/ScriptSystemDescriptionCodec.hpp>
#include <lux/engine/simulation/scripting/cpp_static/CppStaticScriptBridge.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
#include <lux/engine/simulation/scripting/native/NativeScriptBackend.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>

namespace
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;
    using Ability = authoring_consumer::CounterAbility;
    constexpr lux::system::SystemInstanceId HostId{11U};
    constexpr HookPointId First{12U}, Selected{13U};
    std::int32_t provider_total{};
    unsigned provider_calls{};

    void need(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }

    std::vector<std::byte> readFile(const char* path)
    {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        need(static_cast<bool>(input), "open input");
        const auto size = input.tellg();
        need(size > 0, "input size");
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        input.seekg(0);
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        need(static_cast<bool>(input), "read input");
        return bytes;
    }

    auto loadArtifact(const char* path)
    {
        auto bytes = readFile(path);
        need(bytes.size() > 56U, "typed asset header");
        std::array<std::uint8_t, 16U> identity{};
        for (std::size_t index{}; index < identity.size(); ++index)
            identity[index] = std::to_integer<std::uint8_t>(bytes[40U + index]);
        auto asset = lux::asset::TAssetSerDeser<lux::script::ScriptArtifactAsset>::decode(lux::asset::AssetId{identity},
            lux::cxx::SharedBytes<>::copyOf(bytes),
            lux::asset::AssetDecodeLimits{32U * 1024U * 1024U, 32U * 1024U * 1024U, 0U});
        need(static_cast<bool>(asset), "decode source artifact");
        return *asset;
    }

    std::vector<lux::script::ScriptBindingHint> loadHints(const char* path, const lux::script::ScriptArtifact& artifact)
    {
        std::ifstream file(path);
        nlohmann::json document;
        file >> document;
        need(document.at("schema") == "lux-script-binding-suggestions" && document.at("version") == 1 &&
            document.at("module") == artifact.description().module_name, "suggestion provenance");
        std::vector<lux::script::ScriptBindingHint> hints;
        for (const auto& item : document.at("suggestions"))
        {
            const auto kind = item.at("kind").get<std::string>();
            need(kind == "hook" || kind == "event", "suggestion kind");
            hints.push_back({item.at("symbol").get<std::uint64_t>(), {
                kind == "hook" ? lux::script::EScriptBindingHintKind::HOOK : lux::script::EScriptBindingHintKind::EVENT,
                item.at("target").get<std::string>()}});
        }
        return hints;
    }

    struct Host final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr std::array Hooks{
            makeHookPointSpec<void(std::int32_t)>(First, "first", true, false),
            makeHookPointSpec<void(std::int32_t)>(Selected, "selected", true, true)
        };
        inline static constexpr SimulationSystemDescription Description{
            .type = {.canonical_name = "consumer.authoring.Host", .version = 1U}, .hooks = Hooks
        };
        Host() noexcept : first_endpoint(HostId, First, first), selected_endpoint(HostId, Selected, selected) {}
        void record(std::int32_t value) noexcept { provider_total += value; ++provider_calls; }
        HookPoint<void(std::int32_t)> first, selected;
        ScriptHookEndpoint<void(std::int32_t)> first_endpoint, selected_endpoint;
    };

    lux::cxx::expected<void, SimulationSystemBuildFailure> install(SimulationBuilder& builder,
        SimulationSystemView view) noexcept
    {
        auto host = builder.emplaceSystem<Host>(view.instanceId());
        if (!host) return lux::cxx::unexpected(host.error());
        if ((*host)->first.prepare(1U) != EEndpointMutationError::NONE ||
            (*host)->selected.prepare(1U) != EEndpointMutationError::NONE)
            return lux::cxx::unexpected(SimulationSystemBuildFailure{
                ESimulationSystemBuildError::CONSTRUCTION_FAILURE, view.instanceId()});
        auto result = builder.publishScriptAbility(view.instanceId(), lux::script::bindScriptAbility<Ability>(**host));
        if (!result) return result;
        result = builder.publishScriptHook(view.instanceId(), (*host)->first_endpoint.descriptor());
        if (!result) return result;
        result = builder.publishScriptHook(view.instanceId(), (*host)->selected_endpoint.descriptor());
        if (!result) return result;
        result = builder.addSystemTask<Host>(view.instanceId(), [](Host&) noexcept {});
        if (!result) return result;
        result = builder.addSystemHookTask<Host>(view.instanceId(), First,
            [](Host& value, const HookInvocation& invocation) noexcept {
                static_cast<void>(value.first.dispatch(invocation, 5));
            });
        if (!result) return result;
        return builder.addSystemHookTask<Host>(view.instanceId(), Selected,
            [](Host& value, const HookInvocation& invocation) noexcept {
                static_cast<void>(value.selected.dispatch(invocation, 5));
            });
    }

    struct Sources final
    {
        std::array<const lux::script::ScriptArtifact*, 3U> artifacts;
        std::array<lux::asset::AssetId, 3U> assets;
        std::array<lux::world::WorldObjectId, 3U> objects;
        std::array<ecs::Entity, 3U> entities;
        const lux::script::NativeModule* module{};
        static bool artifact(void* context, const lux::asset::AssetId& id, ResolvedScriptArtifact& result) noexcept
        {
            auto& source = *static_cast<Sources*>(context);
            for (std::size_t i{}; i < source.assets.size(); ++i)
                if (source.assets[i] == id) { result = {source.artifacts[i], nullptr, nullptr}; return true; }
            return false;
        }
        static bool world(void* context, const lux::world::WorldObjectId& id, ecs::Entity& result) noexcept
        {
            auto& source = *static_cast<Sources*>(context);
            for (std::size_t i{}; i < source.objects.size(); ++i)
                if (source.objects[i] == id) { result = source.entities[i]; return true; }
            return false;
        }
        static bool native(void* context, const lux::asset::AssetId& id, const lux::script::ScriptArtifact&,
            ResolvedNativeModule& result) noexcept
        {
            auto& source = *static_cast<Sources*>(context);
            if (id != source.assets[2]) return false;
            result = {source.module, nullptr, nullptr};
            return true;
        }
    };
}

int main(int argc, char** argv)
{
    try
    {
        need(argc == 3, "arguments: save|load|reject binding-file");
        const std::string_view mode{argv[1]};
        need(mode == "save" || mode == "load" || mode == "reject", "invalid mode");
        auto cpp_description = materializeCppStaticScript(generated::CommonBehavior);
        need(static_cast<bool>(cpp_description), "C++ generated contract");
        auto cpp = lux::script::ScriptArtifact::create(std::move(*cpp_description), {});
        need(static_cast<bool>(cpp), "C++ artifact");
        auto lua_asset = loadArtifact(LUA_ARTIFACT);
        auto flow_asset = loadArtifact(FLOW_ARTIFACT);
        auto module = lux::script::loadNativeModule(flow_asset->data().payload(), "consumer.authoring.flow");
        need(static_cast<bool>(module), "Native executable");

        SimulationDescriptionBuilder description;
        need(static_cast<bool>(description.addSystem(HostId, "Host", Host::Description)), "declare host");
        need(static_cast<bool>(description.addExecutionDependency(SimulationExecutionPoint::task(HostId),
            SimulationExecutionPoint::hook(HostId, First))), "task order");
        need(static_cast<bool>(description.addExecutionDependency(SimulationExecutionPoint::hook(HostId, First),
            SimulationExecutionPoint::hook(HostId, Selected))), "Hook order");
        auto built = std::move(description).build();
        need(static_cast<bool>(built), "description");
        SimulationSystemRegistry registrations;
        need(static_cast<bool>(registrations.add({
            .type = lux::system::systemTypeId(Host::Description.type.canonical_name),
            .cpp_type = lux::cxx::typeToken<Host>(), .description = &Host::Description,
            .access = Host::Access.spec(), .configuration = {}, .install = &install})), "registration");
        ecs::Registry registry;
        auto simulation = Simulation::create(registry,
            std::make_shared<SimulationDescription>(std::move(*built)), registrations);
        need(static_cast<bool>(simulation), "simulation");
        std::array<std::uint8_t, 16U> identity{};
        identity[0] = 0x31U;
        Sources sources{{&*cpp, &lua_asset->data(), &flow_asset->data()},
            {lux::asset::AssetId{identity}, lua_asset->id(), flow_asset->id()}, {}, {}, &*module};
        for (std::size_t i{}; i < sources.objects.size(); ++i)
        {
            identity[0] = static_cast<std::uint8_t>(0x71U + i);
            sources.objects[i] = lux::world::WorldObjectId{uuids::uuid{identity}};
            sources.entities[i] = registry.create();
        }
        const std::array hint_paths{CPP_HINTS, LUA_HINTS, FLOW_HINTS};
        ScriptSystemDescriptionBuilder mounts;
        const ScriptBindingTarget explicit_target = HookScriptTarget{HostId, Selected};
        for (std::size_t i{}; mode == "save" && i < sources.artifacts.size(); ++i)
        {
            auto hints = loadHints(hint_paths[i], *sources.artifacts[i]);
            need(hints.size() == 1U, "one source suggestion");
            auto choice = selectScriptBindingFromHints(*sources.artifacts[i], hints[0].symbol,
                simulation->description(), true, &explicit_target, hints);
            need(choice && choice->target == explicit_target, "explicit binding precedence");
            need(static_cast<bool>(mounts.addMount({ScriptMountId{i + 1U}, sources.assets[i],
                EntityScriptMount{sources.objects[i]}, true, {*choice}})), "mount selection");
        }
        std::optional<ScriptSystemDescription> mount_description;
        const ScriptSystemCodecLimits limits{1024U * 1024U, 1024U * 1024U, 1024U * 1024U};
        if (mode == "save")
        {
            auto built_mounts = std::move(mounts).build(simulation->description());
            need(static_cast<bool>(built_mounts), "mount description");
            mount_description.emplace(std::move(*built_mounts));
            auto bytes = encodeScriptSystemDescription(*mount_description, limits);
            need(static_cast<bool>(bytes), "encode binding selection");
            std::ofstream file(argv[2], std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
            need(static_cast<bool>(file), "save bindings");
        }
        else
        {
            auto bytes = readFile(argv[2]);
            auto restored = decodeScriptSystemDescription(bytes, simulation->description(), limits);
            need(static_cast<bool>(restored), "reload old bindings");
            mount_description.emplace(std::move(*restored));
        }
        const std::array pools{CppStaticScriptPoolDescription{&generated::CommonBehavior, 1U, 0U, 0U,
            alignof(std::max_align_t), 1U}};
        auto cpp_backend = CppStaticScriptBackend::create(pools);
        const std::array lua_contributions{lux::script::lua::makeScriptAbilityLuaContribution<Ability>()};
        const std::array lua_blocks{LuaPreparedBlockClass{1U, 1U}};
        auto lua_backend = LuaScriptBackend::create({.instance_capacity = 1U, .prepared_call_capacity = 1U,
            .continuation_capacity = 1U, .execution_depth_capacity = 8U, .ability_catalog_method_capacity = 1U,
            .prepared_ability_capacity = 1U, .abilities = lua_contributions,
            .prepared_ability_blocks = lua_blocks, .prepared_ability_storage_bytes = 4096U});
        const std::array native_contributions{lux::script::native::makeScriptAbilityNativeContribution<Ability>()};
        const std::array populations{NativeScriptStoragePopulation{&*module, 1U, 0U}};
        NativeScriptBackend native_backend{{&sources, &Sources::native}, {.module_capacity = 1U,
            .instance_capacity = 1U, .prepared_call_capacity = 1U, .max_ability_imports_per_module = 1U,
            .abilities = native_contributions, .storage_populations = populations, .state_storage_bytes = 4096U}};
        need(cpp_backend && lua_backend && native_backend, "backend preparation");
        const std::array backends{cpp_backend->descriptor(), lua_backend->descriptor(), native_backend.descriptor()};
        auto runtime = ScriptSystem::create(
            simulation->description(), *mount_description, registry, simulation->clock(),
            {16U, 3U, 8U, 8U, 8U, 8U, 64U, 8U, 8U, 8U, 8U, 8U}, {&sources, &Sources::artifact},
            {&sources, &Sources::world}, simulation->scriptApiCapabilities(), backends,
            simulation->scriptHookEndpoints(), simulation->scriptEventEndpoints());
        need(static_cast<bool>(runtime), "runtime");
        const auto prepared = runtime->prepare();
        if (mode == "reject")
        {
            need(!prepared && authoring_consumer::cpp_total == 0 && provider_calls == 0U, "fail before user code");
            return runtime->shutdown() ? 0 : 4;
        }
        need(static_cast<bool>(prepared), "mount admission");
        auto callbacks = simulation->bindHookCallbacks({&*runtime,
            [](void* state, const SimulationClockSnapshot&, bool stable) noexcept {
                auto& value = *static_cast<ScriptSystem*>(state);
                if (stable) value.beginStableAdmission();
                return static_cast<bool>(value.processLifecycle());
            },
            [](void* state, const SimulationClockSnapshot&, bool stable) noexcept {
                return !stable || static_cast<bool>(static_cast<ScriptSystem*>(state)->executeStablePoint());
            },
            [](void* state, const SimulationClockSnapshot&) noexcept {
                return static_cast<bool>(static_cast<ScriptSystem*>(state)->processLifecycle());
            }});
        auto executor = lux::task::TaskExecutor::create({2U, 32U});
        need(callbacks && executor, "execution graph binding");
        for (unsigned step{}; step < 3U; ++step)
            need(static_cast<bool>(simulation->execute(*executor, SimulationDuration{1})), "execute graph");
        need(authoring_consumer::cpp_total == 30 && provider_total == 60 && provider_calls == 6U, "observations");
        need(runtime->activeContinuationCount() == 0U && runtime->activeAwaitableCount() == 0U &&
            lua_backend->stats().vm_coroutine_creations == 0U && native_backend.stats().frame_storage_bytes == 0U,
            "sync exports must not allocate continuations");
        callbacks->reset();
        need(static_cast<bool>(runtime->shutdown()), "retire objects");
        std::cout << "three source frontends -> selected saved bindings -> graph: cpp=30 provider=60 calls=6\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
