#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorUiFactories.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::editor
{
    FrameworkResult<std::unique_ptr<ui::Pane>> createPane(
        EditorContext& context,
        const PaneDescription& description
    ) noexcept
    {
        auto factory = context.ui().resolveFactory(description.type);
        if (!factory)
        {
            return cxx::unexpected(std::move(factory.error()));
        }
        return factory->get()(context, description);
    }
} // namespace lux::editor
