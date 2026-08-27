#pragma once

#include <vector>

#include <imgui.h>

#include <lux/engine/function/visibility.h>

namespace lux::ui
{
    class LUX_FUNCTION_PUBLIC DrawDataSnapshot final
    {
    public:
        DrawDataSnapshot() = default;
        ~DrawDataSnapshot();
        DrawDataSnapshot(const DrawDataSnapshot&) = delete;
        DrawDataSnapshot& operator=(const DrawDataSnapshot&) = delete;
        DrawDataSnapshot(DrawDataSnapshot&& other) noexcept;
        DrawDataSnapshot& operator=(DrawDataSnapshot&& other) noexcept;

        void capture(const ImDrawData& draw_data);
        void clear() noexcept;

        [[nodiscard]] const ImDrawData& drawData() const noexcept
        {
            return draw_data_;
        }

    private:
        void rebuildPointers() noexcept;

        ImDrawData draw_data_{};
        std::vector<ImDrawList*> owned_lists_;
    };
} // namespace lux::ui
