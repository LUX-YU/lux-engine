#pragma once
#include <lux/engine/editor/ui/visibility.h>
#include <lux/engine/ui/Root.hpp>
namespace lux::input
{
    struct InputSnapshot;
}
namespace lux::editor::ui
{
    [[nodiscard]] LUX_EDITOR_UI_PUBLIC lux::cxx::expected<void, lux::ui::EInputError>
    feedWindowInput(lux::ui::Root&, const input::InputSnapshot&) noexcept;
}
