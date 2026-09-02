#pragma once

#include <imgui.h>

#include <vector>

namespace lux::ui::detail
{
    class ImGuiDrawDataSnapshot final
    {
    public:
        ImGuiDrawDataSnapshot() = default;
        ~ImGuiDrawDataSnapshot();
        ImGuiDrawDataSnapshot(const ImGuiDrawDataSnapshot&) = delete;
        ImGuiDrawDataSnapshot& operator=(const ImGuiDrawDataSnapshot&) = delete;
        ImGuiDrawDataSnapshot(ImGuiDrawDataSnapshot&& other) noexcept;
        ImGuiDrawDataSnapshot& operator=(ImGuiDrawDataSnapshot&& other) noexcept;

        void capture(const ImDrawData& draw_data);
        void clear() noexcept;
        [[nodiscard]] const ImDrawData& drawData() const noexcept { return draw_data_; }

    private:
        void rebuildPointers() noexcept;

        ImDrawData draw_data_{};
        std::vector<ImDrawList*> owned_lists_;
    };
} // namespace lux::ui::detail
