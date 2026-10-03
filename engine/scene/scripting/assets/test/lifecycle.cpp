#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/scripting/cpp_static/CppStaticScriptBridge.hpp>

#include <cassert>
#include <memory>

namespace
{
    using namespace lux;
    using namespace lux::simulation::script;
    struct Counts final
    {
        int prepared{}, revoked{}, released{}, constructed{}, destroyed{}, artifact_released{};
        bool reject{};
        ScriptInstanceId identity;
        ScriptSystem* system{};
    };
    Counts* observed{};
    struct Probe final
    {
        Probe() noexcept { ++observed->constructed; }
        ~Probe() noexcept
        {
            assert(observed->revoked == 1 && observed->released == 0);
            ++observed->destroyed;
        }
        void begin() noexcept { assert(observed->prepared == 1 && observed->revoked == 0); }
    };
    struct Scope final
    {
        explicit Scope(Counts& counts) noexcept : counts(counts) {}
        ~Scope() noexcept
        {
            assert(counts.revoked == 1 && counts.destroyed == counts.constructed);
            ++counts.released;
        }
        Counts& counts;
    };
    ScriptApiInstanceResult prepare(void* context, ScriptInstanceId instance) noexcept
    {
        auto& counts = *static_cast<Counts*>(context);
        assert(instance.valid());
        if (counts.reject)
            return cxx::unexpected(EScriptApiPrepareError::CAPACITY_EXCEEDED);
        counts.identity = instance;
        ++counts.prepared;
        auto owner = std::make_shared<Scope>(counts);
        auto* address = owner.get();
        return ScriptApiInstanceBinding::create(std::move(owner), address, [](void* context) noexcept {
            auto& counts = static_cast<Scope*>(context)->counts;
            assert(counts.destroyed == 0 && counts.released == 0);
            const auto nested = counts.system->processLifecycle();
            assert(!nested && nested.error() == EScriptSystemError::ENDPOINT_BUSY);
            ++counts.revoked;
        });
    }
}

void qualifyInstanceBindings()
{
    using Entry = detail::TCppStaticSyncEntry<&Probe::begin>;
    const std::array exports{CppStaticExportEntry{1, "begin", Entry::Parameters, Entry::Results, &Entry::invoke}};
    const script::ScriptApiContractIdView contract_id{"lux.ec2.test.instance"};
    const std::array requirements{CppStaticApiRequirement{contract_id, 1}};
    const CppStaticContract contract{
        "ec2.instance", "ec2.instance.contract", false, detail::cppStaticObject<Probe>(), exports, {1, 0}, requirements,
        {}, [](std::uint64_t hash, std::uint32_t& slot) noexcept {
            slot = 0;
            return hash == script::ScriptApiContractIdView{"lux.ec2.test.instance"}.hash();
        }
    };
    auto description = materializeCppStaticScript(contract);
    assert(description);
    auto artifact = script::ScriptArtifact::create(std::move(*description), {});
    assert(artifact);
    const std::array pools{CppStaticScriptPoolDescription{&contract, 1, 0, 0, alignof(std::max_align_t), 2}};
    auto backend = CppStaticScriptBackend::create(pools);
    assert(backend);
    const std::array backends{backend->descriptor()};
    auto simulation = simulation::SimulationDescriptionBuilder{}.build();
    assert(simulation);
    simulation::ecs::Registry registry;
    simulation::SimulationTime time;
    const asset::AssetId id{*uuids::uuid::from_string("491f06e3-8618-4dfe-ae93-4c06b2b0e172")};
    const std::array mounts{ScriptRuntimeMount{{1}, id, SimulationScriptScope{}}};
    auto capacity = planScriptRuntimeCapacity(mounts);
    assert(capacity);
    const ScriptRuntimeLimits limits{8, 1, 4, 4, 4, 4, 256, 4, 4, 4, 4, 4};
    struct Source final
    {
        script::ScriptArtifact& artifact;
        Counts& counts;
    };
    for (const bool reject : {false, true})
    {
        Counts counts;
        counts.reject = reject;
        observed = &counts;
        Source source{*artifact, counts};
        const ScriptArtifactResolver resolver{
            &source, [](void* context, const asset::AssetId&, ResolvedScriptArtifact& result) noexcept {
                auto& source = *static_cast<Source*>(context);
                result = {&source.artifact, &source.counts, [](void* context) noexcept {
                    auto& counts = *static_cast<Counts*>(context);
                    assert(counts.released == counts.prepared && counts.destroyed == counts.constructed);
                    ++counts.artifact_released;
                }};
                return true;
            }
        };
        const std::array publications{
            ScriptApiCapabilityPublication{contract_id, 1, &counts, &counts, 1, {}, &prepare}
        };
        auto system = ScriptSystem::create(*simulation, *capacity, mounts, registry, time, limits, resolver,
            publications, backends, {}, {});
        assert(system);
        counts.system = &*system;
        auto ready = system->prepare();
        if (reject)
        {
            assert(!ready && ready.error() == EScriptSystemError::CAPACITY_EXCEEDED);
            assert(counts.prepared == 0 && counts.constructed == 0 && counts.artifact_released == 1);
        }
        else
        {
            assert(ready && system->activeInstanceCount() == 1);
            assert(counts.prepared == 1 && counts.identity.valid() && counts.constructed == 1);
            assert(system->requestStop());
            assert(system->processLifecycle(EScriptLifecycleAdmission::RETIRE_ONLY));
            assert(counts.revoked == 1 && counts.released == 1 && counts.destroyed == 1);
            assert(counts.artifact_released == 1 && system->activeInstanceCount() == 0);
        }
        assert(system->shutdown());
    }
    observed = nullptr;
}
