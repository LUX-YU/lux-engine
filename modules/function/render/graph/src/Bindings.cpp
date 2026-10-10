#include <lux/engine/render/graph/Bindings.hpp>

namespace lux::render
{
    FrameGraphBindings::FrameGraphBindings(
        const CompiledGraphPlan& plan,
        GraphFrameValues frame,
        std::span<const GraphImportBinding> imports
    ) noexcept : plan_(&plan), frame_(frame), imports_(imports)
    {
    }

    RenderResult<FrameGraphBindings> FrameGraphBindings::create(
        const CompiledGraphPlan& plan,
        GraphFrameValues frame,
        std::span<const GraphImportBinding> imports
    ) noexcept
    {
        if (imports.size() != plan.imports().size())
        {
            return cxx::unexpected(RenderError{kGraphInvalidBinding, {imports.size()}});
        }
        for (std::size_t index = 0; index < imports.size(); ++index)
        {
            const auto& binding = imports[index];
            const auto resource = plan.imports()[index];
            const bool is_wrong_resource = binding.resource != resource;
            const bool is_invalid_backing = !binding.backing.isValid();
            const bool is_image_offset = binding.dynamic_offset != 0 &&
                plan.definition().resources()[resource.value() - 1].kind == EGraphResourceKind::IMAGE;
            const bool is_invalid_binding = is_wrong_resource || is_invalid_backing || is_image_offset;
            if (is_invalid_binding)
            {
                return cxx::unexpected(RenderError{kGraphInvalidBinding, {index}});
            }
            for (std::size_t previous = 0; previous < index; ++previous)
            {
                if (imports[previous].backing == binding.backing)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidBinding, {index}});
                }
            }
        }
        return FrameGraphBindings{plan, frame, imports};
    }
}
