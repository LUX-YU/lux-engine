#pragma once
#include <lux/engine/ui/Root.hpp>
namespace lux::input
{
    struct InputSnapshot;
}
namespace lux::editor::desktop
{
    [[nodiscard]] lux::cxx::expected<void, lux::ui::EInputError>
    feedWindowInput(lux::ui::Root&, const input::InputSnapshot&) noexcept;
}
