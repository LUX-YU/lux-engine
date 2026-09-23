#include <lux/engine/editor/metadata/EditorPluginExports.hpp>
#include <lux/engine/function/render/features/meta_visibility.h>
#include "render_feature_client_meta_registration.hpp"

extern "C" LUX_RENDER_FEATURE_META_PUBLIC
const lux::editor::EditorPluginExports *lux_editor_exports_v1() noexcept
{
    static const lux::editor::EditorPluginExports exports{
        sizeof(exports), 1,
        +[](lux::meta::ReflectionRegistry &registry, lux::meta::qual_type_index_fix_list &) {
            LuxRegisterRender_feature_clientMetas_META(registry);
        }, nullptr, 0
    };
    return &exports;
}
