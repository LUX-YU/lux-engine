from pathlib import Path
import json
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
base=s/'engine/scene/scripting/assets_lua'
(base/'test/assets.lua').write_text('''---@lux.requires lux.scene.assets
---@lux.requires lux.scene.skeletons
---@lux.requires lux.simulation.delay
EC2Assets = {}

---@lux.method
---@lux.coroutine
---@return void
function EC2Assets:run()
    local id = lux.Assets.assetId(0xfedcba98, 0x76544abc, 0xffffffff, 0xffffffff)
    assert(lux.Assets.assetWord(id, 0) == 0xfedcba98)
    assert(lux.Assets.assetWord(id, 3) == 0xffffffff)
    local result = lux.Skeletons.read(id)
    assert(lux.Assets.succeeded(result))
    local handle = lux.Assets.handle(result)
    assert(lux.Skeletons.boneCount(handle) == 2)
    assert(lux.Skeletons.parentIndex(handle, 0) == -1)
    assert(lux.Skeletons.parentIndex(handle, 1) == 0)
    assert(lux.Skeletons.parentIndex(handle, 2) == -9)
    assert(lux.Assets.inspectionError(lux.Assets.describeAsset(handle)) == 0)
    assert(lux.Assets.assetWord(lux.Assets.inspectedId(lux.Assets.describeAsset(handle)), 3) == 0xffffffff)
    local before = self:get_component("Counter")
    lux.Delay.nextStep()
    assert(lux.Skeletons.boneCount(handle) == 2)
    assert(self:patch_component("Counter", before + 2))
    assert(self:get_component("Counter") == before) -- original deferred command barrier
    lux.Assets.releaseAsset(handle)
    lux.Assets.releaseAsset(handle) -- idempotent script release
    assert(lux.Assets.inspectionError(lux.Assets.describeAsset(handle)) == 4)
    local missing = lux.Assets.readAsset(lux.Assets.assetId(1, 2, 3, 4))
    assert(not lux.Assets.succeeded(missing))
    assert(lux.Assets.errorDomain(missing) == 1)
    assert(lux.Assets.errorCode(missing) ~= 0)
    local raw = lux.Assets.readAsset(id)
    assert(lux.Assets.succeeded(raw))
    local raw_handle = lux.Assets.handle(raw)
    local bytes = lux.Assets.copyAssetBytes(raw_handle, 0, 0, 4)
    assert(lux.Assets.bytesError(bytes) == 0 and lux.Assets.bytesCount(bytes) == 4)
    assert(lux.Assets.byteAt(bytes, 0) >= 0 and lux.Assets.byteAt(bytes, 4) == -1)
    assert(lux.Assets.bytesError(lux.Assets.copyAssetBytes(raw_handle, 0xffffffff, 0xffffffff, 1)) == 7)
    lux.Assets.releaseAsset(raw_handle)
end
''')
(base/'test/runtime.cpp').write_text(r'''#include <lux/engine/scene/scripting/AssetAbilityLua.hpp>
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
#include <fstream>
#include <iostream>
#include <thread>

using namespace lux;
using namespace lux::scene::script;
using namespace lux::simulation;
using namespace lux::simulation::script;

namespace
{
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
            std::cerr << stage << " failed, code=" << static_cast<unsigned>(result.error()) << '\n';
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
        script::lua::makeScriptAbilityLuaContribution<DelayAbility>()
    };
    rdesc::Script description;
    description.module_name = "ec2.assets";
    description.exports = {{"run", 1, {}, {}}};
    description.body = rdesc::LuaSourceScript{"EC2Assets", {1}};
    for (const auto& ability : contributions)
        description.api_requirements.push_back({script::ScriptApiContractId{ability.description->id.name()},
            ability.description->schema_hash});
    auto artifact = script::ScriptArtifact::create(std::move(description), std::move(payload));
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
    config.prepared_event_capacity = 1; config.prepared_event_storage_bytes = 4096;
    auto backend = LuaScriptBackend::create(config);
    require(backend, "Lua backend");
    const std::array backends{backend->descriptor()};

    const asset::AssetId id{*uuids::uuid::from_string("fedcba98-7654-4abc-ffff-ffffffffffff")};
    auto skeleton = std::make_shared<rdesc::Skeleton>();
    skeleton->bones = {{"root", -1, Eigen::Affine3f::Identity(), Eigen::Affine3f::Identity()},
        {"child", 0, Eigen::Affine3f::Identity(), Eigen::Affine3f::Identity()}};
    auto owned_asset = asset::SkeletonAsset::create({id, asset::SkeletonAsset::asset_type}, skeleton);
    require(owned_asset, "skeleton");
    auto encoded = asset::TAssetSerDeser<asset::SkeletonAsset>::encode(**owned_asset, {8192});
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
        auto access = ScriptAssetAccess::create(*execution, *read, {1, 2, 49152, {8192, 16384, 8}});
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
                result = {static_cast<script::ScriptArtifact*>(context), context, [](void*) noexcept {}};
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
            require(system->requestStop(), "stop"); require(system->processLifecycle(), "retire");
        }
        require(system->shutdown(), "shutdown");
        assert(system->activeInstanceCount() == 0 && (*scope)->retainedResults() == 0);
        assert(backend->stats().prepared_ability_slots == 0);
    }
    std::cout << "EC2 generated Lua: typed codec + identity + Delay + original command barrier + missing asset + "
                 "raw bytes + release + stop-before-resume PASS\n";
}
''')
p=base/'CMakeLists.txt';t=p.read_text();i=t.index('endif()',t.index('    set_tests_properties'))
t=t[:i]+'''    add_executable(scene_script_asset_runtime_test test/runtime.cpp)
    target_link_libraries(scene_script_asset_runtime_test PRIVATE scene_script_assets_lua simulation_script)
    get_target_property(_delay_generated simulation_script LUX_SCRIPT_ABILITY_GENERATED_DIR)
    target_include_directories(scene_script_asset_runtime_test PRIVATE ${_delay_generated})
    add_dependencies(scene_script_asset_runtime_test simulation_script_script_abilities_generate)
    if(MSVC)
        target_compile_options(scene_script_asset_runtime_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(scene_script_asset_runtime_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME scene.script_asset_lua COMMAND scene_script_asset_runtime_test ${CMAKE_CURRENT_SOURCE_DIR}/test/assets.lua)
    set_tests_properties(scene.script_asset_lua PROPERTIES TIMEOUT 45 LABELS "native;EC2")
'''+t[i:];p.write_text(t)
p=s/'editor/tests/architecture/rules.json';d=json.loads(p.read_text())
def find(d):
 if isinstance(d,dict):
  if 'scene_script_assets' in d:return d
  for x in d.values():
   f=find(x)
   if f is not None:return f
 return None
find(d)['scene_script_asset_runtime_test']={'layer':'TEST','role':'TEST','capabilities':['CPU','PROCESS'],'path':'engine/scene/scripting/assets_lua'}
p.write_text(json.dumps(d,ensure_ascii=False,indent=2)+'\n')
p=s/'engine/scene/scripting/assets/src/ScriptAssetAccess.cpp';t=p.read_text().replace('''            if (auto scope = state->scope.lock())''','''            if (state->stopping)
                return unexpected(EScriptAssetError::STOPPING);
            if (auto scope = state->scope.lock())''');p.write_text(t)
