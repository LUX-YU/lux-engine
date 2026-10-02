#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/editor/extensions/builtin/render_visibility.h>

extern "C" void
scene_runtime_render_configuration_meta(lux::meta::ReflectionRegistry&, lux::meta::qual_type_index_fix_list&);
extern "C" LUX_RENDER_EDITOR_PUBLIC const lux::editor::extensions::EditorExtensionExports* lux_editor_exports_v8(
) noexcept
{
    using namespace lux::editor;
    static const extensions::EditorExtensionExports exports{
        .counts = {.reflection = 1},
        .contribute = +[](extensions::ContributionDraft& draft,
                          contracts::CodeLease code) -> extensions::ContributionResult<void> {
            draft.reflection.push_back({std::move(code), &scene_runtime_render_configuration_meta});
            return {};
        }
    };
    return &exports;
}
