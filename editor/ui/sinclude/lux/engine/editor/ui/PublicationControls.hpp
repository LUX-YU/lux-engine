#include <lux/engine/editor/detail/SignalDelivery.hpp>
#pragma once

#include <lux/engine/editor/AssetSave.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>

namespace lux::editor::ui
{
    // Only the request identity and display are UI state. The tool owns publication work.
    template <class Tool> class TPublicationControls final : public lux::ui::Element
    {
    public:
        TPublicationControls(
            lux::ui::Element& parent,
            lux::ui::ElementId id,
            Tool& tool,
            EditorResult<void>& status
        )
            : lux::ui::Element(parent, std::move(id)), tool_(tool),
              layout_(*this, lux::ui::ElementId{"layout"}), message_(layout_, lux::ui::ElementId{"message"}),
              actions_(layout_, lux::ui::ElementId{"actions"}, lux::ui::ELayoutType::HORIZONTAL),
              retry_(actions_, lux::ui::ElementId{"retry"}, "Retry publication"),
              abandon_(actions_, lux::ui::ElementId{"abandon"}, "Abandon publication"),
              retry_connection_(lux::editor::detail::takeConnection(
                  lux::object::LuxObject::connect(
                      std::addressof(retry_),
                      &lux::ui::Button::activated,
                      [this]() noexcept { retry_requested_ = true; }
                  ),
                  status
              )),
              abandon_connection_(lux::editor::detail::takeConnection(
                  lux::object::LuxObject::connect(
                      std::addressof(abandon_),
                      &lux::ui::Button::activated,
                      [this]() noexcept { abandon_requested_ = true; }
                  ),
                  status
              ))
        {
            setStretch({1, 0});
            actions_.setVisible(false);
        }
        void track(SaveRequestId request) noexcept
        {
            request_ = request;
            retry_requested_ = abandon_requested_ = false;
            message_.setText("Publishing captured source and compiled asset...");
        }

    private:
        lux::ui::SizeHint sizeHintContent() noexcept override
        {
            return layout_.sizeHint();
        }
        lux::ui::SizeHint measureContent(float width) noexcept override
        {
            return layout_.measure(width);
        }
        void arrangeContent() noexcept override
        {
            layout_.arrange({{}, rect().size});
        }
        void draw() noexcept override
        {
            drawChild(layout_);
        }
        void update() noexcept override
        {
            if (!request_.serial)
                return;
            if (retry_requested_ || abandon_requested_)
            {
                const auto result =
                    abandon_requested_ ? tool_.abandonSave(request_) : tool_.retrySave(request_);
                if (!result)
                    message_.setText(result.error().domain + ": " + result.error().message);
                if (!result && result.error().code == EEditorError::BUSY)
                    return;
                retry_requested_ = abandon_requested_ = false;
            }
            const auto status = tool_.saveStatus(request_);
            if (!status)
            {
                message_.setText(status.error().message);
                return;
            }
            const auto* failed = std::get_if<SaveRetryable>(&*status);
            actions_.setVisible(failed != nullptr);
            if (failed)
            {
                retry_.setEnabled(failed->retry_allowed);
                message_.setText("Publication failed: " + failed->failure.domain + ": " + failed->failure.message);
                return;
            }
            if (const auto* done = std::get_if<SaveSucceeded>(&*status))
                message_.setText(
                    done->cleanup ? "Source and compiled asset published"
                                  : "Published; cleanup requires attention: " + done->cleanup.error().message
                );
            else if (std::holds_alternative<SaveAbandoned>(*status))
                message_.setText("Publication abandoned");
            else
                return;
            if (tool_.acknowledgeSave(request_))
                request_ = {};
        }
        Tool& tool_;
        SaveRequestId request_;
        bool retry_requested_{}, abandon_requested_{};
        lux::ui::Layout layout_;
        lux::ui::Label message_;
        lux::ui::Layout actions_;
        lux::ui::Button retry_, abandon_;
        object::Connection retry_connection_, abandon_connection_;
    };
}
