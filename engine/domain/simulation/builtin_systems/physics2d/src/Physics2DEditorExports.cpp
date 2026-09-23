#include <lux/engine/editor/metadata/EditorPluginExports.hpp>
#include <lux/engine/physics2d/Physics2DSystem.hpp>
#include <lux/engine/physics2d/editor_visibility.h>
#include <lux/engine/ui/Frame.hpp>

extern "C" void lux_physics2d_configuration_meta(lux::meta::ReflectionRegistry &,
                                                lux::meta::qual_type_index_fix_list &);

namespace
{
bool editConfiguration(lux::ui::Frame &frame, void *value) noexcept
{
    auto &config = *static_cast<lux::physics2d::Physics2DSystemConfiguration *>(value);
    bool changed{};
    changed |= frame.editScalar("Gravity X", config.gravity_x).changed;
    changed |= frame.editScalar("Gravity Y", config.gravity_y).changed;
    changed |= frame.editScalar("Fixed step (ns)", config.fixed_step_nanoseconds).changed;
    changed |= frame.editScalar("Maximum substeps", config.max_substeps).changed;
    changed |= frame.editScalar("Body capacity", config.body_capacity).changed;
    return changed;
}
}

extern "C" LUX_PHYSICS2D_EDITOR_PUBLIC const lux::editor::EditorPluginExports *lux_editor_exports_v1() noexcept
{
    static const auto &system = lux::physics2d::physics2DSystemRegistrations().front();
    static const lux::editor::ConfigurationEditorRegistration configuration{
        "lux.physics2d.Configuration", 1, system.configuration,
        +[](lux::meta::ReflectionRegistry &registry) noexcept {
            return registry.findClass(lux::cxx::typeToken<lux::physics2d::Physics2DSystemConfiguration>().name());
        }, &editConfiguration
    };
    static const lux::editor::EditorPluginExports exports{
        sizeof(lux::editor::EditorPluginExports), 1, &lux_physics2d_configuration_meta, &configuration, 1
    };
    return &exports;
}
