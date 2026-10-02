#include <lux/engine/editor/desktop/ReviewView.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <algorithm>

namespace lux::editor::desktop
{
    namespace
    {
        const char* label(EReviewChoice choice) noexcept
        {
            using enum EReviewChoice;
            switch (choice)
            {
            case SAVE:
                return "Save";
            case DISCARD:
                return "Discard changes";
            case CANCEL:
                return "Cancel";
            case KEEP_CONTENT:
                return "Keep content open";
            case CLOSE_CONTENT:
                return "Close content";
            case STOP_RUN:
                return "Stop run";
            case KEEP_RUN:
                return "Keep frozen run";
            }
            return nullptr;
        }
    }
    struct ReviewView::Impl final
    {
        ReviewQuestion question_;
        std::optional<ReviewAnswer> answer_;
        lux::ui::Layout body_;
        lux::ui::Label message_;
        lux::ui::Layout actions_;
        std::vector<std::unique_ptr<lux::ui::Button>> buttons_;
        std::vector<object::Connection> connections_;
        std::optional<views::ViewPreparationFailure> failure_;
        Impl(ReviewView& view, ReviewQuestion question)
            : question_(std::move(question)), body_(view, lux::ui::ElementId{"body"}),
              message_(body_, lux::ui::ElementId{"message"}, question_.message),
              actions_(body_, lux::ui::ElementId{"choices"}, lux::ui::ELayoutType::HORIZONTAL)
        {
            message_.setWrap(true);
            view.setContent(body_);
            view.setModal(true);
            buttons_.reserve(question_.choices.size());
            connections_.reserve(question_.choices.size() + 1);
            for (auto choice : question_.choices)
            {
                auto button =
                    std::make_unique<lux::ui::Button>(actions_, lux::ui::ElementId{label(choice)}, label(choice));
                auto connected =
                    object::LuxObject::connect(button.get(), &lux::ui::Button::activated, [&view, choice]() noexcept {
                        (void)view.answer(choice);
                    });
                if (!connected)
                {
                    failure_ = views::ViewPreparationFailure{
                        "object.connect",
                        static_cast<std::uint64_t>(connected.error()),
                        "Review button connection failed"
                    };
                    return;
                }
                buttons_.push_back(std::move(button));
                connections_.push_back(std::move(*connected));
            }
            auto connected = object::LuxObject::connect(&view, &lux::ui::Pane::closeRequested, [&view]() noexcept {
                (void)view.answer(EReviewChoice::CANCEL);
            });
            if (!connected)
                failure_ = views::ViewPreparationFailure{
                    "object.connect",
                    static_cast<std::uint64_t>(connected.error()),
                    "Review close connection failed"
                };
            else
                connections_.push_back(std::move(*connected));
        }
    };
    ReviewView::ReviewView(object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id, ReviewQuestion question)
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{"lux.editor.review"}, question.title),
          impl_(std::make_unique<Impl>(*this, std::move(question)))
    {}
    ReviewView::~ReviewView() noexcept = default;
    cxx::expected<std::unique_ptr<ReviewView>, views::ViewPreparationFailure> ReviewView::create(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        ReviewQuestion question
    )
    {
        const bool invalid_header = !question.request || question.title.empty();
        const bool invalid_choices =
            question.choices.empty() || question.choices.size() > 7 ||
            std::ranges::find(question.choices, EReviewChoice::CANCEL) == question.choices.end();
        if (invalid_header || invalid_choices)
            return cxx::unexpected(views::ViewPreparationFailure{"review.question", 0, "Invalid review question"});
        for (std::size_t i{}; i < question.choices.size(); ++i)
        {
            auto earlier = std::span{question.choices}.first(i);
            if (!label(question.choices[i]) || std::ranges::find(earlier, question.choices[i]) != earlier.end())
                return cxx::unexpected(views::ViewPreparationFailure{"review.choices", 0, "Invalid or repeated choice"}
                );
        }
        auto result = std::unique_ptr<ReviewView>(new ReviewView(dispatcher, std::move(id), std::move(question)));
        if (result->impl_->failure_)
            return cxx::unexpected(std::move(*result->impl_->failure_));
        return result;
    }
    views::ViewResult<void> ReviewView::answer(EReviewChoice choice) noexcept
    {
        if (impl_->answer_)
            return cxx::unexpected(views::EViewError::BUSY);
        if (std::ranges::find(impl_->question_.choices, choice) == impl_->question_.choices.end())
            return cxx::unexpected(views::EViewError::INVALID_ID);
        impl_->answer_ = ReviewAnswer{impl_->question_.request, choice};
        return {};
    }
    const std::optional<ReviewAnswer>& ReviewView::response() const noexcept
    {
        return impl_->answer_;
    }
    const ReviewQuestion& ReviewView::question() const noexcept
    {
        return impl_->question_;
    }
}
