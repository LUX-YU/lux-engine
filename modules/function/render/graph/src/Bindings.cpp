#include <cmath>
#include <exception>
#include <lux/engine/render/graph/Bindings.hpp>

namespace lux::render
{
    GraphInvocationData makeGraphInvocationData(const RenderGraphDefinition& definition) noexcept
    {
        GraphInvocationData result;
        for (const auto& pass : definition.passes())
        {
            GraphPassInvocation values;
            values.scalars = pass.initial_scalars;
            for (const auto& field : pass.bindings)
            {
                if (field.role == rdesc::EPassFieldRole::SAMPLER)
                {
                    values.fields.emplace_back(field.sampler);
                }
                else if (field.role == rdesc::EPassFieldRole::COLOR_ATTACHMENT)
                {
                    values.fields.emplace_back(ColorClearValue{field.clear});
                }
                else if (field.role == rdesc::EPassFieldRole::DEPTH_STENCIL)
                {
                    values.fields.emplace_back(DepthStencilClearValue{field.clear_depth, field.clear_stencil});
                }
                else
                {
                    values.fields.emplace_back(std::monostate{});
                }
            }
            result.passes.push_back(std::move(values));
        }
        return result;
    }

    RenderResult<FrameGraphBindings> FrameGraphBindings::validate(
        const LogicalGraphPlan& plan,
        GraphFrameValues frame,
        std::span<const GraphImportBinding> imports,
        const GraphInvocationData* values
    ) noexcept
    {
        if (imports.size() != plan.imports().size())
        {
            return cxx::unexpected(RenderError{kGraphInvalidBinding, {imports.size()}});
        }
        for (std::size_t i = 0; i < imports.size(); ++i)
        {
            const auto& binding = imports[i];
            const auto resource = plan.imports()[i];
            if (binding.resource != resource || !binding.backing.isValid() || binding.ready_epoch == 0 ||
                (binding.dynamic_offset != 0 &&
                 plan.identity().resources[resource.value() - 1].kind() == EGraphResourceKind::IMAGE))
            {
                return cxx::unexpected(RenderError{kGraphInvalidBinding, {i}});
            }
            const auto& contract = plan.identity().resources[resource.value() - 1].import_contract;
            if (contract && contract->temporal_history &&
                (!values || values->history_epoch == 0 || binding.history_epoch != values->history_epoch))
            {
                return cxx::unexpected(RenderError{kGraphInvalidBinding, {i}});
            }
            for (std::size_t j = 0; j < i; ++j)
            {
                if (imports[j].backing == binding.backing)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidBinding, {i}});
                }
            }
        }
        if (!values && plan.requiresInvocationData())
        {
            return cxx::unexpected(RenderError{kGraphInvalidBinding, {}});
        }
        if (!values)
        {
            return FrameGraphBindings{plan, frame, imports, nullptr};
        }
        if (values && values->passes.size() != plan.identity().passes.size())
        {
            return cxx::unexpected(RenderError{kGraphInvalidBinding, {values->passes.size()}});
        }
        for (std::size_t p = 0; p < plan.identity().passes.size(); ++p)
        {
            const auto& pass = plan.identity().passes[p];
            if (!values)
            {
                if (pass.scalar_size != 0 || !pass.bindings.empty() || pass.condition.isValid())
                {
                    return cxx::unexpected(RenderError{kGraphInvalidBinding, {p}});
                }
                continue;
            }
            const auto& supplied = values->passes[p];
            if (supplied.scalars.size() != pass.scalar_size || supplied.fields.size() != pass.bindings.size() ||
                (!pass.condition.isValid() && !supplied.enabled))
            {
                return cxx::unexpected(RenderError{kGraphInvalidBinding, {p}});
            }
            for (std::size_t q = 0; q < p; ++q)
            {
                if (pass.condition.isValid() && pass.condition == plan.identity().passes[q].condition &&
                    supplied.enabled != values->passes[q].enabled)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidBinding, {p}});
                }
            }
            for (std::size_t f = 0; f < pass.bindings.size(); ++f)
            {
                const auto& binding = pass.bindings[f].value;
                std::size_t expected = 0;
                if (std::holds_alternative<SamplerBinding>(binding))
                {
                    expected = 1;
                }
                if (const auto* attachment = std::get_if<AttachmentBinding>(&binding))
                {
                    expected = attachment->role == rdesc::EPassFieldRole::COLOR_ATTACHMENT
                                   ? 2
                                   : (attachment->role == rdesc::EPassFieldRole::DEPTH_STENCIL ? 3 : 0);
                }
                if (supplied.fields[f].index() != expected)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidBinding, {p, f}});
                }
                if (expected == 3)
                {
                    const auto depth = std::get<DepthStencilClearValue>(supplied.fields[f]).depth;
                    if (!std::isfinite(depth) || depth < 0.0f || depth > 1.0f)
                    {
                        return cxx::unexpected(RenderError{kGraphInvalidBinding, {p, f}});
                    }
                }
                if (expected == 1 && !std::get<GraphSampler>(supplied.fields[f]).isValid())
                {
                    return cxx::unexpected(RenderError{kGraphInvalidBinding, {p, f}});
                }
            }
        }
        return FrameGraphBindings{plan, frame, imports, values};
    }

    GraphResourceId FrameGraphBindings::resourceFor(GraphPassId pass, std::uint32_t use_index) const noexcept
    {
        const bool invalid = !pass.isValid() || pass.value() > plan_->identity().passes.size() ||
                             use_index >= plan_->identity().passes[pass.value() - 1].uses.size();
        if (invalid)
        {
            std::terminate(); // Structural caller contract, never a release-disabled assert.
        }
        for (const auto& choice : plan_->inputChoices())
        {
            if (choice.consumer == pass && choice.use_index == use_index &&
                !values_->passes[choice.conditional_producer.value() - 1].enabled)
            {
                return choice.fallback;
            }
        }
        return plan_->identity().passes[pass.value() - 1].uses[use_index].resource;
    }

    bool mayShareSceneInvocation(GraphPassId id, const FrameGraphBindings& a, const FrameGraphBindings& b) noexcept
    {
        const bool invalid = &a.plan() != &b.plan() || !id.isValid() ||
                             id.value() > a.plan().sceneShareEligible().size() ||
                             !a.plan().sceneShareEligible()[id.value() - 1];
        if (invalid)
        {
            return false;
        }
        const auto* x = a.values();
        const auto* y = b.values();
        if (!x || !y || x->scene == 0 || x->scene != y->scene || x->scene_revision != y->scene_revision)
        {
            return false;
        }
        for (const auto source : a.plan().sceneSources()[id.value() - 1])
        {
            if (*a.invocation(source) != *b.invocation(source))
            {
                return false;
            }
            for (const auto& use : a.plan().identity().passes[source.value() - 1].uses)
            {
                for (std::size_t i = 0; i < a.imports().size(); ++i)
                {
                    if (a.imports()[i].resource == use.resource && a.imports()[i] != b.imports()[i])
                    {
                        return false;
                    }
                }
            }
        }
        return true;
    }
} // namespace lux::render
