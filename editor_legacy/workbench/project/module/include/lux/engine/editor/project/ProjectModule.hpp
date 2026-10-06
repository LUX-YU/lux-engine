#pragma once
#include <lux/cxx/reflection/runtime/Marker.hpp>

namespace lux::editor::extensions
{
    struct EditorModuleDescriptor;
}
namespace lux::editor::project
{
    // Shared project activities and browser, independently selectable from every author tool.
    LUX_META(luxmodule)
    const extensions::EditorModuleDescriptor& projectModule() noexcept;
}
