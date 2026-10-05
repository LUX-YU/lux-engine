#pragma once
#include <lux/engine/editor/views/ViewError.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <memory>
#include <optional>

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
        std::optional<std::string> input_label;
        std::string initial_text;
    };
    struct ReviewAnswer final
    {
        std::uint64_t request{};
        EReviewChoice choice{EReviewChoice::CANCEL};
        std::string text;
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
        [[nodiscard]] views::ViewResult<void> setText(std::string);
        // Reopen the same owned draft after a rejected answer; the caller retains its original source stamp.
        void rejectAnswer(std::string message);

    private:
        ReviewView(object::ObjectDispatcherRef, lux::ui::PaneId, ReviewQuestion);
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
