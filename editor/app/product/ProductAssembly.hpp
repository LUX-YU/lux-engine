#pragma once
#include <lux/engine/editor/EditorError.hpp>
namespace lux::ui
{
    class Root;
}
namespace lux::editor
{
    class EditorContext;
    EditorResult<void> assembleProduct(lux::ui::Root&, EditorContext&) noexcept;
}
