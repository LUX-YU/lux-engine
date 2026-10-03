#include "PluginProbe.hpp"
#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/process/asset_loading/AssetReadOverlay.hpp>
#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/scripting/cpp_static/CppStaticScriptBridge.hpp>

#include <cassert>
#include <cstdio>

namespace
{
    using namespace lux;
    using namespace lux::simulation::script;
    namespace native = lux::scene::script;

    template <class T> auto take(T value)
    {
        assert(value);
        return std::move(*value);
    }

    struct Code final
    {
        engine::platform::DynamicLibrary library;
        PluginCounts& counts;
        Code(const char* file, PluginCounts& value) : library(file), counts(value) { assert(library.is_loaded()); }
        ~Code() noexcept
        {
            assert(counts.decoded == 1 && counts.destroyed == 1);
            assert(std::this_thread::get_id() == counts.owner);
            ++counts.unloaded;
        }
    };

    struct Probe final { void begin() noexcept {} };
    struct Binding final
    {
        native::ScriptAssetAccess& access;
        ScriptInstanceId instance;
    };
    struct Reply final
    {
        std::optional<native::ScriptAssetReadOutcome> value;
        unsigned calls{};
    };
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    auto execution = take(process::ExecutionRuntime::create({1, 64, 64, {32}, process::BlockingSchedulerConfig{1, 64}}));
    process::TaskScope files{execution};
    asset::AssetVfs vfs;
    auto endpoint = take(process::asset_loading::VfsAssetReadEndpoint::create(
        vfs.view().capture(), take(execution.blocking()), files, {4}
    ));
    const auto id = asset::AssetId{*uuids::uuid::from_string("fedcba98-7654-4abc-ffff-ffffffffffff")};
    auto bytes = std::make_shared<const std::vector<std::byte>>(16, std::byte{42});
    auto read = take(process::asset_loading::makeAssetReadOverlay(
        {{id, {cxx::SharedBytes<>::fromOwner(bytes, std::span<const std::byte>{*bytes})}}}, endpoint->port()
    ));
    using Entry = detail::TCppStaticSyncEntry<&Probe::begin>;
    const std::array exports{CppStaticExportEntry{1, "begin", Entry::Parameters, Entry::Results, &Entry::invoke}};

