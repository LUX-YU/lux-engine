#include "PluginBinding.hpp"
#include "PluginComponent.hpp"

#include <test/PluginComponent.component_editor.hpp>

namespace lux::editor::inspector::test
{
    ComponentEditorBinding pluginBinding() noexcept
    {
        return generated::pluginComponentEditorBindings().front();
    }
} // namespace lux::editor::inspector::test
