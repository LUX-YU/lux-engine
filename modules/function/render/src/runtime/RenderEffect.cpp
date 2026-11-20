#include <lux/engine/render/effect/EffectGraphBuild.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>
#include <utility>

namespace lux::render::runtime_detail
{
    RenderResult<void> RenderEffect::retainExecution() noexcept
    {
        if (execution_count_ == UINT32_MAX)
        {
            return cxx::unexpected(RenderError{kRuntimeCapacity});
        }
        ++execution_count_;
        return {};
    }

    void RenderEffect::releaseExecution() noexcept
    {
        if (execution_count_ == 0)
        {
            std::terminate();
        }
        --execution_count_;
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

        for (auto& device : runtime_->devices_) device->removeUniformPools(input.effect);
        runtime_->effects_.erase(input.effect);
        output = {};
    }
} // namespace lux::render
