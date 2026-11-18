#include <exec/variant_sender.hpp>
#include <limits>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>
#include <set>

namespace lux::render::runtime_detail
{
    RenderResult<void> RenderEffect::setParameters(EffectParameters parameters) noexcept
    {
        if (!parameters.valid())
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        if (parameters == parameters_)
        {
            return {};
        }
        if (parameter_revision_ == std::numeric_limits<std::uint64_t>::max())
        {
            return cxx::unexpected(RenderError{kRuntimeCapacity});
        }
        parameters_ = parameters;
        ++parameter_revision_;
        return {};
    }

    bool RenderEffect::tryBorrowExecution() noexcept
    {
        if (execution_borrows_ == UINT32_MAX)
        {
            return false;
        }
        ++execution_borrows_;
        return true;
    }

    void RenderEffect::releaseExecution() noexcept
    {
        if (execution_borrows_ == 0)
        {
            std::terminate();
        }
        --execution_borrows_;
    }
} // namespace lux::render::runtime_detail

namespace lux::render
{
    RenderResult<std::vector<RenderEffect::ResourceDependency>>
    RenderServer::Impl::qualifyEffectRecipe(
        const CreateEffectRequest& request, std::vector<CommandBindingLocation>& commands
    ) const noexcept
    {
        if (auto valid = validateEffectOutput(request.output); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        if (!request.parameters.valid())
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        std::set<std::string_view> keys;
        std::set<std::pair<std::size_t, std::string_view>> capabilities;
        std::set<std::pair<std::uint32_t, std::uint32_t>> resource_ids;
        std::vector<const CapabilityDescription*> declarations;
        std::vector<RenderEffect::ResourceDependency> dependencies;
        for (const auto& use : request.implementations)
        {
            const auto* implementation = capability_registry_.find(use.implementation);
            if (!implementation) return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (use.contribution.empty() || !keys.emplace(use.contribution).second)
                return cxx::unexpected(RenderError{kCapabilityDeclaration});
            if (auto error = capabilityOutputError(*implementation, request.output); error.type)
                return cxx::unexpected(error);

            for (const auto& binding : implementation->command_bindings)
            {
                const auto previous = std::ranges::find(commands, binding.command, &CommandBindingLocation::command);
                if (previous == commands.end()) commands.push_back(binding);
                else if (previous->binding != binding.binding)
                    return cxx::unexpected(RenderError{kCapabilityDeclaration});
            }
            const auto parent = capability_registry_.capabilityOf(use.implementation);
            const auto* declaration = capability_registry_.get(parent);
            if (!declaration) return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (!capabilities.emplace(declaration->type.hash(), declaration->type.name()).second)
            {
                continue;
            }
            declarations.push_back(declaration);
            for (auto type : capability_registry_.resources(parent))
            {
                if (!resource_ids.emplace(type.index, type.gen).second) continue;
                const auto* factory = domain_->resources_.factory(type);
                if (!factory) return cxx::unexpected(RenderError{kStorageIdentity});
                dependencies.push_back({factory->descriptor->domain, type, {}, {}});
            }
        }
        for (const auto* declaration : declarations)
        {
            for (const auto prerequisite : declaration->prerequisites)
            {
                if (!capabilities.contains({prerequisite.hash(), prerequisite.name()}))
                    return cxx::unexpected(RenderError{kCapabilityPrerequisite});
            }
        }
        return dependencies;
    }

    auto RenderServer::Impl::createEffect(CreateEffect::Request&& request) noexcept
    {
        std::vector<CommandBindingLocation> commands;
        auto dependencies = qualifyEffectRecipe(request, commands);
        auto error = dependencies ? RenderError{} : dependencies.error();
        if (!error.type && request.definition.features.empty()) error = {kFeatureContract};

        std::vector<const FeatureEntry*> registrations;
        std::vector<ResourceDeclaration> declarations;
        if (!error.type)
        {
            registrations.reserve(request.definition.features.size());
            for (const auto& selection : request.definition.features)
            {
                const auto* feature = features_.find(selection.feature);
                if (!feature)
                {
                    error = {kRuntimeIdentity};
                    break;
                }
                registrations.push_back(feature);
            }
        }
        if (!error.type)
        {
            domain_->resources_.forEachType(
                [&](ResourceTypeId id, const ResourceDefinition& definition) noexcept
                {
                    const auto* factory = domain_->resources_.factory(id);
                    const auto* bindings = static_cast<const detail::StorageBindingOps*>(factory->storage->binding_ops);
                    declarations.push_back({
                        id, definition.type, definition.domain, factory->frame_call.channels,
                        bindings && bindings->resolve
                    });
                }
            );
            std::ranges::sort(declarations, {}, [](const auto& type) { return type.name.name(); });
        }

        // The worker owns request values and borrows immutable registrations through Server delivery.
        auto compiled =
            stdexec::schedule(cpu_scheduler_) |
            stdexec::then([
                declarations = std::move(declarations), request = std::move(request), commands = std::move(commands),
                registrations = std::move(registrations), limit = config_.max_config_bytes,
                dependencies = dependencies ? std::move(*dependencies) : std::vector<RenderEffect::ResourceDependency>{}
            ]() mutable noexcept
            {
                auto effect = buildEffect(declarations, request, registrations, limit, std::move(dependencies));
                if (effect) effect->commands = std::move(commands);
                return effect;
            }) |
            stdexec::continues_on(server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery)) |
            stdexec::let_value([this](EffectBuildResult& built) { return adoptBuiltEffect(std::move(built)); });
        auto rejected = stdexec::just(EffectCreated{error});
        using VSource = exec::variant_sender<decltype(rejected), decltype(compiled)>;
        return error.type ? VSource{std::move(rejected)} : VSource{std::move(compiled)};
    }

    exec::task<EffectCreated> RenderServer::Impl::adoptBuiltEffect(EffectBuildResult candidate) noexcept
    {
        if (!candidate) co_return EffectCreated{candidate.error()};
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (stop.stop_requested()) co_return EffectCreated{RenderError{kCommandCancelled}};
        const auto id = domain_->effects_.emplace(std::move(*candidate));
        compiled_graphs_.fetch_add(1, std::memory_order_relaxed);
        co_return EffectCreated{{}, id};
    }

    void RenderServer::Impl::bindCreateEffectCommand() noexcept
    {
        bindFrameworkCommand<CreateEffect, &Impl::createEffect>();
    }

    void RenderServer::Impl::setEffectParameters(const SetEffectParameters::Request& input, AckResponse& output) noexcept
    {
        auto* effect = domain_->effects_.find(input.effect);
        if (!effect)
        {
            output.error = {kRuntimeIdentity};
            return;
        }
        const auto updated = effect->setParameters(input.parameters);
        output.error = updated ? RenderError{} : updated.error();
    }

    void RenderServer::Impl::removeEffect(const EffectRequest& input, AckResponse& output) noexcept
    {
        const auto* effect = domain_->effects_.find(input.effect);
        if (!effect)
        {
            output.error = {kRuntimeIdentity};
            return;
        }
        const bool referenced = std::ranges::any_of(domain_->jobs_, [&](const auto& job) noexcept
        {
            return job.content.effect == input.effect;
        });
        if (effect->hasExecutions() || referenced)
        {
            output.error = {kRuntimeState};
            return;
        }
        domain_->effects_.erase(input.effect);
        output = {};
    }
} // namespace lux::render
