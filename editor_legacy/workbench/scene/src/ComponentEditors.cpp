#include <scene_fields_hierarchy_legacy.inspector.generated.hpp>
#include <scene_fields_transform_legacy.inspector.generated.hpp>
#include <scene_fields_visual_legacy.inspector.generated.hpp>
#include <scene_fields_camera_legacy.inspector.generated.hpp>
#include <lux/engine/editor/scene/InspectorView.hpp>

namespace lux::editor::scene
{
    std::vector<InspectorComponent> sceneInspectorComponents()
    {
        std::vector<InspectorComponent> result;
        const auto append = [&](const auto& bindings) {
            result.insert(result.end(), bindings.begin(), bindings.end());
        };
        append(generated::scene_fields_transform_legacyBindings());
        append(generated::scene_fields_hierarchy_legacyBindings());
        append(generated::scene_fields_visual_legacyBindings());
        append(generated::scene_fields_camera_legacyBindings());
        return result;
    }
    std::vector<RunInspectorComponent> runInspectorComponents()
    {
        std::vector<RunInspectorComponent> result;
        const auto append = [&](const auto& bindings) {
            result.insert(result.end(), bindings.begin(), bindings.end());
        };
        append(run_generated::scene_fields_transform_legacyBindings());
        append(run_generated::scene_fields_hierarchy_legacyBindings());
        append(run_generated::scene_fields_visual_legacyBindings());
        append(run_generated::scene_fields_camera_legacyBindings());
        return result;
    }
} // namespace lux::editor::scene
