#include <lux/engine/render/effect/EffectGraphBuild.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>
#include <utility>

namespace lux::render::runtime_detail
{
    EffectExecutionUse::EffectExecutionUse(EffectExecutionUse&& other) noexcept
        : effect_(std::exchange(other.effect_, nullptr))
    {
    }

    EffectExecutionUse& EffectExecutionUse::operator=(EffectExecutionUse&& other) noexcept
    {
        if (this != &other)
        {
            if (effect_) effect_->releaseExecution();
            effect_ = std::exchange(other.effect_, nullptr);
        }
        return *this;
    }

    EffectExecutionUse::~EffectExecutionUse() noexcept
    {
        if (effect_) effect_->releaseExecution();
    }

    RenderResult<EffectExecutionUse> RenderEffect::borrowExecution() noexcept
    {
        if (execution_borrows_ == UINT32_MAX)
        {
            return cxx::unexpected(RenderError{kRuntimeCapacity});
        }
        ++execution_borrows_;
        return EffectExecutionUse{*this};
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
    auto RenderServer::Impl::createEffect(CreateEffect::Request&& request) noexcept
    {
        if (!request.graph) return stdexec::just(EffectCreated{RenderError{kCapabilityDeclaration}});
        RenderEffect candidate;
        auto initialized = initializeEffectGraph(
            candidate,
            std::move(request.graph),
            effect_catalog_,
            runtime_->command_registry_,
            config_.max_config_bytes,
            config_.diagnostics
        );
        if (!initialized) return stdexec::just(EffectCreated{initialized.error()});
        return stdexec::just(EffectCreated{{}, runtime_->effects_.emplace(std::move(candidate))});
    }

    void RenderServer::Impl::bindCreateEffectCommand() noexcept
    {
        bindFrameworkCommand<CreateEffect, &Impl::createEffect>();
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
        if (effect->hasExecutions() || referenced)
        {
            output.error = {kRuntimeState};
            return;
        }
        runtime_->effects_.erase(input.effect);
        output = {};
    }
} // namespace lux::render
