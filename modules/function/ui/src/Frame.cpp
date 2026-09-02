#include <lux/engine/ui/Frame.hpp>
#include <lux/engine/ui/Theme.hpp>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <lux/engine/ui/detail/DragDropEncoding.hpp>
#include <lux/engine/ui/detail/EditLifecycle.hpp>
#include <lux/engine/ui/detail/UiContract.hpp>

#include <algorithm>
#include <type_traits>
#include <utility>

namespace lux::ui
{
    namespace
    {
        template<class Type>
        [[nodiscard]] EditResult editResult(bool changed) noexcept
        {
            static_cast<void>(sizeof(Type));
            return EditResult{
                changed,
                ImGui::IsItemActivated(),
                ImGui::IsItemDeactivatedAfterEdit(),
                false
            };
        }

        template<class Type>
        [[nodiscard]] EditResult editScalarImpl(
            std::string_view label,
            Type& value,
            const ScalarEditSpec<Type>& spec,
            ImGuiDataType data_type
        )
        {
            const void* minimum = spec.minimum ? std::addressof(*spec.minimum) : nullptr;
            const void* maximum = spec.maximum ? std::addressof(*spec.maximum) : nullptr;
            const void* step = spec.step ? std::addressof(*spec.step) : nullptr;
            const char* format = spec.format.empty() ? nullptr : spec.format.data();
            bool changed{};
            switch (spec.mode)
            {
            case EScalarEditMode::INPUT:
                changed = ImGui::InputScalar(label.data(), data_type, std::addressof(value), step, nullptr, format);
                break;
            case EScalarEditMode::DRAG:
                changed = ImGui::DragScalar(
                    label.data(),
                    data_type,
                    std::addressof(value),
                    spec.speed,
                    minimum,
                    maximum,
                    format
                );
                break;
            case EScalarEditMode::SLIDER:
                if (minimum != nullptr && maximum != nullptr)
                {
                    changed = ImGui::SliderScalar(
                        label.data(),
                        data_type,
                        std::addressof(value),
                        minimum,
                        maximum,
                        format
                    );
                }
                else
                {
                    changed = ImGui::DragScalar(
                        label.data(),
                        data_type,
                        std::addressof(value),
                        spec.speed,
                        minimum,
                        maximum,
                        format
                    );
                }
                break;
            }
            return editResult<Type>(changed);
        }

        void requireActive(const Frame& frame)
        {
            if (frame.theme().metrics.row_height <= 0.0F)
                detail::failUiContract();
        }
    } // namespace

    DisabledScope::DisabledScope(DisabledScope&& other) noexcept : active_(std::exchange(other.active_, false)) {}

