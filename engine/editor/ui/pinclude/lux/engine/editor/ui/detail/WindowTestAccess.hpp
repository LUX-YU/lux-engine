#pragma once
#include <cstddef>
#include <lux/engine/editor/ui/visibility.h>
extern "C" LUX_EDITOR_UI_PUBLIC void lux_er1_ui_allocation_fail_after(std::size_t) noexcept;
extern "C" LUX_EDITOR_UI_PUBLIC std::size_t lux_er1_ui_allocation_disarm() noexcept;
