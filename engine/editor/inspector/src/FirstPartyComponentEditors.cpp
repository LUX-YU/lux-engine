#include <lux/engine/editor/inspector/FirstPartyComponentEditors.hpp>

#include <lux/engine/simulation/ecs/Parent.component_editor.hpp>
#include <lux/engine/simulation/ecs/Transform.component_editor.hpp>
#include <lux/engine/simulation/ecs/Visual.component_editor.hpp>

#include <new>
#include <vector>

namespace lux::editor::inspector
{
    lux::cxx::expected<ComponentEditorBindingTable, ComponentEditorBindingFailure>
    buildFirstPartyComponentEditorBindings() noexcept
    {
        try
        {
            std::vector<ComponentEditorBinding> bindings;
            const auto append = [&bindings](std::span<const ComponentEditorBinding> values) {
                bindings.insert(bindings.end(), values.begin(), values.end());
            };
            append(generated::transformComponentEditorBindings());
            append(generated::hierarchyComponentEditorBindings());
            append(generated::visualComponentEditorBindings());
            return ComponentEditorBindingTable::build(std::move(bindings));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(ComponentEditorBindingFailure{
                EComponentEditorBindingError::ALLOCATION_FAILURE,
                {},
                {}
            });
        }
    }
} // namespace lux::editor::inspector
