#pragma once

#include <cstdint>
#include <vector>

#include <imgui.h>

#include <lux/engine/function/visibility.h>

namespace lux::ui::drawdata
{
    struct DrawDataSummary final
    {
        std::uint32_t command_lists{0};
        std::uint32_t vertices{0};
        std::uint32_t indices{0};
    };

    class LUX_FUNCTION_PUBLIC DrawDataSnapshot final
    {
    public:
        DrawDataSnapshot() = default;
        ~DrawDataSnapshot();
        DrawDataSnapshot(const DrawDataSnapshot&) = delete;
        DrawDataSnapshot& operator=(const DrawDataSnapshot&) = delete;
        DrawDataSnapshot(DrawDataSnapshot&&) = delete;
        DrawDataSnapshot& operator=(DrawDataSnapshot&&) = delete;

        void capture(const ImDrawData& draw_data);
        void clear() noexcept;

        [[nodiscard]] const ImDrawData& drawData() const noexcept { return draw_data_; }

    private:
        ImDrawData draw_data_{};
        ImVector<ImDrawList*> command_lists_;
        std::vector<ImDrawList*> owned_lists_;
    };

    [[nodiscard]] LUX_FUNCTION_PUBLIC DrawDataSummary
    summarize(const ImDrawData& draw_data) noexcept;
} // namespace lux::ui::drawdata
