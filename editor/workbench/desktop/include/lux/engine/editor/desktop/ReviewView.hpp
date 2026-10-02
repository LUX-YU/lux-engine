#pragma once
#include <lux/engine/editor/views/IViewHost.hpp>

namespace lux::editor::desktop
{
    enum class EReviewChoice : std::uint8_t
    {
        SAVE,
        DISCARD,
        CANCEL,
        KEEP_CONTENT,
        CLOSE_CONTENT,
        STOP_RUN,
        KEEP_RUN
    };
    struct ReviewQuestion final
    {
        std::uint64_t request{};
        std::string title, message;
        std::vector<EReviewChoice> choices;
    };
    struct ReviewAnswer final
    {
        std::uint64_t request{};
        EReviewChoice choice{EReviewChoice::CANCEL};
    };
    // Owns presentation values only. The application retains the exact source stamps and performs
    // the decision; the modal neither reads content nor starts saving or closing it.
    class ReviewView final : public lux::ui::Pane
    {
    public:
        [[nodiscard]] static cxx::expected<std::unique_ptr<ReviewView>, views::ViewPreparationFailure> create(
            object::ObjectDispatcherRef,
            lux::ui::PaneId,
            ReviewQuestion
        );
        ~ReviewView() noexcept override;
        ReviewView(const ReviewView&) = delete;
        ReviewView& operator=(const ReviewView&) = delete;
        ReviewView(ReviewView&&) = delete;
        ReviewView& operator=(ReviewView&&) = delete;
        // Same path used by actual buttons. The first valid answer remains owned until this view retires.
        [[nodiscard]] views::ViewResult<void> answer(EReviewChoice) noexcept;
        [[nodiscard]] const std::optional<ReviewAnswer>& response() const noexcept;
        [[nodiscard]] const ReviewQuestion& question() const noexcept;

    private:
        ReviewView(object::ObjectDispatcherRef, lux::ui::PaneId, ReviewQuestion);
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