    DisabledScope& DisabledScope::operator=(DisabledScope&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            if (active_)
                ImGui::EndDisabled();
            active_ = std::exchange(other.active_, false);
        }
        return *this;
    }

    DisabledScope::~DisabledScope() noexcept
    {
        if (active_)
            ImGui::EndDisabled();
    }

    IdScope::IdScope(IdScope&& other) noexcept : active_(std::exchange(other.active_, false)) {}

    IdScope& IdScope::operator=(IdScope&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            if (active_)
                ImGui::PopID();
            active_ = std::exchange(other.active_, false);
        }
        return *this;
    }

    IdScope::~IdScope() noexcept
    {
        if (active_)
            ImGui::PopID();
    }

    ChildScope::ChildScope(ChildScope&& other) noexcept
        : active_(std::exchange(other.active_, false)), visible_(other.visible_)
    {
    }

    ChildScope& ChildScope::operator=(ChildScope&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            if (active_)
                ImGui::EndChild();
            active_ = std::exchange(other.active_, false);
            visible_ = other.visible_;
        }
        return *this;
    }

    ChildScope::~ChildScope() noexcept
    {
        if (active_)
            ImGui::EndChild();
    }

    TableScope::TableScope(TableScope&& other) noexcept : active_(std::exchange(other.active_, false)) {}

    TableScope& TableScope::operator=(TableScope&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            if (active_)
                ImGui::EndTable();
            active_ = std::exchange(other.active_, false);
        }
        return *this;
    }

    TableScope::~TableScope() noexcept
    {
        if (active_)
            ImGui::EndTable();
    }

    void TableScope::nextRow()
    {
        if (!active_)
            detail::failUiContract();
        ImGui::TableNextRow();
    }

    void TableScope::nextColumn()
    {
        if (!active_)
            detail::failUiContract();
        ImGui::TableNextColumn();
    }

    void TableScope::headersRow()
    {
        if (!active_)
            detail::failUiContract();
        ImGui::TableHeadersRow();
    }

    TreeRowScope::TreeRowScope(TreeRowScope&& other) noexcept
        : open_(other.open_), pushed_(std::exchange(other.pushed_, false)), activated_(other.activated_),
          context_requested_(other.context_requested_)
    {
    }

    TreeRowScope& TreeRowScope::operator=(TreeRowScope&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            if (pushed_)
                ImGui::TreePop();
            open_ = other.open_;
            pushed_ = std::exchange(other.pushed_, false);
            activated_ = other.activated_;
            context_requested_ = other.context_requested_;
        }
        return *this;
    }

    TreeRowScope::~TreeRowScope() noexcept
    {
        if (pushed_)
            ImGui::TreePop();
    }

    PopupScope::PopupScope(PopupScope&& other) noexcept
        : active_(std::exchange(other.active_, false)), modal_(other.modal_)
    {
    }

    PopupScope& PopupScope::operator=(PopupScope&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            if (active_)
                ImGui::EndPopup();
            active_ = std::exchange(other.active_, false);
            modal_ = other.modal_;
        }
        return *this;
    }

    PopupScope::~PopupScope() noexcept
    {
        if (active_)
            ImGui::EndPopup();
    }

    void PopupScope::close() noexcept
    {
        if (active_)
            ImGui::CloseCurrentPopup();
    }

    DragSourceScope::DragSourceScope(DragSourceScope&& other) noexcept
        : active_(std::exchange(other.active_, false))
    {
    }

    DragSourceScope& DragSourceScope::operator=(DragSourceScope&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            if (active_)
                ImGui::EndDragDropSource();
            active_ = std::exchange(other.active_, false);
        }
        return *this;
    }

    DragSourceScope::~DragSourceScope() noexcept
    {
        if (active_)
            ImGui::EndDragDropSource();
    }

    void DragSourceScope::setPayload(PayloadTypeIdView type, std::span<const std::byte> bytes)
    {
        if (!active_)
            detail::failUiContract();
        setDragDropPayload(type, bytes);
    }

    DropTargetScope::DropTargetScope(DropTargetScope&& other) noexcept
        : active_(std::exchange(other.active_, false))
    {
    }

    DropTargetScope& DropTargetScope::operator=(DropTargetScope&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            if (active_)
                ImGui::EndDragDropTarget();
            active_ = std::exchange(other.active_, false);
        }
        return *this;
    }

    DropTargetScope::~DropTargetScope() noexcept
    {
        if (active_)
            ImGui::EndDragDropTarget();
    }

    std::optional<DragDropPayloadView> DropTargetScope::accept()
    {
        return active_ ? detail::acceptDragDropPayload() : std::nullopt;
    }

    void Frame::text(std::string_view value)
    {
        requireActive(*this);
        ImGui::TextUnformatted(value.data(), value.data() + value.size());
    }

    void Frame::textMuted(std::string_view value)
    {
        requireActive(*this);
        ImGui::TextDisabled("%.*s", static_cast<int>(value.size()), value.data());
    }

    void Frame::textWrapped(std::string_view value)
    {
        requireActive(*this);
        ImGui::TextWrapped("%.*s", static_cast<int>(value.size()), value.data());
    }

    bool Frame::button(std::string_view label)
    {
        requireActive(*this);
        return ImGui::Button(label.data());
    }

    bool Frame::smallButton(std::string_view label)
    {
        requireActive(*this);
        return ImGui::SmallButton(label.data());
    }

    EditResult Frame::checkbox(std::string_view label, bool& value)
    {
        requireActive(*this);
        return editResult<bool>(ImGui::Checkbox(label.data(), std::addressof(value)));
    }

    EditResult Frame::inputText(std::string_view label, std::string& value, InputTextSpec spec)
    {
        requireActive(*this);
        const auto flags = spec.read_only ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None;
        const bool changed = spec.hint.empty() ? ImGui::InputText(label.data(), std::addressof(value), flags) :
                                                 ImGui::InputTextWithHint(
                                                     label.data(),
                                                     spec.hint.data(),
                                                     std::addressof(value),
                                                     flags
                                                 );
        return editResult<std::string>(changed);
    }

    EditResult Frame::editScalar(
        std::string_view label,
        std::int32_t& value,
        const ScalarEditSpec<std::int32_t>& spec
    )
    {
        requireActive(*this);
        return editScalarImpl(label, value, spec, ImGuiDataType_S32);
    }

    EditResult Frame::editScalar(
        std::string_view label,
        std::uint32_t& value,
        const ScalarEditSpec<std::uint32_t>& spec
    )
    {
        requireActive(*this);
        return editScalarImpl(label, value, spec, ImGuiDataType_U32);
    }

    EditResult Frame::editScalar(
        std::string_view label,
        std::int64_t& value,
        const ScalarEditSpec<std::int64_t>& spec
    )
    {
        requireActive(*this);
        return editScalarImpl(label, value, spec, ImGuiDataType_S64);
    }

    EditResult Frame::editScalar(
        std::string_view label,
        std::uint64_t& value,
        const ScalarEditSpec<std::uint64_t>& spec
    )
    {
        requireActive(*this);
        return editScalarImpl(label, value, spec, ImGuiDataType_U64);
    }

    EditResult Frame::editScalar(std::string_view label, float& value, const ScalarEditSpec<float>& spec)
    {
        requireActive(*this);
        return editScalarImpl(label, value, spec, ImGuiDataType_Float);
    }

    EditResult Frame::editScalar(std::string_view label, double& value, const ScalarEditSpec<double>& spec)
    {
        requireActive(*this);
        return editScalarImpl(label, value, spec, ImGuiDataType_Double);
    }

    EditResult Frame::editChoice(
        std::string_view label,
        std::int64_t& value,
        std::span<const ComboOption> options
    )
    {
        requireActive(*this);
        const auto selected = std::ranges::find(options, value, &ComboOption::value);
        const char* preview = selected == options.end() ? "<unknown>" : selected->label.data();
        bool changed{};
        if (ImGui::BeginCombo(label.data(), preview))
        {
            for (const auto& option : options)
            {
                const bool current = option.value == value;
                if (ImGui::Selectable(option.label.data(), current))
                {
                    value = option.value;
                    changed = true;
                }
                if (current)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        return detail::atomicEditResult(changed);
    }

    bool Frame::lastItemHovered() const noexcept
    {
        return ImGui::IsItemHovered();
    }

    bool Frame::lastItemFocused() const noexcept
    {
        return ImGui::IsItemFocused();
    }

    void Frame::tooltip(std::string_view value)
    {
        requireActive(*this);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%.*s", static_cast<int>(value.size()), value.data());
    }

    DisabledScope Frame::disabled(bool disabled)
    {
        requireActive(*this);
        if (disabled)
            ImGui::BeginDisabled();
        return DisabledScope{disabled};
    }

    IdScope Frame::id(WidgetIdView id)
    {
        requireActive(*this);
        if (!id.isValid())
            detail::failUiContract();
        ImGui::PushID(id.name().data(), id.name().data() + id.name().size());
        return IdScope{true};
    }

    ChildScope Frame::child(const ChildSpec& spec)
    {
        requireActive(*this);
        if (!spec.id.isValid())
            detail::failUiContract();
        const bool visible = ImGui::BeginChild(
            spec.id.name().data(),
            ImVec2{spec.size.width, spec.size.height},
            spec.border
        );
        return ChildScope{true, visible};
    }

    TableScope Frame::table(const TableSpec& spec)
    {
        requireActive(*this);
        if (!spec.id.isValid() || spec.columns == 0U)
            detail::failUiContract();
        ImGuiTableFlags flags{};
        if (spec.borders)
            flags |= ImGuiTableFlags_BordersInnerV;
        if (spec.row_background)
            flags |= ImGuiTableFlags_RowBg;
        const bool open = ImGui::BeginTable(spec.id.name().data(), static_cast<int>(spec.columns), flags);
        return TableScope{open};
    }

    void Frame::propertyRow(std::string_view label)
    {
        requireActive(*this);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(label.data(), label.data() + label.size());
        ImGui::TableSetColumnIndex(1);
    }

    TreeRowScope Frame::treeRow(const TreeRowSpec& spec)
    {
        requireActive(*this);
        if (!spec.id.isValid())
            detail::failUiContract();
        auto id_scope = id(spec.id);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (spec.selected)
            flags |= ImGuiTreeNodeFlags_Selected;
        if (spec.leaf)
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (spec.default_open)
            flags |= ImGuiTreeNodeFlags_DefaultOpen;
        const bool open = ImGui::TreeNodeEx(spec.label.data(), flags);
        const bool activated = ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen();
        const bool context_requested = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        return TreeRowScope{open, open && !spec.leaf, activated, context_requested};
    }

    void Frame::openPopup(WidgetIdView id)
    {
        requireActive(*this);
        if (!id.isValid())
            detail::failUiContract();
        ImGui::OpenPopup(id.name().data());
    }

    PopupScope Frame::popup(const PopupSpec& spec)
    {
        requireActive(*this);
        if (!spec.id.isValid())
            detail::failUiContract();
        const bool open = spec.modal ? ImGui::BeginPopupModal(spec.id.name().data()) :
                                       ImGui::BeginPopup(spec.id.name().data());
        return PopupScope{open, spec.modal};
    }

    DragSourceScope Frame::dragSource()
    {
        requireActive(*this);
        return DragSourceScope{ImGui::BeginDragDropSource()};
    }

    DropTargetScope Frame::dropTarget()
    {
        requireActive(*this);
        return DropTargetScope{ImGui::BeginDragDropTarget()};
    }
} // namespace lux::ui
