#pragma once

#include <span>
#include <string>
#include <vector>

#include <lux/engine/function/visibility.h>
#include <lux/engine/object/Object.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/ui_next/UiIds.hpp>

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

    class PaneDrawContext final
    {
      public:
        void activateContext(UiContextId context);

      private:
        friend class UISession;
        explicit PaneDrawContext(std::vector<UiContextId>& active) noexcept
            : active_(&active)
        {
        }

        std::vector<UiContextId>* active_{nullptr};
    };

    class LUX_FUNCTION_PUBLIC Pane : public lux::object::Object<Pane>
    {
      public:
        inline static constexpr signal_type<PaneFocusChanged>
            focusChanged{"focusChanged"};
        inline static constexpr signal_type<PaneVisibilityChanged>
            visibilityChanged{"visibilityChanged"};

        Pane(
            lux::object::ObjectDispatcher& dispatcher,
            PaneId id,
            PaneTypeId type,
            std::string title
        );
        ~Pane() override;

        [[nodiscard]] const PaneId& id() const noexcept { return id_; }
        [[nodiscard]] const PaneTypeId& type() const noexcept { return type_; }
        [[nodiscard]] std::string_view title() const noexcept { return title_; }
        [[nodiscard]] bool visible() const noexcept { return visible_; }
        [[nodiscard]] bool focused() const noexcept { return focused_; }
        [[nodiscard]] bool hovered() const noexcept { return hovered_; }

        void setVisible(bool visible);

        [[nodiscard]] virtual std::span<const UiContextId>
        contexts() const noexcept
        {
            return {};
        }

      protected:
        virtual void draw(PaneDrawContext& context) = 0;

      private:
        friend class UISession;
        friend struct detail::PaneStateAccess;
        void setFocused(bool focused);
        void setHovered(bool hovered) noexcept { hovered_ = hovered; }

        PaneId id_;
        PaneTypeId type_;
        std::string title_;
        bool visible_{true};
        bool focused_{false};
        bool hovered_{false};
    };
}
