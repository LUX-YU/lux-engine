#include <lux/engine/editor/EditorUiRegistrar.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::editor
{
    FrameworkResult<std::unique_ptr<ui::Pane>> EditorUiRegistrar::create(
        EditorContext& context,
        const UiDescription& description
    ) noexcept
    {
        auto factory = findFactory(description.type);
        if (!factory)
        {
            return cxx::unexpected(std::move(factory.error()));
        }
        return factory->get()(context, description);
    }
} // namespace lux::editor
