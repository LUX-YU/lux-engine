#include <lux/engine/editor/project/AssetPickerElement.hpp>
#include <imgui.h>
#include <algorithm>

namespace lux::editor::project
{
    AssetPickerElement::AssetPickerElement(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::ElementId id,
        ProjectCatalogModel* query,
        std::uint32_t magic,
        asset::AssetId value
    )
        : Element(std::move(dispatcher), std::move(id)), query_(query), required_magic_(magic), value_(value)
    {
        setStretch({1, 0});
        if (query_)
        {
            auto connected =
                object::LuxObject::connect(query_, &ProjectCatalogModel::changed, [this](std::uint64_t) noexcept {
                    refresh_requested_ = true;
                });
            if (!connected)
                std::terminate();
            changes_ = std::move(*connected);
        }
        static_cast<void>(refresh());
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
        if (value_ == value)
            return;
        value_ = value;
        delivery_ = emit(edited, lux::ui::EditResult{true, true, true, false});
    }
    ProjectQueryResult<void> AssetPickerElement::select(AssetReference reference)
    {
        if (!enabled() || !query_ || !required_magic_)
            return lux::cxx::unexpected(VProjectQueryFailure{EProjectQueryError::UNBOUND});
        auto selected = query_->resolve(reference, required_magic_);
        if (!selected)
        {
            error_ = selected.error();
            return lux::cxx::unexpected(selected.error());
        }
        adopt(*selected);
        return {};
    }
    ProjectQueryResult<void> AssetPickerElement::refresh()
    {
        if (!query_)
            return lux::cxx::unexpected(VProjectQueryFailure{EProjectQueryError::UNBOUND});
        auto version = query_->version();
        if (!version)
        {
            error_ = version.error();
            return lux::cxx::unexpected(version.error());
        }
        if (*version == catalog_.version())
        {
            error_.reset();
            return {};
        }
        auto candidate = query_->snapshot();
        if (!candidate)
        {
            error_ = candidate.error();
            return lux::cxx::unexpected(candidate.error());
        }
        catalog_ = std::move(*candidate);
        error_.reset();
        return {};
    }
    void AssetPickerElement::update() noexcept
    {
        if (!query_)
            return;
        const auto revision = query_->version();
        const bool needs_refresh = refresh_requested_ || !revision || *revision != catalog_.version() || error_;
        if (needs_refresh)
        {
            refresh_requested_ = false;
            static_cast<void>(refresh());
        }
    }
    lux::ui::SizeHint AssetPickerElement::sizeHintContent() noexcept
    {
        const float height = ImGui::GetIO().Fonts->Fonts[0]->FontSize + ImGui::GetStyle().FramePadding.y * 2;
        return {{40, height}, {200, height}, {std::numeric_limits<float>::infinity(), height}};
    }
    void AssetPickerElement::draw() noexcept
    {
        const auto found = std::ranges::find(catalog_.assets(), value_, &AssetCatalogEntry::id);
        const std::string_view name = found != catalog_.assets().end() ? found->path
                                      : value_.isNull()              ? "None"
                                                                     : "Missing asset";
        ImGui::BeginDisabled(!query_ || !required_magic_);
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
        if (query_ && ImGui::BeginPopup("asset-picker"))
        {
            if (ImGui::Selectable("None", value_.isNull()))
                adopt({});
            for (std::size_t index{}; index < catalog_.assets().size(); ++index)
            {
                const auto& row = catalog_.assets()[index];
                if (row.magic != required_magic_)
                    continue;
                ImGui::PushID(static_cast<int>(index));
                if (ImGui::Selectable(row.path.c_str(), row.id == value_))
                    static_cast<void>(select(catalog_.reference(row.id)));
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        ImGui::EndDisabled();
        if (error_ && ImGui::IsItemHovered())
            ImGui::SetTooltip("Asset query/selection failed; the previous catalog and value are retained.");
    }
}
