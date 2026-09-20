#include <desktop_hierarchy.inspector.generated.hpp>
#include <desktop_transform.inspector.generated.hpp>
#include <desktop_visual.inspector.generated.hpp>
#include <desktop_camera.inspector.generated.hpp>
#include <lux/engine/editor/gui/scene/ComponentBinding.hpp>

namespace lux::editor::gui
{
    std::vector<ComponentBinding> firstPartyComponentBindings()
    {
        std::vector<ComponentBinding> result;
        const auto append = [&](const auto &bindings)
        { result.insert(result.end(), bindings.begin(), bindings.end()); };
        append(generated::desktop_transformBindings());
        append(generated::desktop_hierarchyBindings());
        append(generated::desktop_visualBindings());
        append(generated::desktop_cameraBindings());
        return result;
    }
} // namespace lux::editor::gui
