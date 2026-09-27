#pragma once
#include <lux/engine/editor/EditorError.hpp>
namespace lux::editor
{
    class EditorContext;
    EditorResult<void> assembleCommands(EditorContext&) noexcept;
}
