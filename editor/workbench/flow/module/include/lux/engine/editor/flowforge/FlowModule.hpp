#pragma once
#include <lux/cxx/reflection/runtime/Marker.hpp>

namespace lux::editor::extensions
{
    struct EditorModuleDescriptor;
}
namespace lux::editor::flowforge
{
    // Cold declaration shared by the selected product index and the dynamic extension export.
    LUX_META(luxmodule)
    const extensions::EditorModuleDescriptor& flowModule() noexcept;
} // namespace lux::editor::flowforge
