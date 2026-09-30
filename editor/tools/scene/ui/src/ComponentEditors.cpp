#include <scene_fields_hierarchy.inspector.generated.hpp>
#include <scene_fields_transform.inspector.generated.hpp>
#include <scene_fields_visual.inspector.generated.hpp>
#include <scene_fields_camera.inspector.generated.hpp>
#include <lux/engine/editor/scene/InspectorView.hpp>

namespace lux::editor::scene
{
    std::vector<InspectorComponent> sceneInspectorComponents()
    {
        std::vector<InspectorComponent> result;
        const auto append = [&](const auto& bindings) {
            result.insert(result.end(), bindings.begin(), bindings.end());
        };
        append(generated::scene_fields_transformBindings());
        append(generated::scene_fields_hierarchyBindings());
        append(generated::scene_fields_visualBindings());
        append(generated::scene_fields_cameraBindings());
        return result;
    }
} // namespace lux::editor::scene
