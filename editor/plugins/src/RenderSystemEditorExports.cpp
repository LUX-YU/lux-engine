#include <lux/engine/editor/metadata/EditorPluginExports.hpp>
#include <lux/engine/editor/plugins/render_visibility.h>
#include <lux/engine/scene/RenderSystem.hpp>

extern "C" void
scene_runtime_render_configuration_meta(lux::meta::ReflectionRegistry&, lux::meta::qual_type_index_fix_list&);
extern "C" LUX_RENDER_EDITOR_PUBLIC const lux::editor::EditorPluginExports* lux_editor_exports_v6() noexcept
{
    static const lux::editor::EditorPluginExports exports{
        sizeof(exports),
        lux::editor::kEditorPluginInterfaceVersion,
        &scene_runtime_render_configuration_meta,
        nullptr,
        0
    };
    return &exports;
}
