#include <lux/engine/ui/Table.hpp>
#include <lux/engine/ui/detail/NullTerminatedText.hpp>
#include <lux/engine/ui/detail/Contract.hpp>
#include <imgui.h>
#include <utility>

namespace lux::ui
{
    TableScope::TableScope(TableScope&& other) noexcept : active_(std::exchange(other.active_, false)) {}

    TableScope& TableScope::operator=(TableScope&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            if (active_)
            {
                ImGui::EndTable();
            }
            active_ = std::exchange(other.active_, false);
        }
        return *this;
    }

    TableScope::~TableScope() noexcept
    {
        if (active_)
        {
            ImGui::EndTable();
        }
    }

    void TableScope::nextRow()
    {
        if (!active_)
        {
            detail::failContract();
        }
        ImGui::TableNextRow();
    }

    void TableScope::nextColumn()
    {
        if (!active_)
        {
            detail::failContract();
        }
        ImGui::TableNextColumn();
    }

    void TableScope::headersRow()
    {
        if (!active_)
        {
            detail::failContract();
        }
        ImGui::TableHeadersRow();
    }

    TableScope::TableScope(const TableSpec& spec) noexcept
    {
        if (!spec.id.isValid() || spec.columns == 0U)
        {
            detail::failContract();
        }
        ImGuiTableFlags flags{};
        if (spec.borders)
        {
            flags |= ImGuiTableFlags_BordersInnerV;
        }
        if (spec.row_background)
        {
            flags |= ImGuiTableFlags_RowBg;
        }
        const detail::NullTerminatedText id_text{spec.id.name()};
        const bool open = ImGui::BeginTable(id_text.c_str(), static_cast<int>(spec.columns), flags);
        if (open && spec.first_column_width > 0.0F)
        {
            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, spec.first_column_width);
            for (std::uint32_t column = 1U; column < spec.columns; ++column)
            {
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            }
        }
        active_ = open;
    }

    void propertyRow(std::string_view label) noexcept
    {
        ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight());
        ImGui::TableSetColumnIndex(0);
        const char* begin = detail::dataOrEmpty(label);
        ImGui::TextUnformatted(begin, begin + label.size());
        ImGui::TableSetColumnIndex(1);
    }

}
