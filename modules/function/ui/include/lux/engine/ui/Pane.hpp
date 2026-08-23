#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include <lux/engine/function/visibility.h>
#include <lux/engine/object/Object.hpp>
#include <lux/engine/object/ObjectAnnotations.hpp>
#include <lux/engine/ui/UiIds.hpp>

namespace lux::ui
{
    namespace detail
    {
        struct PaneStateAccess;
    }

    struct PaneFocusChanged final
    {
        bool focused{false};
    };

    struct PaneVisibilityChanged final
    {
        bool visible{false};
    };

    class LUX_FUNCTION_PUBLIC PaneDrawContext final
    {
    public:
        void activateContext(UiContextIdView context);

    private:
        friend class UISession;
        explicit PaneDrawContext(std::vector<UiContextIdView> &active) noexcept
            : active_(std::addressof(active)) {}

        std::vector<UiContextIdView> *active_{nullptr};
    };

    class LUX_FUNCTION_PUBLIC LUX_OBJECT() Pane : public lux::object::Object<Pane>
    {
    public:
        static const signal_type<PaneFocusChanged> 		focusChanged;
        static const signal_type<PaneVisibilityChanged> visibilityChanged;

        Pane(lux::object::ObjectDispatcherRef dispatcher, PaneId id, PaneTypeId type, std::string title);
        ~Pane() override;

        [[nodiscard]] const PaneId &id() const noexcept { return id_; }
        [[nodiscard]] const PaneTypeId &type() const noexcept { return type_; }
        [[nodiscard]] std::string_view title() const noexcept { return title_; }
        [[nodiscard]] bool visible() const noexcept { return visible_; }
        [[nodiscard]] bool focused() const noexcept { return focused_; }
        [[nodiscard]] bool hovered() const noexcept { return hovered_; }

        void setTitle(std::string title);
        void setVisible(bool visible);

        /** Stable base contexts backed by this Pane for the Pane lifetime. */
        [[nodiscard]] virtual std::span<const UiContextIdView>
        contexts() const noexcept
        {
            return {};
        }

    protected:
        virtual void draw(PaneDrawContext &context) = 0;

    private:
        friend class UISession;
        friend struct detail::PaneStateAccess;
        void setFocused(bool focused);
        void setHovered(bool hovered) noexcept { hovered_ = hovered; }
        void rebuildImguiLabel();

        PaneId 		id_;
        PaneTypeId 	type_;
        std::string title_;
        std::string imgui_label_;
        bool 		visible_{true};
        bool 		focused_{false};
        bool 		hovered_{false};
    };
} // namespace lux::ui
