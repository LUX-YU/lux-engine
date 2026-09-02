#pragma once

#include <lux/engine/function/visibility.h>
#include <lux/engine/ui/DragDrop.hpp>
#include <lux/engine/ui/Geometry.hpp>
#include <lux/engine/ui/UiIds.hpp>
#include <lux/engine/ui/ValueEdit.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace lux::ui
{
    class Theme;
    class UISession;

    struct ChildSpec final
    {
        WidgetIdView id;
        Size size;
        bool border{};
    };

    struct TableSpec final
    {
        WidgetIdView id;
        std::uint32_t columns{};
        bool headers{};
        bool borders{true};
        bool row_background{true};
    };

    struct TreeRowSpec final
    {
        WidgetIdView id;
        std::string_view label;
        bool selected{};
        bool leaf{};
        bool default_open{};
    };

    struct PopupSpec final
    {
        WidgetIdView id;
        bool modal{};
    };

    struct FrameInfo final
    {
        Size display_size;
        float delta_seconds{};
    };

    class LUX_FUNCTION_PUBLIC DisabledScope final
    {
    public:
        DisabledScope(const DisabledScope&) = delete;
        DisabledScope& operator=(const DisabledScope&) = delete;
        DisabledScope(DisabledScope&& other) noexcept;
        DisabledScope& operator=(DisabledScope&& other) noexcept;
        ~DisabledScope() noexcept;

    private:
        friend class Frame;
        explicit DisabledScope(bool active) noexcept : active_(active) {}
        bool active_{};
    };

    class LUX_FUNCTION_PUBLIC IdScope final
    {
    public:
        IdScope(const IdScope&) = delete;
        IdScope& operator=(const IdScope&) = delete;
        IdScope(IdScope&& other) noexcept;
        IdScope& operator=(IdScope&& other) noexcept;
        ~IdScope() noexcept;

    private:
        friend class Frame;
        explicit IdScope(bool active) noexcept : active_(active) {}
        bool active_{};
    };

    class LUX_FUNCTION_PUBLIC ChildScope final
    {
    public:
        ChildScope(const ChildScope&) = delete;
        ChildScope& operator=(const ChildScope&) = delete;
        ChildScope(ChildScope&& other) noexcept;
        ChildScope& operator=(ChildScope&& other) noexcept;
        ~ChildScope() noexcept;
        [[nodiscard]] bool visible() const noexcept { return visible_; }

    private:
        friend class Frame;
        ChildScope(bool active, bool visible) noexcept : active_(active), visible_(visible) {}
        bool active_{};
        bool visible_{};
    };

    class LUX_FUNCTION_PUBLIC TableScope final
    {
    public:
        TableScope(const TableScope&) = delete;
        TableScope& operator=(const TableScope&) = delete;
        TableScope(TableScope&& other) noexcept;
        TableScope& operator=(TableScope&& other) noexcept;
        ~TableScope() noexcept;
        [[nodiscard]] bool visible() const noexcept { return active_; }
        void nextRow();
        void nextColumn();
        void headersRow();

    private:
        friend class Frame;
        explicit TableScope(bool active) noexcept : active_(active) {}
        bool active_{};
    };

    class LUX_FUNCTION_PUBLIC TreeRowScope final
    {
    public:
        TreeRowScope(const TreeRowScope&) = delete;
        TreeRowScope& operator=(const TreeRowScope&) = delete;
        TreeRowScope(TreeRowScope&& other) noexcept;
        TreeRowScope& operator=(TreeRowScope&& other) noexcept;
        ~TreeRowScope() noexcept;
        [[nodiscard]] bool open() const noexcept { return open_; }
        [[nodiscard]] bool activated() const noexcept { return activated_; }
        [[nodiscard]] bool contextRequested() const noexcept { return context_requested_; }

    private:
        friend class Frame;
        TreeRowScope(bool open, bool pushed, bool activated, bool context_requested) noexcept
            : open_(open), pushed_(pushed), activated_(activated), context_requested_(context_requested)
        {
        }
        bool open_{};
        bool pushed_{};
        bool activated_{};
        bool context_requested_{};
    };

    class LUX_FUNCTION_PUBLIC PopupScope final
    {
    public:
        PopupScope(const PopupScope&) = delete;
        PopupScope& operator=(const PopupScope&) = delete;
        PopupScope(PopupScope&& other) noexcept;
        PopupScope& operator=(PopupScope&& other) noexcept;
        ~PopupScope() noexcept;
        [[nodiscard]] bool visible() const noexcept { return active_; }
        void close() noexcept;

    private:
        friend class Frame;
        PopupScope(bool active, bool modal) noexcept : active_(active), modal_(modal) {}
        bool active_{};
        bool modal_{};
    };

    class LUX_FUNCTION_PUBLIC DragSourceScope final
    {
    public:
        DragSourceScope(const DragSourceScope&) = delete;
        DragSourceScope& operator=(const DragSourceScope&) = delete;
        DragSourceScope(DragSourceScope&& other) noexcept;
        DragSourceScope& operator=(DragSourceScope&& other) noexcept;
        ~DragSourceScope() noexcept;
        [[nodiscard]] bool active() const noexcept { return active_; }
        void setPayload(PayloadTypeIdView type, std::span<const std::byte> bytes);

    private:
        friend class Frame;
        explicit DragSourceScope(bool active) noexcept : active_(active) {}
        bool active_{};
    };

    class LUX_FUNCTION_PUBLIC DropTargetScope final
    {
    public:
        DropTargetScope(const DropTargetScope&) = delete;
        DropTargetScope& operator=(const DropTargetScope&) = delete;
        DropTargetScope(DropTargetScope&& other) noexcept;
        DropTargetScope& operator=(DropTargetScope&& other) noexcept;
        ~DropTargetScope() noexcept;
        [[nodiscard]] bool active() const noexcept { return active_; }
        [[nodiscard]] std::optional<DragDropPayloadView> accept();

    private:
        friend class Frame;
        explicit DropTargetScope(bool active) noexcept : active_(active) {}
        bool active_{};
    };

    class LUX_FUNCTION_PUBLIC Frame final
    {
    public:
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;
        Frame(Frame&& other) noexcept;
        Frame& operator=(Frame&& other) noexcept;
        ~Frame() noexcept;

        void drawPanes();
        void finish() noexcept;
        [[nodiscard]] const Theme& theme() const noexcept;

        void text(std::string_view value);
        void textMuted(std::string_view value);
        void textWrapped(std::string_view value);
        [[nodiscard]] bool button(std::string_view label);
        [[nodiscard]] bool smallButton(std::string_view label);
        [[nodiscard]] EditResult checkbox(std::string_view label, bool& value);
        [[nodiscard]] EditResult inputText(std::string_view label, std::string& value, InputTextSpec spec = {});
        [[nodiscard]] EditResult editScalar(
            std::string_view label,
            std::int32_t& value,
            const ScalarEditSpec<std::int32_t>& spec = {}
        );
        [[nodiscard]] EditResult editScalar(
            std::string_view label,
            std::uint32_t& value,
            const ScalarEditSpec<std::uint32_t>& spec = {}
        );
        [[nodiscard]] EditResult editScalar(
            std::string_view label,
            std::int64_t& value,
            const ScalarEditSpec<std::int64_t>& spec = {}
        );
        [[nodiscard]] EditResult editScalar(
            std::string_view label,
            std::uint64_t& value,
            const ScalarEditSpec<std::uint64_t>& spec = {}
        );
        [[nodiscard]] EditResult editScalar(
            std::string_view label,
            float& value,
            const ScalarEditSpec<float>& spec = {}
        );
        [[nodiscard]] EditResult editScalar(
            std::string_view label,
            double& value,
            const ScalarEditSpec<double>& spec = {}
        );
        [[nodiscard]] EditResult editChoice(
            std::string_view label,
            std::int64_t& value,
            std::span<const ComboOption> options
        );
        [[nodiscard]] bool lastItemHovered() const noexcept;
        [[nodiscard]] bool lastItemFocused() const noexcept;
        void tooltip(std::string_view value);
        [[nodiscard]] DisabledScope disabled(bool disabled);
        [[nodiscard]] IdScope id(WidgetIdView id);
        [[nodiscard]] ChildScope child(const ChildSpec& spec);
        [[nodiscard]] TableScope table(const TableSpec& spec);
        void propertyRow(std::string_view label);
        [[nodiscard]] TreeRowScope treeRow(const TreeRowSpec& spec);
        void openPopup(WidgetIdView id);
        [[nodiscard]] PopupScope popup(const PopupSpec& spec);
        [[nodiscard]] DragSourceScope dragSource();
        [[nodiscard]] DropTargetScope dropTarget();

    private:
        friend class UISession;
        explicit Frame(UISession& session) noexcept;

        UISession* session_{};
    };
} // namespace lux::ui
