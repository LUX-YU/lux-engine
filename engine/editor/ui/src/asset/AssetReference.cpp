#include <imgui.h>
#include <lux/engine/editor/gui/asset/AssetReference.hpp>
#include <lux/engine/editor/project/Project.hpp>

namespace lux::editor::gui
{
EditorResult<bool> drawAssetReference(const Project &project, asset::AssetId &value, std::uint32_t required_magic)
{
    const auto name = project.assetName(value);
    if (!required_magic)
    {
        ImGui::TextUnformatted(name.data(), name.data() + name.size());
        return false;
    }

    EditorResult<bool> result{false};
    auto next = value;
    const auto select = [&](AssetReference reference) {
        const auto accepted = project.resolveReference(reference, required_magic);
        if (!accepted)
        {
            result = lux::cxx::unexpected(accepted.error());
            return;
        }
        next = *accepted;
    };
    if (ImGui::Button(name.data(), {-1, 0}))
    {
        ImGui::OpenPopup("asset-picker");
    }
    if (ImGui::BeginDragDropTarget())
    {
        if (const auto *payload = ImGui::AcceptDragDropPayload(kAssetReferencePayload))
        {
            const auto decoded = decodeAssetReference(
                {static_cast<const std::byte *>(payload->Data), static_cast<std::size_t>(payload->DataSize)});
            if (decoded)
            {
                select(*decoded);
            }
            else
            {
                result = lux::cxx::unexpected(decoded.error());
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopup("asset-picker"))
    {
        if (ImGui::Selectable("None", value.isNull()))
        {
            next = {};
            ImGui::CloseCurrentPopup();
        }
        const auto rows = project.catalog();
        for (std::size_t index{}; index < rows.size(); ++index)
        {
            const auto &row = rows[index];
            if (row.magic != required_magic)
            {
                continue;
            }
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::Selectable(row.path.c_str(), row.id == value))
            {
                select(project.reference(row.id));
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    if (result)
    {
        result = value != next;
        value = next;
    }
    return result;
}
} // namespace lux::editor::gui
