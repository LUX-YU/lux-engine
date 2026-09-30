#include <desktop_hierarchy.inspector.generated.hpp>
#include <desktop_transform.inspector.generated.hpp>
#include <desktop_visual.inspector.generated.hpp>
#include <desktop_camera.inspector.generated.hpp>
#include <lux/engine/editor/ui/ComponentEditors.hpp>

namespace lux::editor::ui
{
    std::vector<ComponentEditorRegistration> componentEditors()
    {
        std::vector<ComponentEditorRegistration> result;
        const auto append = [&](const auto& bindings) {
            result.insert(result.end(), bindings.begin(), bindings.end());
        };
        append(generated::desktop_transformBindings());
        append(generated::desktop_hierarchyBindings());
        append(generated::desktop_visualBindings());
        append(generated::desktop_cameraBindings());
        for (auto& entry : result)
            entry.provider = {"lux.editor.product", 1};
        return result;
    }
} // namespace lux::editor::ui
