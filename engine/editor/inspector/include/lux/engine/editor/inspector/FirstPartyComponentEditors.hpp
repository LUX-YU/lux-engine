#pragma once

#include <lux/engine/editor/inspector/ComponentEditorBinding.hpp>

#include <lux/cxx/compile_time/expected.hpp>

namespace lux::editor::inspector
{
    [[nodiscard]] LUX_EDITOR_INSPECTOR_PUBLIC lux::cxx::expected<
        ComponentEditorBindingTable,
        ComponentEditorBindingFailure
    > buildFirstPartyComponentEditorBindings() noexcept;
} // namespace lux::editor::inspector
