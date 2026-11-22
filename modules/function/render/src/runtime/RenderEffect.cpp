#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/graph/compile/EffectCompileRequest.hpp>
#include <lux/engine/render/graph/compile/GraphCompiler.hpp>
#include <lux/engine/render/effect/EffectGraphBuild.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>
#include <utility>

namespace lux::render
{
    EffectCreated RenderServer::Impl::createEffect(CreateEffect::Request&& request) noexcept
    {
        if (!request.graph) return EffectCreated{RenderError{kCapabilityDeclaration}};
        if (auto valid = validateEffectOutput(request.output); !valid)
            return EffectCreated{valid.error()};
        RenderEffect candidate;
        candidate.output = request.output;
        auto initialized = initializeEffectGraph(
            candidate, std::move(request.graph),
            {effect_catalog_, runtime_->command_registry_, config_.max_config_bytes, config_.diagnostics}
        );
        if (!initialized) return EffectCreated{initialized.error()};
        return EffectCreated{{}, runtime_->effects_.emplace(std::move(candidate))};
    }

    exec::task<AckResponse> RenderServer::Impl::prepareEffect(PrepareEffect::Request&& input) noexcept
    {
        auto* effect = runtime_->effects_.find(input.effect);
        auto* device = runtime_->device(input.device);
        if (!effect || !device) co_return AckResponse{RenderError{kRuntimeIdentity}};
        if (const auto error = device->admissionError(); error.type) co_return AckResponse{error};
        if (stop_requested_.load()) co_return AckResponse{RenderError{kCommandCancelled}};
        if (effect->graphForDevice(input.device)) co_return AckResponse{};
        if (!effect->preparing_device.isNull()) co_return AckResponse{RenderError{kDeviceBusy}};

        effect->preparing_device = input.device;
        cxx::scope_exit finish([effect]() noexcept { effect->preparing_device = {}; });
        detail::EffectCompileRequest request{
            input.effect, {*effect->source, effect->node_configurations}, {}, effect->output
        };
        request.inputs.reserve(effect->node_states.size());
        for (const auto& state : effect->node_states) request.inputs.push_back({state.node, state.type, state.bytes});
        cxx::scope_exit release_worker([this]() noexcept { recording_workers_.endCompilation(); });
        co_await recording_workers_.beginCompilation();
        if (stop_requested_.load()) co_return AckResponse{RenderError{kCommandCancelled}};
        if (const auto error = device->admissionError(); error.type) co_return AckResponse{error};
        auto compiled = co_await detail::GraphCompiler::compileEffect(
            *device, std::move(request), {effect_catalog_, program_scheduler_, config_.diagnostics, stop_requested_}
        );
        if (!compiled) co_return AckResponse{compiled.error()};
        cxx::scope_exit discard([&]() noexcept { device->removeUniformPools(input.effect); });
        if (stop_requested_.load()) co_return AckResponse{RenderError{kCommandCancelled}};
        if (const auto error = device->admissionError(); error.type) co_return AckResponse{error};
        effect->device_graphs.push_back({input.device, std::move(*compiled)});
        discard.release();
        compiled_graphs_.fetch_add(1, std::memory_order_relaxed);
        co_return AckResponse{};
    }

    void RenderServer::Impl::listEffectJobs(const EffectRequest& input, ListEffectJobs::Response& result) noexcept
    {
        if (!runtime_->effects_.find(input.effect))
        {
            result.error = {kRuntimeIdentity};
            return;
        }
        for (const auto& job : runtime_->jobs_)
            if (job.content.effect == input.effect) result.jobs.push_back(job.control.id());
        result.error = {};
    }

    void RenderServer::Impl::bindEffectGraphCommands() noexcept
    {
        bindFrameworkCommand<CreateEffect, &Impl::createEffect>();
        bindFrameworkCommand<PrepareEffect, &Impl::prepareEffect>();
        bindFrameworkCommand<ListEffectJobs, &Impl::listEffectJobs>();
    }

    void RenderServer::Impl::listEffectCapabilities(const EmptyRequest&, ListEffectCapabilities::Response& output)
        noexcept
    {
        const auto definitions = effect_catalog_.listCapabilities();
        output.capabilities.reserve(definitions.size());
        for (const auto& definition : definitions)
        {
            const auto& identity = definition.identity;
            output.capabilities.push_back({definition, effect_catalog_.listImplementations(identity.id, identity.version)});
        }
        output.error = {};
    }

    void RenderServer::Impl::removeEffect(const EffectRequest& input, AckResponse& output) noexcept
    {
        const auto* effect = runtime_->effects_.find(input.effect);
        if (!effect)
        {
            output.error = {kRuntimeIdentity};
            return;
        }
        const bool referenced = std::ranges::any_of(runtime_->jobs_, [&](const auto& job) noexcept
        {
            return job.content.effect == input.effect;
        });
        if (!effect->preparing_device.isNull() || referenced)
        {
            output.error = {kDeviceBusy};
            return;
        }

        runtime_->effects_.erase(input.effect);
        for (auto& device : runtime_->devices_) device->removeUniformPools(input.effect);
        output = {};
    }
} // namespace lux::render
