#include <lux/engine/editor/ui/asset/AssetPickerElement.hpp>
#include <lux/engine/editor/ui/asset/AssetDragDrop.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <imgui.h>

namespace lux::editor::ui
{
    AssetPickerElement::AssetPickerElement(
        lux::ui::Element& parent,
        lux::ui::ElementId id,
        const ProjectStorage* project,
        std::uint32_t required_magic,
        asset::AssetId value
    )
        : Element(parent, std::move(id)), project_(project), required_magic_(required_magic), value_(value)
    {
        setStretch({1, 0});
    }
    void AssetPickerElement::setValue(asset::AssetId value) noexcept
    {
        if (value_ == value)
            return;
        value_ = value;
        error_.reset();
    }
    void AssetPickerElement::adopt(asset::AssetId value) noexcept
    {
        error_.reset();
        if (value == value_)
            return;
        value_ = value;
        detail::reportSignalDelivery(emit(edited, lux::ui::EditResult{true, true, true, false}), "asset-picker.edited");
    }
    EditorResult<void> AssetPickerElement::select(AssetReference reference) noexcept
    {
        if (!enabled() || !project_ || !required_magic_)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "asset-picker.select"});
        auto selected = project_->resolveReference(reference, required_magic_);
        if (!selected)
        {
            error_ = selected.error();
            return lux::cxx::unexpected(selected.error());
        }
        adopt(*selected);
        return {};
    }
    lux::ui::SizeHint AssetPickerElement::sizeHintContent() noexcept
    {
        const float height = ImGui::GetIO().Fonts->Fonts[0]->FontSize + ImGui::GetStyle().FramePadding.y * 2;
        return {{40, height}, {200, height}, {std::numeric_limits<float>::infinity(), height}};
    }
    void AssetPickerElement::draw() noexcept
    {
        const auto name = project_ ? project_->assetName(value_) : std::string_view{"No project"};
        ImGui::BeginDisabled(!project_ || !required_magic_);
        // A stable ID keeps an open popup valid when the catalog's display name changes.
        const auto position = ImGui::GetCursorScreenPos();
        if (ImGui::Button("##asset", {rect().size.width, rect().size.height}))
            ImGui::OpenPopup("asset-picker");
        const auto padding = ImGui::GetStyle().FramePadding;
        const ImVec4 clip{position.x, position.y, position.x + rect().size.width, position.y + rect().size.height};
        ImGui::GetWindowDrawList()->AddText(
            nullptr,
            0,
            {position.x + padding.x, position.y + padding.y},
            ImGui::GetColorU32(ImGuiCol_Text),
            name.data(),
            name.data() + name.size(),
            0,
            &clip
        );
        if (ImGui::BeginDragDropTarget())
        {
            if (const auto* payload = ImGui::AcceptDragDropPayload(kAssetReferencePayload))
            {
                auto decoded = decodeAssetReference(
                    {static_cast<const std::byte*>(payload->Data), static_cast<std::size_t>(payload->DataSize)}
                );
                if (decoded)
                    static_cast<void>(select(*decoded));
                else
                    error_ = decoded.error();
            }
            ImGui::EndDragDropTarget();
        }
        if (project_ && ImGui::BeginPopup("asset-picker"))
        {
            if (ImGui::Selectable("None", value_.isNull()))
                adopt({});
            const auto rows = project_->catalog();
            for (std::size_t index{}; index < rows.size(); ++index)
            {
                const auto& row = rows[index];
                if (row.magic != required_magic_)
                    continue;
                ImGui::PushID(static_cast<int>(index));
                if (ImGui::Selectable(row.path.c_str(), row.id == value_))
                    static_cast<void>(select(project_->reference(row.id)));
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        ImGui::EndDisabled();
        if (error_ && ImGui::IsItemHovered())
            ImGui::SetTooltip("Asset selection rejected: %s", error_->domain.c_str());
    }
}
