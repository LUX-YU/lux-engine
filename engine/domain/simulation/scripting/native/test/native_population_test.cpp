#include <lux/engine/simulation/scripting/native/NativeScriptBackend.hpp>

#include <array>
#include <cassert>
#include <cstdio>
#include <string_view>

namespace
{
    using namespace lux::simulation::script;

    struct Modules final
    {
        std::array<const lux::script::NativeModule*, 2U> values;
        static bool resolve(void* context, const lux::asset::AssetId& asset, const lux::script::ScriptArtifact&,
                            ResolvedNativeModule& result) noexcept
        {
            auto& self = *static_cast<Modules*>(context);
            const auto index = std::to_integer<std::uint8_t>(asset.bytes()[0]) - 1U;
            if (index >= self.values.size()) return false;
            result = {self.values[index], nullptr, nullptr};
            return true;
        }
    };

    lux::asset::AssetId assetId(std::uint8_t index)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = index + 1U;
        return lux::asset::AssetId{bytes};
    }

    lux::script::ScriptArtifact artifact(const lux::script::NativeModule& module)
    {
        lux::rdesc::Script description;
        description.module_name = module.name();
        description.body = lux::rdesc::NativeModuleScript{
            module.abiVersion(), module.stateLayoutHash(), module.stateSize(), module.stateAlignment(), {}
        };
        description.exports.push_back({"Read", 1U, {}, {lux::rdesc::makeScriptValueType<std::uint32_t>()}});
        if (module.functions().size() > 1U)
        {
            description.exports.push_back({"Small", 2U, {}, {}});
            description.exports.push_back({"Large", 3U, {}, {}});
        }
        auto result = lux::script::ScriptArtifact::create(std::move(description), {});
        assert(result);
        return std::move(*result);
    }

    bool exercise(const lux::script::NativeModule& a, const lux::script::NativeModule& b,
                  bool reversed, bool frames, bool zero_coroutines)
    {
        Modules modules{{&a, &b}};
        std::array populations{
            NativeScriptStoragePopulation{&a, 1U, frames ? 2U : 0U},
            NativeScriptStoragePopulation{&b, 2U, frames ? 1U : 0U}
        };
        if (reversed) std::swap(populations[0], populations[1]);
        NativeScriptBackendConfig config{
            .module_capacity = 2U, .instance_capacity = 4U, .prepared_call_capacity = 4U,
            .continuation_capacity = zero_coroutines ? 0U : 3U, .max_ability_imports_per_module = 1U,
            .max_continuation_frame_bytes = zero_coroutines ? 0U : 1024U,
            .continuation_frame_storage_bytes = zero_coroutines ? 0U : 16384U,
            .continuation_frame_storage_alignment = 16U,
            .storage_populations = populations, .state_storage_bytes = 16384U
        };
        NativeScriptBackend backend{{&modules, &Modules::resolve}, config};
        if (!backend)
        {
            std::fprintf(stderr, "backend construction rejected zero_coroutines=%d\n", zero_coroutines);
            return false;
        }
        auto first_artifact = artifact(a);
        auto second_artifact = artifact(b);
        auto api = backend.descriptor();
        std::array<ScriptBackendInstance, 3U> instances{};
        const auto create = [&](std::size_t slot, std::uint8_t module) {
            return api.createInstance(api.context, {assetId(module), SimulationScriptScope{}, nullptr, {1U, 1U}},
                module == 0U ? first_artifact : second_artifact, instances[slot]);
        };
        // B must not consume A's same-stride class. A remains available afterwards.
        for (std::size_t slot{}; slot < instances.size(); ++slot)
        {
            const auto status = create(slot, slot == 2U ? 0U : 1U);
            if (status != EScriptBackendResult::SUCCESS)
            {
                std::fprintf(stderr, "state mapping failed: reversed=%d slot=%zu result=%u\n",
                    reversed, slot, static_cast<unsigned>(status));
                for (std::size_t previous{}; previous < slot; ++previous)
                    api.destroyInstance(api.context, instances[previous]);
                return false;
            }
        }
        assert(backend.stats().active_states == 3U);
        assert(backend.stats().state_reserved_slots == 3U && backend.stats().state_live_bytes == 96U);
        ScriptBackendInstance overflow;
        assert(api.createInstance(api.context, {assetId(1U), SimulationScriptScope{}, nullptr},
            second_artifact, overflow) == EScriptBackendResult::CAPACITY_EXCEEDED);
        assert(!overflow && backend.stats().state_capacity_failures == 1U);
        if (frames)
        {
            std::array<ScriptBackendPreparedMethod, 2U> calls{};
            assert(api.prepareMethod(api.context, instances[2], *first_artifact.findExport(2U), calls[0]) ==
                EScriptBackendResult::SUCCESS);
            assert(api.prepareMethod(api.context, instances[0], *second_artifact.findExport(2U), calls[1]) ==
                EScriptBackendResult::SUCCESS);
            ScriptStepContext step{{1U, 1U}, nullptr, nullptr, nullptr};
            lux_script_call_frame frame{};
            std::array<ScriptBackendContinuation, 3U> continuations{};
            for (std::size_t index{}; index < continuations.size(); ++index)
            {
                const auto& call = calls[index == 2U ? 1U : 0U].resumable;
                const auto result = call.invoke(call.context, frame, step, continuations[index]);
                if (result.state != EScriptStepState::SUSPENDED)
                {
                    std::fprintf(stderr, "frame mapping failed: reversed=%d invocation=%zu\n", reversed, index);
                    for (std::size_t previous{}; previous < index; ++previous)
                        continuations[previous].destroy(continuations[previous].state);
                    for (std::size_t i{}; i < calls.size(); ++i)
                        api.releaseMethod(api.context, instances[i == 0U ? 2U : 0U], calls[i]);
                    for (auto instance : instances) api.destroyInstance(api.context, instance);
                    return false;
                }
            }
            assert(backend.stats().active_frames == 3U && backend.stats().heap_frame_allocations == 0U);
            assert(backend.stats().frame_reserved_slots == 3U);
            const auto occupied = 2U * a.findFunction(3U)->step->frame_size + b.findFunction(3U)->step->frame_size;
            assert(backend.stats().frame_live_bytes == 384U && backend.stats().frame_occupied_bytes == occupied);
            ScriptBackendContinuation over_capacity;
            const auto& full_call = calls[0].resumable;
            assert(full_call.invoke(full_call.context, frame, step, over_capacity).state == EScriptStepState::FAILED);
            assert(!over_capacity);
            for (auto continuation : continuations) continuation.destroy(continuation.state);
            assert(backend.stats().active_frames == 0U);
            assert(backend.stats().frame_live_bytes == 0U && backend.stats().frame_occupied_bytes == 0U);
            const auto& recycled_call = calls[0].resumable;
            ScriptBackendContinuation recycled;
            const auto recycled_result = recycled_call.invoke(recycled_call.context, frame, step, recycled);
            assert(recycled_result.state == EScriptStepState::SUSPENDED);
            recycled.destroy(recycled.state);

            ScriptBackendPreparedMethod read_method;
            assert(api.prepareMethod(api.context, instances[2], *first_artifact.findExport(1U), read_method) ==
                EScriptBackendResult::SUCCESS);
            std::uint32_t observed{};
            lux_script_value_slot output{LUX_SCRIPT_VK_UINT32, {}, sizeof(observed),
                lux::semantic::typeId("lux.u32"), &observed};
            lux_script_call_frame read_frame{};
            read_frame.user_context = read_method.synchronous.context;
            read_frame.return_count = 1U;
            read_frame.returns = &output;
            assert(read_method.synchronous.invoke(&read_frame) == 0 && observed == 6U);
            api.releaseMethod(api.context, instances[2], read_method);
            for (std::size_t i{}; i < calls.size(); ++i)
                api.releaseMethod(api.context, instances[i == 0U ? 2U : 0U], calls[i]);
        }
        else assert(backend.stats().frame_storage_bytes == 0U);
        for (auto instance : instances) api.destroyInstance(api.context, instance);
        assert(backend.stats().active_states == 0U);
        assert(backend.stats().state_live_bytes == 0U);
        assert(create(0U, 1U) == EScriptBackendResult::SUCCESS);
        api.destroyInstance(api.context, instances[0]);
        if (&a != &b)
        {
            // Unknown exact state layout or executable frame envelope must fail at preparation.
            const std::array restricted{
                NativeScriptStoragePopulation{frames ? &b : &a, 3U, frames ? 3U : 0U}
            };
            config.storage_populations = restricted;
            NativeScriptBackend limited{{&modules, &Modules::resolve}, config};
            assert(limited);
            auto limited_api = limited.descriptor();
            ScriptBackendInstance unknown;
            assert(limited_api.createInstance(limited_api.context,
                {assetId(frames ? 0U : 1U), SimulationScriptScope{}, nullptr},
                frames ? first_artifact : second_artifact, unknown) == EScriptBackendResult::CAPACITY_EXCEEDED);
            assert(!unknown && limited.stats().active_states == 0U);
        }
        std::printf("population: reverse=%d frames=%d success=3 recycled=1 state_bytes=%zu frame_bytes=%zu\n",
            reversed, frames, backend.stats().state_storage_bytes, backend.stats().frame_storage_bytes);
        return true;
    }
}

int main(int argc, char** argv)
{
    assert(argc == 6);
    const std::string_view scenario{argv[1]};
    auto a = lux::script::loadNativeModule(std::filesystem::path{argv[2]});
    auto b = lux::script::loadNativeModule(std::filesystem::path{argv[3]});
    auto wide = lux::script::loadNativeModule(std::filesystem::path{argv[4]});
    auto narrow = lux::script::loadNativeModule(std::filesystem::path{argv[5]});
    assert(a && b && wide && narrow);
    const bool shared = scenario == "shared";
    const bool frames = scenario == "frames" || shared;
    const bool sync = scenario == "sync";
    for (bool reversed : {false, true})
        if (!exercise(frames ? *wide : *a, shared ? *wide : frames ? *narrow : *b, reversed, frames, sync)) return 1;
    return 0;
}
