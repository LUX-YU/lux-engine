#include <lux/engine/ui_next_drawdata/DrawDataSnapshot.hpp>

namespace lux::ui::drawdata
{
    DrawDataSnapshot::~DrawDataSnapshot() { clear(); }

    void DrawDataSnapshot::capture(const ImDrawData& draw_data)
    {
        clear();
        draw_data_ = draw_data;
        command_lists_.reserve(draw_data.CmdListsCount);
        owned_lists_.reserve(static_cast<std::size_t>(draw_data.CmdListsCount));
        for (int index = 0; index < draw_data.CmdListsCount; ++index)
        {
            auto* copy = draw_data.CmdLists[index]->CloneOutput();
            owned_lists_.push_back(copy);
            command_lists_.push_back(copy);
        }
        draw_data_.CmdLists = command_lists_;
        draw_data_.CmdListsCount = command_lists_.Size;
    }

    void DrawDataSnapshot::clear() noexcept
    {
        for (auto* list : owned_lists_)
            IM_DELETE(list);
        owned_lists_.clear();
        command_lists_.clear();
        draw_data_.Clear();
    }

    DrawDataSummary summarize(const ImDrawData& draw_data) noexcept
    {
        return {
            static_cast<std::uint32_t>(draw_data.CmdListsCount),
            static_cast<std::uint32_t>(draw_data.TotalVtxCount),
            static_cast<std::uint32_t>(draw_data.TotalIdxCount)
        };
    }
} // namespace lux::ui::drawdata
