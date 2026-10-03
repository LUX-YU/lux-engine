#include <lux/engine/scene/scripting/AssetAbilityLua.hpp>
#include <lux/engine/scene/scripting/ScriptAssetAccess.hpp>
#include <lux/engine/scene/scripting/SkeletonAbility.hpp>
#include <lux/engine/process/asset_loading/AssetReadOverlay.hpp>
#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>
#include <lux/engine/resource/asset/animation/SkeletonAsset.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
#include <lux/engine/simulation/scripting/DeferredScriptHost.hpp>
#include <DelayAbility.ability.generated.hpp>
#include <DelayAbility.ability.lua.generated.hpp>

#include <cassert>
#include <atomic>
#include <fstream>
#include <iostream>
#include <thread>

using namespace lux;
using namespace lux::scene::script;
using namespace lux::simulation;
using namespace lux::simulation::script;

namespace
{
    struct ReadCount final : process::asset_loading::AssetReadPort::Endpoint
    {
        process::asset_loading::AssetReadPort read;
        std::atomic_size_t calls{};
        explicit ReadCount(process::asset_loading::AssetReadPort port) : read(std::move(port)) {}
        lux::async::SubmitResult submit(process::asset_loading::ReadAssetImage request, void* state,
            void (*complete)(void*, Outcome&&) noexcept, lux::async::SubmitOptions options) noexcept override
        {
            ++calls;
            return read.submit(request, state, complete, options);
        }
    };
    // A real public endpoint consumer. Only ScriptSystem's own connected lane is invoked,
    // inside its mandatory ExecutionRegion; no private runtime/VM entry or fake completion.
    struct Endpoint final
    {
        void* context{};
        ScriptHookLane lane{};
        ScriptHookEndpointDescriptor descriptor() noexcept
        {
            return {{1}, {1}, simulation::detail::TEndpointSignatureStorage<void()>::view(), this,
                [](void* self, void* context, ScriptHookLane lane) noexcept {
                    auto& endpoint = *static_cast<Endpoint*>(self);
                    assert(!endpoint.lane);
                    endpoint.context = context; endpoint.lane = lane;
                    return EndpointConnectResult{{0, 1}, EEndpointMutationError::NONE};
                },
                [](void* self, EndpointConnectionToken) noexcept {
                    static_cast<Endpoint*>(self)->lane = nullptr;
                    return EEndpointMutationError::NONE;
                },
                [](void*, const void*) noexcept {},
                [](void* self) noexcept { return static_cast<Endpoint*>(self)->lane != nullptr; }
            };
        }
        void invoke() noexcept
        {
            assert(lane);
            lux_script_call_frame frame{};
            lane(context, frame);
        }
    };
    template <class T> void require(const T& result, const char* stage)
    {
        if (!result)
        {
            std::cerr << stage << " failed, code=";
            if constexpr (std::is_enum_v<std::remove_cvref_t<decltype(result.error())>>)
                std::cerr << static_cast<unsigned>(result.error());
            else
                std::cerr << static_cast<unsigned>(result.error().code);
            std::cerr << '\n';
            std::abort();
        }
    }
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    std::ifstream input{argv[1], std::ios::binary};
    assert(input);
    const std::string source{std::istreambuf_iterator<char>{input}, {}};
    std::vector<std::byte> payload(source.size());
    std::memcpy(payload.data(), source.data(), source.size());
    const std::array contributions{
        assetAbilityLua(), skeletonAbilityLua(),
        lux::script::lua::makeScriptAbilityLuaContribution<DelayAbility>()
    };
    rdesc::Script description;
    description.module_name = "ec2.assets";
    description.exports = {{"run", 1, {}, {}}};
    description.body = rdesc::LuaSourceScript{"EC2Assets", {1}};
    for (const auto& ability : contributions)
        description.api_requirements.push_back({lux::script::ScriptApiContractId{ability.description->id.name()},
            ability.description->schema_hash});
    auto artifact = lux::script::ScriptArtifact::create(std::move(description), std::move(payload));
    require(artifact, "artifact");
    auto requirement = describeLuaPreparedRequirements(artifact->description(), contributions);
    require(requirement, "requirements");
    assert(requirement->ability_methods == 23);

