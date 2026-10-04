#include "ModuleFixture.hpp"
#include <lux/engine/dynamic_library/LibraryExport.hpp>
#include <lux/engine/editor/extensions/EditorExtension.hpp>

#if defined(_WIN32)
#define MODULE_EXPORT __declspec(dllexport)
#else
#define MODULE_EXPORT __attribute__((visibility("default")))
#endif
extern "C" MODULE_EXPORT const lux::engine::platform::LibraryExportIdentity* lux_plugin_identity_v1() noexcept
{
    static const lux::engine::platform::LibraryExportIdentity
        identity{sizeof(identity), 1, "ec4.module.fixture", 1, MODULE_RUNTIME_ABI, "ec4-module", "ec4-module"};
    return &identity;
}

extern "C" MODULE_EXPORT const lux::editor::extensions::EditorExtensionExports* lux_editor_exports_v10() noexcept
{
    return module_fixture::module().exports();
}