    for (const bool stop_before_delivery : {false, true})
    {
        PluginCounts counts;
        auto code = std::make_shared<Code>(argv[1], counts);
        auto* start = code->library.get_symbol<StartPluginRead>("ec2_start_asset_read");
        assert(start != nullptr);
        std::weak_ptr<Code> weak = code;
        auto access = take(native::ScriptAssetAccess::create(execution, read, {1, 1, 24576, {8192, 16384, 8}}));
        Binding binding{*access};
        auto publication = native::publishAssetAbility(*access);
        publication.context = &binding;
        publication.prepare_instance = [](void* context, ScriptInstanceId instance) noexcept {
            auto& binding = *static_cast<Binding*>(context);
            binding.instance = instance;
            return native::ScriptAssetAccess::prepareInstance(&binding.access, instance);
        };
        const std::array requirements{CppStaticApiRequirement{publication.contract, publication.schema_hash}};
        const CppStaticContract contract{
            "ec2.plugin", "ec2.plugin.contract", false, detail::cppStaticObject<Probe>(), exports,
            {1, 0}, requirements, {}, [](std::uint64_t hash, std::uint32_t& slot) noexcept {
                slot = 0;
                return hash == script::ScriptApiContractIdView{"lux.scene.assets"}.hash();
            }
        };
        auto artifact = take(script::ScriptArtifact::create(take(materializeCppStaticScript(contract)), {}));
        const std::array pools{CppStaticScriptPoolDescription{&contract, 1, 0, 0, alignof(std::max_align_t), 2}};
        auto backend = take(CppStaticScriptBackend::create(pools));
        const std::array backends{backend.descriptor()};
        const std::array publications{publication};
        auto simulation = take(simulation::SimulationDescriptionBuilder{}.build());
        simulation::ecs::Registry registry;
        simulation::SimulationTime time;
        const std::array mounts{ScriptRuntimeMount{{1}, id, SimulationScriptScope{}}};
        const ScriptRuntimeLimits limits{8, 1, 4, 4, 4, 4, 256, 4, 4, 4, 4, 4};
        const ScriptArtifactResolver source{
            &artifact, [](void* context, const asset::AssetId&, ResolvedScriptArtifact& result) noexcept {
                result = {static_cast<script::ScriptArtifact*>(context), context, [](void*) noexcept {}};
                return true;
            }
        };
        auto system = take(ScriptSystem::create(simulation, take(planScriptRuntimeCapacity(mounts)), mounts,
            registry, time, limits, source, publications, backends, {}, {}));
        assert(system.prepare());
        assert(binding.instance.valid());
        auto scope = take(access->prepare(binding.instance)); // The actual installed instance's original scope.
        auto reply = std::make_shared<Reply>();
        auto completion = native::ScriptAssetScope::Completion::fromErased(script::ScriptAbilityErasedCompletion::bind(
            reply, reply.get(), 0, 0,
            [](void* context, auto, auto, semantic::TypeId, const void* data, std::uint32_t size) noexcept
                -> script::ScriptAbilityErasedCompletion::CompletionResult {
                assert(size == sizeof(native::ScriptAssetReadOutcome));
                auto& reply = *static_cast<Reply*>(context);
                reply.value = *static_cast<const native::ScriptAssetReadOutcome*>(data);
                ++reply.calls;
                return {};
            },
            [](void*, auto, auto, script::ScriptAbilityOperationError) noexcept
                -> script::ScriptAbilityErasedCompletion::CompletionResult { std::terminate(); },
            [](void*, auto, auto) noexcept { return true; }
        ));
        const auto prior_tasks = execution.taskInfos();
        assert(start(*scope, id, std::move(completion), code, counts));
        code.reset(); // Task and retained result, not this test, must now pin all plugin code.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        bool finished{};
        while (!finished && std::chrono::steady_clock::now() < deadline)
        {
            for (const auto& task : execution.taskInfos())
            {
                const bool is_prior = std::ranges::any_of(prior_tasks, [&](const auto& prior) {
                    return prior.id == task.id;
                });
                if (!is_prior && task.state == process::ETaskState::SUCCEEDED)
                    finished = true;
            }
            std::this_thread::yield();
        }
        assert(finished);
        assert(counts.decoded == 1 && !weak.expired());
        if (stop_before_delivery)
        {
            assert(system.requestStop());
            assert(system.processLifecycle(EScriptLifecycleAdmission::RETIRE_ONLY));
        }
        assert(execution.waitUntil([&]() noexcept {
            assert(execution.dispatchTaskEvents());
            return !execution.hasPendingWork();
        }));
        assert(execution.dispatchTaskEvents());
        if (stop_before_delivery)
            assert(reply->calls == 0);
        else
        {
            assert(reply->calls == 1 && reply->value->succeeded());
            assert(counts.destroyed == 0 && !weak.expired());
            assert(system.requestStop());
            assert(system.processLifecycle(EScriptLifecycleAdmission::RETIRE_ONLY));
        }
        assert(system.activeInstanceCount() == 0);
        assert(scope->retainedResults() == 0 && scope->reservedBytes() == 0);
        assert(system.shutdown());
        access.reset();
        assert(counts.destroyed == 1 && counts.unloaded == 1 && weak.expired());
        assert(!counts.wrong_decode_thread && !counts.wrong_destroy_thread);
    }
    assert(files.join());
    std::puts("EC2 actual plugin codec/result: worker decode, owner destruction, ScriptSystem stop and final pin PASS");
}
