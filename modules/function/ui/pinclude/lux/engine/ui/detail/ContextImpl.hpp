#pragma once

#include <imgui.h>
#include <lux/engine/ui/Context.hpp>

#include <vector>

namespace lux::ui
{
struct Context::Impl final
{
    ~Impl();
    ImGuiContext *native{};
    std::vector<std::uint8_t> font_bytes;
    std::vector<ImWchar> font_ranges;
    bool frame_open{};
    bool output_ready{};
};
} // namespace lux::ui
