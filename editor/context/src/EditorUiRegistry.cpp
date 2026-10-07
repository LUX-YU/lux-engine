#include <algorithm>
#include <lux/engine/editor/ContextErrors.hpp>
#include <lux/engine/editor/EditorUiRegistry.hpp>

namespace lux::editor
{
    EditorUiRegistry::EditorUiRegistry(std::vector<Factory> entries) noexcept : entries_(std::move(entries)) {}

    EditorUiRegistry::~EditorUiRegistry() = default;

    FrameworkResult<EditorUiRegistry::FactoryRef> EditorUiRegistry::resolveFactory(std::string_view type) const noexcept
    {
        const auto found = std::ranges::find(entries_, type, &Factory::type_);
        if (found == entries_.end())
        {
            return cxx::unexpected(error::Error{Errors::EditorUnknownUiType});
        }
        return std::cref(*found);
    }
} // namespace lux::editor
