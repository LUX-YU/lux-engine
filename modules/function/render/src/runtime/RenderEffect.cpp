#include <exec/variant_sender.hpp>
#include <limits>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>
#include <map>
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
    RenderResult<runtime_detail::QualifiedEffectRecipe>
    RenderServer::Impl::qualifyEffectRecipe(const CreateEffectRequest& request) const noexcept
    {
        if (auto valid = validateEffectOutput(request.output); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        if (!request.parameters.valid())
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        if (request.definition.features.empty())
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        runtime_detail::QualifiedEffectRecipe recipe;
        recipe.uses_capabilities = !request.implementations.empty();
        std::map<std::string_view, const CapabilityImplementation*> contributions;
        std::set<std::pair<std::size_t, std::string_view>> capabilities;
        std::set<std::pair<std::uint32_t, std::uint32_t>> resource_ids;
        std::vector<const CapabilityDescription*> declarations;
        for (const auto& use : request.implementations)
        {
            const auto* implementation = capability_registry_.find(use.implementation);
            if (!implementation) return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (use.contribution.empty() || !contributions.emplace(use.contribution, implementation).second)
                return cxx::unexpected(RenderError{kCapabilityDeclaration});
            if (auto error = capabilityOutputError(*implementation, request.output); error.type)
                return cxx::unexpected(error);

            const auto& factory = implementation->state_factory;
            const auto state = recipe.contributions.factories.size();
            if (factory.create_) recipe.contributions.factories.push_back(factory);
            for (const auto& binding : implementation->command_bindings)
            {
                const auto* method = runtime_->command_registry_.binding(binding);
                if (!method || !factory.create_ || method->effect_state_type != factory.type_)
                    return cxx::unexpected(RenderError{kCommandContract});
                recipe.contributions.commands.push_back({{binding.command, method->call}, state});
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
                const auto* factory = runtime_->resources_.factory(type);
                if (!factory) return cxx::unexpected(RenderError{kStorageIdentity});
                recipe.resources.push_back({factory->descriptor->domain, type, {}, {}});
            }
        }
        const auto command_id = [](const auto& selection) noexcept { return commandTag(selection.route.command); };
        auto& commands = recipe.contributions.commands;
        std::ranges::sort(commands, {}, command_id);
        if (std::ranges::adjacent_find(commands, std::ranges::equal_to{}, command_id) != commands.end())
            return cxx::unexpected(RenderError{kCapabilityConflict});

        for (const auto* declaration : declarations)
        {
            for (const auto prerequisite : declaration->prerequisites)
            {
                if (!capabilities.contains({prerequisite.hash(), prerequisite.name()}))
                    return cxx::unexpected(RenderError{kCapabilityPrerequisite});
            }
        }
        recipe.features.reserve(request.definition.features.size());
        for (const auto& selection : request.definition.features)
        {
            const auto* feature = features_.find(selection.feature);
            if (!feature) return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (recipe.uses_capabilities)
            {
                const std::string_view key = selection.contribution_key;
                const CapabilityImplementation* owner = nullptr;
                for (const auto& [name, implementation] : contributions)
                {
                    const bool is_child = key.size() > name.size() && key.starts_with(name) && key[name.size()] == '/';
                    if (key != name && !is_child) continue;
                    if (owner) return cxx::unexpected(RenderError{kCapabilityDeclaration});
                    owner = implementation;
                }
                if (!owner || std::ranges::find(owner->features, selection.feature) == owner->features.end())
                {
                    return cxx::unexpected(RenderError{kCapabilityDeclaration});
                }
            }
            recipe.features.push_back(feature);
        }
        return recipe;
    }

    auto RenderServer::Impl::createEffect(CreateEffect::Request&& request) noexcept
    {
        auto recipe = qualifyEffectRecipe(request);
        const auto error = recipe ? RenderError{} : recipe.error();
        std::vector<ResourceDeclaration> declarations;
        runtime_detail::EffectContributions effect_contributions;
        if (recipe)
        {
            effect_contributions = std::move(recipe->contributions);
            runtime_->resources_.forEachType(
                [&](ResourceTypeId id, const ResourceDefinition& definition) noexcept
                {
                    const auto* factory = runtime_->resources_.factory(id);
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
                declarations = std::move(declarations), request = std::move(request),
                qualified = recipe ? std::move(*recipe) : runtime_detail::QualifiedEffectRecipe{},
                limit = config_.max_config_bytes
            ]() mutable noexcept
            {
                return buildEffect(declarations, request, std::move(qualified), limit);
            }) |
            stdexec::continues_on(server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery)) |
            stdexec::let_value([this, contributions = std::move(effect_contributions)](EffectBuildResult& built) mutable
            {
                return adoptBuiltEffect(std::move(built), std::move(contributions));
            });
        auto rejected = stdexec::just(EffectCreated{error});
        using VSource = exec::variant_sender<decltype(rejected), decltype(compiled)>;
        return error.type ? VSource{std::move(rejected)} : VSource{std::move(compiled)};
    }

    exec::task<EffectCreated> RenderServer::Impl::adoptBuiltEffect(
        EffectBuildResult candidate, runtime_detail::EffectContributions contributions
    ) noexcept
    {
        if (!candidate) co_return EffectCreated{candidate.error()};
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (stop.stop_requested()) co_return EffectCreated{RenderError{kCommandCancelled}};
        for (const auto& factory : contributions.factories)
        {
            auto state = factory.create_();
            if (!state) co_return EffectCreated{state.error()};
            candidate->contribution_states.emplace_back(*state, factory.destroy_);
        }
        candidate->command_routes.reserve(contributions.commands.size());
        for (auto& command : contributions.commands)
        {
            command.route.call.object = candidate->contribution_states[command.state].get();
            candidate->command_routes.push_back(command.route);
        }
        const auto id = runtime_->effects_.emplace(std::move(*candidate));
        compiled_graphs_.fetch_add(1, std::memory_order_relaxed);
        co_return EffectCreated{{}, id};
    }

    void RenderServer::Impl::bindCreateEffectCommand() noexcept
    {
        bindFrameworkCommand<CreateEffect, &Impl::createEffect>();
    }

    void RenderServer::Impl::setEffectParameters(const SetEffectParameters::Request& input, AckResponse& output) noexcept
    {
        auto* effect = runtime_->effects_.find(input.effect);
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
        if (effect->hasExecutions() || referenced)
        {
            output.error = {kRuntimeState};
            return;
        }
        runtime_->effects_.erase(input.effect);
        output = {};
    }
} // namespace lux::render