    using Scalar = semantic::TTypeTraits<std::int32_t>;
    const ScriptHostComponentContract component{
        123, semantic::typeId(Scalar::CanonicalName), Scalar::CanonicalName, Scalar::AbiKind,
        sizeof(std::int32_t), alignof(std::int32_t)
    };
    const std::array lua_components{LuaComponentBinding{"Counter", component.component_type,
        component.semantic_type, std::string{component.canonical_name}, component.abi_kind, component.size,
        component.alignment}};
    const std::array deferred{scriptDeferredComponent<std::int32_t>(component)};
    const std::array blocks{LuaPreparedBlockClass{23, 2}};
    LuaScriptBackendConfig config;
    config.instance_capacity = 2; config.prepared_call_capacity = 4;
    config.continuation_capacity = 4; config.execution_depth_capacity = 4;
    config.ability_catalog_method_capacity = 23; config.prepared_ability_capacity = 46;
    config.components = lua_components; config.abilities = contributions;
    config.prepared_ability_blocks = blocks; config.prepared_ability_storage_bytes = 65536;
    config.prepared_event_capacity = 0; // This script declares no event source.
    auto backend = LuaScriptBackend::create(config);
    require(backend, "Lua backend");
    // Use the real backend's cold admission, preserving every untouched generated entry.
    // A mismatched codec or absent bounded resume transport must never reach a provider.
    const auto skeleton_projection = skeletonAbilityLua();
    std::vector<lux::script::lua::ScriptAbilityLuaMethodProjection> methods{
        skeleton_projection.methods.begin(), skeleton_projection.methods.end()
    };
    assert(methods.front().results.size() == 1);
    auto operation = methods.front().results.front();
    methods.front().results = std::span{&operation, 1U};
    auto rejected_contributions = contributions;
    rejected_contributions[1].methods = methods;
    auto rejected_config = config;
    rejected_config.abilities = rejected_contributions;
    ++operation.representation;
    auto wrong_representation = LuaScriptBackend::create(rejected_config);
    assert(!wrong_representation && wrong_representation.error() ==
        ELuaScriptBindingBackendError::INVALID_VALUE_OPERATION);
    operation = skeleton_projection.methods.front().results.front();
    operation.push_resume = nullptr;
    auto missing_resume = LuaScriptBackend::create(rejected_config);
    assert(!missing_resume && missing_resume.error() == ELuaScriptBindingBackendError::INVALID_VALUE_OPERATION);
    operation = skeleton_projection.methods.front().results.front();
    ++operation.size;
    auto wrong_layout = LuaScriptBackend::create(rejected_config);
    assert(!wrong_layout && wrong_layout.error() == ELuaScriptBindingBackendError::INVALID_VALUE_OPERATION);
    const std::array backends{backend->descriptor()};

