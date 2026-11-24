#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/graph/compile/RenderGraphCompiler.hpp>
#include <lux/engine/render/effect/EffectImplementation.hpp>
#include <lux/engine/render/runtime/commands/CommandRegistry.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>
#include <utility>

namespace lux::render
{
    namespace
    {
        using effect_detail::ValidatedEffectGraph;

        [[nodiscard]] RenderResult<ValidatedEffectGraph>
        validateForBuild(const EffectGraph& source, const EffectCatalog& catalog, const RenderErrorSink& sink) noexcept
        {
            auto result = effect_detail::validateEffectGraph(source, catalog);
            if (result)
            {
                return std::move(*result);
            }
            const auto code = result.error().issue == EEffectIssue::CYCLE ? kCapabilityCycle : kCapabilityDeclaration;
            return cxx::unexpected(effect_detail::reportEffectFailure(sink, {code}, result.error(), "Effect graph validation failed"));
        }
    } // namespace

    EffectCreated RenderServer::Impl::createEffect(CreateEffect::Request&& request) noexcept
    {
        if (!request.graph) return EffectCreated{RenderError{kCapabilityDeclaration}};
        if (auto valid = validateEffectOutput(request.output); !valid)
            return EffectCreated{valid.error()};
        RenderEffect candidate;
        candidate.output = request.output;
        auto max_config_bytes = config_.max_config_bytes;
        const auto& sink = config_.diagnostics;
        const auto& registry = runtime_->command_registry_;
        for (const auto& [id, node] : request.graph->nodes())
        {
            if (node.arguments.size() > max_config_bytes)
                return EffectCreated{RenderError{kInvalidConfig, {id.value}}};
            max_config_bytes -= node.arguments.size();
        }
        auto validated = validateForBuild(*request.graph, effect_catalog_, sink);
        if (!validated) return EffectCreated{validated.error()};
        for (auto index : validated->topological_order)
        {
            auto& node = validated->nodes[index];
            if (!node.live) continue;
            auto created = node.implementation->create(node.source, request.graph->findNode(node.source)->arguments);
            if (!created || !*created)
            {
                const auto error = created ? RenderError{kCapabilityDeclaration} : created.error();
                return EffectCreated{effect_detail::reportEffectFailure(
                    sink, error, {EEffectIssue::INITIALIZATION_FAILURE, node.source, {}, {}}, "Effect implementation creation failed"
                )};
            }
            node.configured = std::move(*created);
            const auto bytes = node.configured->configurationBytes();
            if (bytes > max_config_bytes)
            {
                return EffectCreated{effect_detail::reportEffectFailure(
                    sink, {kInvalidConfig, {node.source.value}}, {EEffectIssue::INITIALIZATION_FAILURE, node.source, {}, {}},
                    "Effect implementation configuration exceeds the retained byte budget"
                )};
            }
            max_config_bytes -= bytes;
            const auto state = node.configured->state();
            for (auto binding : registry.effectBindings(*node.implementation))
            {
                const auto* command = registry.findCommand(binding.command);
                const auto* method = registry.binding(binding);
                const bool is_missing = !command || !method || !state.object;
                if (is_missing) return EffectCreated{RenderError{kCommandContract}};
                const bool is_wrong_route = !command->effect || !command->effect_node || command->storage;
                const bool is_wrong_state = method->effect_state_type != state.type ||
                    method->effect_state_type.name() != state.type.name() || !method->handler.isNull();
                if (is_wrong_route || is_wrong_state || !method->call.invoke)
                    return EffectCreated{RenderError{kCommandContract, {node.source.value, commandTag(binding.command)}}};
                auto call = method->call;
                call.object = state.object;
                candidate.command_routes.push_back({binding.command, call, node.source});
            }
        }
        auto& routes = candidate.command_routes;
        std::ranges::sort(routes, {}, &RenderEffect::CommandRoute::key);
        if (std::ranges::adjacent_find(routes, std::ranges::equal_to{}, &RenderEffect::CommandRoute::key) != routes.end())
            return EffectCreated{RenderError{kCapabilityConflict}};
        candidate.graph = std::move(*validated);
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
        detail::EffectCompileConfig compile_config{
            {effect->output.format, effect->output.samples},
            runtime_->resources_, device->uploader() != nullptr, {}
        };
        if (effect->output.kind == ETargetKind::SURFACE) compile_config.target_final_use = EGraphUse::PRESENT;
        else if (effect->output.sampled) compile_config.target_final_use = EGraphUse::SAMPLED;
        cxx::scope_exit release_worker([this]() noexcept { recording_workers_.endCompilation(); });
        co_await recording_workers_.beginCompilation();
        if (stop_requested_.load()) co_return AckResponse{RenderError{kCommandCancelled}};
        if (const auto error = device->admissionError(); error.type) co_return AckResponse{error};
        detail::RenderGraphCompiler compiler{
            device->session(), {recording_workers_.programScheduler(), config_.diagnostics, stop_requested_, device->failure}
        };
        auto compiled = co_await compiler.compile(effect->graph, std::move(compile_config));
        if (!compiled) co_return AckResponse{compiled.error()};
        if (stop_requested_.load()) co_return AckResponse{RenderError{kCommandCancelled}};
        if (const auto error = device->admissionError(); error.type) co_return AckResponse{error};
        device->installEffectResources(input.effect, compiled->constant_buffers);
        effect->device_graphs.emplace(input.device, std::move(*compiled));
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
            if (job.content.effect == input.effect) result.jobs.push_back(job.id());
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
            const auto& identity = definition;
            const auto commands = runtime_->command_registry_.nodeCommands(definition);
            output.capabilities.push_back({
                definition, effect_catalog_.listImplementations(identity.id, identity.version),
                std::vector<CommandId>(commands.begin(), commands.end())
            });
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
        const bool installing = std::ranges::any_of(runtime_->targets_, [&](const auto& target) noexcept
        {
            return targetState(target).pending_replacement == input.effect;
        });
        if (!effect->preparing_device.isNull() || referenced || installing)
        {
            output.error = {kDeviceBusy};
            return;
        }

        runtime_->effects_.erase(input.effect);
        for (auto& device : runtime_->devices_) device->removeEffectResources(input.effect);
        output = {};
    }
} // namespace lux::render
