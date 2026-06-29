#pragma once
// ============================================================================
//  ReflectedFieldTable.hpp — the 2-column (Field | Value) ImGui table scaffold
//  shared by every reflection-driven inspector (the editor's InspectorPanel and
//  SceneFeatureSettingPanel both draw a RefClass this way).
//
//  Only the per-field renderer differs between callers, so it is a callback:
//  the table setup / columns / header row / EndTable live here once.
// ============================================================================

#include <lux/engine/meta/Meta.hpp>   // RefClass / RefField

#include <imgui.h>

namespace lux::ui
{
    /**
     * @brief Draw a reflected struct's fields as a 2-column (Field | Value) table.
     *
     *        Sets up the table (inner borders / row-bg / stretch sizing), the two
     *        stretch columns split by @p field_col_frac, and the header row, then
     *        invokes @p drawField(field, instance) for every field. The callback
     *        owns its own row and both columns — exactly the contract of
     *        @ref WidgetDispatch::draw.
     *
     * @param table_id       Unique ImGui id for the table (per struct instance).
     * @param rc             The reflected class whose `fields` are drawn.
     * @param instance       Pointer to the struct's storage (field offsets apply).
     * @param field_col_frac Width fraction of the "Field" column, in [0,1].
     * @param drawField      Callable `void(const lux::meta::RefField&, void*)`.
     */
    template <class DrawField>
    void drawReflectedFieldTable(const char*                table_id,
                                 const lux::meta::RefClass& rc,
                                 void*                      instance,
                                 float                      field_col_frac,
                                 DrawField&&                drawField)
    {
        constexpr ImGuiTableFlags kFlags =
            ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_RowBg         |
            ImGuiTableFlags_SizingStretchProp;

        if (ImGui::BeginTable(table_id, 2, kFlags))
        {
            ImGui::TableSetupColumn("Field",
                ImGuiTableColumnFlags_WidthStretch, field_col_frac);
            ImGui::TableSetupColumn("Value",
                ImGuiTableColumnFlags_WidthStretch, 1.0f - field_col_frac);
            ImGui::TableHeadersRow();

            for (const auto& field : rc.fields)
                drawField(field, instance);

            ImGui::EndTable();
        }
    }

} // namespace lux::ui
