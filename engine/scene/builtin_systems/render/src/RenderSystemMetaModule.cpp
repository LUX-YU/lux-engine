#include <lux/engine/editor/metadata/EditorPluginExports.hpp>
#include <lux/engine/scene/RenderSystem.hpp>

extern "C" void scene_runtime_render_configuration_meta(lux::meta::ReflectionRegistry &,
                                                        lux::meta::qual_type_index_fix_list &);
extern "C" LUX_ENGINE_SCENE_RENDER_PUBLIC
const lux::editor::EditorPluginExports *lux_editor_exports_v1() noexcept
{
    static const lux::editor::EditorPluginExports exports{
        sizeof(exports), 1, &scene_runtime_render_configuration_meta, nullptr, 0
    };
    return &exports;
}
