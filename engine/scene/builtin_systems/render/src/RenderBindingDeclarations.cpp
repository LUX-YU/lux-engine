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
            const bool duplicate_result = std::ranges::any_of(
                declaration.results,
                [&](auto result) noexcept
                { return std::ranges::find(existing.results, result) != existing.results.end(); }
            );
            if (existing.type == declaration.type && existing.role == declaration.role)
            {
                return cxx::unexpected(ERenderBindingDeclarationError::DUPLICATE_TYPE);
            }
            if (existing.name == declaration.name && existing.role == declaration.role)
            {
                return cxx::unexpected(ERenderBindingDeclarationError::DUPLICATE_NAME);
            }
            if (duplicate_result && (existing.role == declaration.role || !existing.partitioned_results ||
                                     !declaration.partitioned_results))
            {
                return cxx::unexpected(ERenderBindingDeclarationError::DUPLICATE_RESULT);
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
