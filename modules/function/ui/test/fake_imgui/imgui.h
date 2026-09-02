#pragma once

#include <cstddef>

struct ImGuiPayload final
{
    const void* Data{};
    int DataSize{};
};

namespace ImGui
{
    inline bool begin_result{true};
    inline bool payload_available{true};
    inline int begin_count{};
    inline int accept_count{};
    inline int end_count{};
    inline ImGuiPayload payload{};

    inline void resetDragDropDiagnostics() noexcept
    {
        begin_result = true;
        payload_available = true;
        begin_count = 0;
        accept_count = 0;
        end_count = 0;
        payload = {};
    }

    inline bool SetDragDropPayload(const char*, const void*, std::size_t)
    {
        return true;
    }

    inline bool BeginDragDropTarget()
    {
        ++begin_count;
        return begin_result;
    }

    inline const ImGuiPayload* AcceptDragDropPayload(const char*)
    {
        ++accept_count;
        return payload_available ? &payload : nullptr;
    }

    inline void EndDragDropTarget()
    {
        ++end_count;
    }
} // namespace ImGui
