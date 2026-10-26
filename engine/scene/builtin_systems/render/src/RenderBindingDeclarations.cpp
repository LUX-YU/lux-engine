#include <lux/engine/scene/render/RenderBindingDeclarations.hpp>

namespace lux::scene
{
    RenderBindingRegistrationResult RenderBindingDeclarations::add(RenderBindingDeclaration declaration) noexcept
    {
        const bool invalid_identity =
            declaration.version == 0 || declaration.name.empty() || !declaration.type.isValid();
        const bool missing_entry = !declaration.create || !declaration.destroy || !declaration.connect;
        const bool unpartitioned_view_result = declaration.scope == ERenderOperationScope::VIEW &&
                                               !declaration.results.empty() && !declaration.partitioned_results;
        const bool invalid = invalid_identity || missing_entry || unpartitioned_view_result;
        if (invalid)
        {
            return cxx::unexpected(ERenderBindingDeclarationError::INVALID_DECLARATION);
        }
        for (const auto& existing : declarations_)
        {
            if (existing.type == declaration.type && existing.role == declaration.role)
            {
                return cxx::unexpected(ERenderBindingDeclarationError::DUPLICATE_TYPE);
            }
            if (existing.name == declaration.name && existing.role == declaration.role)
            {
                return cxx::unexpected(ERenderBindingDeclarationError::DUPLICATE_NAME);
            }
        }
        for (std::size_t i = 0; i < declaration.resource_inputs.size(); ++i)
        {
            const auto& selection = declaration.resource_inputs[i];
            const auto contract =
                std::ranges::find(declaration.inputs, selection.schema, &render::RenderPayloadContract::schema);
            const bool invalid_contract =
                contract == declaration.inputs.end() || contract->use != render::ERenderPayloadUse::RESOURCE_INPUT;
            const bool invalid_scope =
                selection.source == ERenderInputSource::VIEW && declaration.scope != ERenderOperationScope::VIEW;
            const bool duplicate = std::ranges::any_of(
                std::span{declaration.resource_inputs}.first(i),
                [&](const auto& previous) noexcept { return previous.schema == selection.schema; }
            );
            if (invalid_contract || invalid_scope || duplicate)
            {
                return cxx::unexpected(ERenderBindingDeclarationError::INVALID_DECLARATION);
            }
        }
        declarations_.push_back(std::move(declaration));
        return {};
    }

    std::span<const RenderBindingDeclaration> RenderBindingDeclarations::declarations() const noexcept
    {
        return declarations_;
    }

    bool RenderBindingDeclarations::matches(
        const render::RenderPayloadContract& provided,
        const render::RenderPayloadContract& requested
    ) noexcept
    {
        return provided.schema == requested.schema && provided.version == requested.version &&
               provided.cppType() == requested.cppType() && provided.use == requested.use &&
               provided.lifetime == requested.lifetime;
    }
} // namespace lux::scene
