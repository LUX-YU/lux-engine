#pragma once

#include <lux/engine/function/visibility.h>

namespace lux::ui::detail
{
    /** Private lease for backend calls that still use Dear ImGui's process-global current context. */
    class LUX_FUNCTION_PUBLIC ImGuiContextLease final
    {
    public:
        explicit ImGuiContextLease(void* context) noexcept;
        ~ImGuiContextLease();
        ImGuiContextLease(const ImGuiContextLease&) = delete;
        ImGuiContextLease& operator=(const ImGuiContextLease&) = delete;

    private:
        void* previous_{};
    };
} // namespace lux::ui::detail