    const asset::AssetId id{*uuids::uuid::from_string("fedcba98-7654-4abc-ffff-ffffffffffff")};
    auto skeleton = std::make_shared<rdesc::Skeleton>();
    skeleton->bones = {{"root", -1, Eigen::Affine3f::Identity(), Eigen::Affine3f::Identity()},
        {"child", 0, Eigen::Affine3f::Identity(), Eigen::Affine3f::Identity()}};
    auto owned_asset = asset::SkeletonAsset::create({id, asset::SkeletonAsset::asset_type}, skeleton);
    require(owned_asset, "skeleton");
    auto encoded = asset::TAssetSerDeser<asset::SkeletonAsset>::encode(**owned_asset, asset::AssetEncodeLimits{8192});
    assert(encoded);
    auto owner = std::make_shared<const std::vector<std::byte>>(std::move(*encoded));
    auto image = cxx::SharedBytes<>::fromOwner(owner, std::span<const std::byte>{*owner});
    auto execution = process::ExecutionRuntime::create({1, 64, 64, {32}, process::BlockingSchedulerConfig{1, 64}});
    require(execution, "execution");
    process::TaskScope files{*execution};
    asset::AssetVfs vfs;
    auto blocking = execution->blocking(); assert(blocking);
    auto endpoint = process::asset_loading::VfsAssetReadEndpoint::create(vfs.view().capture(), *blocking, files, {4});
    assert(endpoint);
    auto read = process::asset_loading::makeAssetReadOverlay({{id, {image}}}, (*endpoint)->port());
    assert(read);
    const std::array hook_specs{makeHookPointSpec<void()>({1}, "tick", true, true)};
    const SimulationSystemDescription test_system{{.canonical_name = "lux.ec2.AssetProbe", .version = 1}, hook_specs};
    SimulationDescriptionBuilder builder;
    assert(builder.addSystem({1}, "tick", test_system));
    auto simulation = std::move(builder).build(); assert(simulation);
    const ScriptRuntimeLimits limits{8, 1, 4, 4, 4, 4, 256, 4, 4, 4, 4, 4};
    const std::array capacities{ecs::EcsCommandProducerCapacity{16, 4096}};
    for (int stop_window = 0; stop_window != 3; ++stop_window)
    {
        auto counted = std::make_shared<ReadCount>(*read);
        auto access = ScriptAssetAccess::create(*execution, process::asset_loading::AssetReadPort{counted},
            {1, 2, 49152, {8192, 16384, 8}});
        require(access, "asset access");
        ecs::Registry registry;
        const auto entity = registry.create(); registry.emplace<std::int32_t>(entity, 7);
        ecs::EcsCommandBuffer commands; assert(commands.prepare(capacities));
        DeferredScriptHost host{registry, deferred};
        SimulationTime time;
        const std::array mounts{ScriptRuntimeMount{{1}, id, EntityScriptScope{entity}, {{1, HookScriptTarget{{1}, {1}}}}}};
        auto capacity = planScriptRuntimeCapacity(mounts); require(capacity, "capacity");
        Endpoint tick;
        const std::array hooks{tick.descriptor()};
        const std::array publications{publishAssetAbility(**access), publishSkeletonAbility(**access)};
        const ScriptArtifactResolver resolver{
            &*artifact, [](void* context, const asset::AssetId&, ResolvedScriptArtifact& result) noexcept {
                result = {static_cast<lux::script::ScriptArtifact*>(context), context, [](void*) noexcept {}};
                return true;
            }
        };
        auto system = ScriptSystem::create(*simulation, *capacity, mounts, registry, time, limits, resolver,
            publications, backends, hooks, {}, host.api());
        require(system, "ScriptSystem");
        require(system->prepare(), "prepare");
        const auto status = system->queryMountStatus({1}); assert(status && *status);
        auto scope = (*access)->prepare((**status).instance); assert(scope);
        auto alias = (*access)->prepare((**status).instance); assert(alias && alias->get() == scope->get());
        const auto step = [&](bool invoke) {
            const auto original = registry.get<std::int32_t>(entity);
            {
                auto writer = commands.begin(0, ecs::EEcsCommandPolicy::CONTINUE_ON_INVALID_TARGET); assert(writer);
                auto batch = host.begin(*writer); assert(batch);
                auto region = system->beginExecutionRegion(); require(region, "region");
                if (invoke) tick.invoke();
                auto report = system->executeStablePoint(); require(report, "stable point");
                if (!system->failures().empty())
                {
                    const auto failure = system->failures().back();
                    std::cerr << "script failure " << unsigned(failure.error) << " status " << failure.status << '\n';
                    std::abort();
                }
                assert(registry.get<std::int32_t>(entity) == original);
                require(region->finish(), "region finish");
            }
            assert(ecs::applyEcsCommands(registry, commands));
            require(system->processLifecycle(), "lifecycle");
            ++time.step_index; time.delta = std::chrono::milliseconds{16}; time.elapsed += time.delta;
        };
        step(true);
        assert(system->activeContinuationCount() == 1 && registry.get<std::int32_t>(entity) == 7);
        const auto settle = [&] {
            assert(execution->waitUntil([&]() noexcept {
                assert(execution->dispatchTaskEvents());
                (*access)->deliverCompletions();
                for (const auto& task : execution->taskInfos())
                    if (task.state == process::ETaskState::QUEUED || task.state == process::ETaskState::RUNNING)
                        return false;
                return !execution->hasPendingWork();
            }));
        };
        if (stop_window == 2) settle(); // accepted native result, VM has not resumed
        if (stop_window != 0)
        {
            require(system->requestStop(), "stop"); require(system->processLifecycle(), "retire");
            settle();
            assert(registry.get<std::int32_t>(entity) == 7);
        }
        else
        {
            for (int frame = 0; frame != 16 && system->activeContinuationCount() != 0; ++frame)
            {
                settle(); step(false);
            }
            assert(system->activeContinuationCount() == 0 && system->activeAwaitableCount() == 0);
            assert(registry.get<std::int32_t>(entity) == 9 && (*scope)->retainedResults() == 0);
            assert(counted->calls == 3); // One typed read, one missing image, one raw read; queries add none.
            const auto stats = backend->stats();
            std::cout << "EC2 Lua counts: reads=" << counted->calls
                << " ability_slots=" << stats.prepared_ability_slots
                << " ability_high_water=" << stats.prepared_ability_high_water
                << " coroutine_resumes=" << stats.vm_coroutine_resumes
                << " awaitables=" << system->activeAwaitableCount()
                << " retained=" << (*scope)->retainedResults() << '\n';
            require(system->requestStop(), "stop"); require(system->processLifecycle(), "retire");
        }
        require(system->shutdown(), "shutdown");
        assert(system->activeInstanceCount() == 0 && (*scope)->retainedResults() == 0);
        assert(backend->stats().prepared_ability_slots == 0);
    }
    std::cout << "EC2 generated Lua: typed codec + identity + Delay + original command barrier + missing asset + "
                 "raw bytes + release + stop-before-resume PASS\n";
}
