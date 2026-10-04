#pragma once
#include <lux/cxx/reflection/runtime/Marker.hpp>

namespace lux::editor::extensions
{
    struct EditorModuleDescriptor;
}

namespace module_fixture
{
    LUX_META(luxmodule)
    const lux::editor::extensions::EditorModuleDescriptor& module() noexcept;
} // namespace module_fixture
