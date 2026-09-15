#pragma once

#include <cassert>
#include <lux/engine/editor/Editor.hpp>

// A test frontend's explicit discard policy. Production UI obtains these decisions
// from the user; both consumers use the same core review and asynchronous closing.
class TestExit final
{
  public:
    void request(lux::editor::Editor &editor)
    {
        if (editor.closing() || editor.cancelStartup())
        {
            return;
        }
        owner_ = &editor;
        poll();
    }

    void poll()
    {
        using namespace lux::editor;
        if (!owner_ || owner_->closing())
        {
            return;
        }
        if (!review_.serial)
        {
            const auto started = owner_->beginExitReview();
            assert(started);
            review_ = *started;
        }
        std::vector<DocumentCloseDecision> decisions;
        for (const auto &summary : owner_->documents())
        {
            auto &document = owner_->document(summary.handle)->get();
            if (document.closeStatus().state != ECloseState::OPEN)
            {
                return;
            }
            for (const auto request : document.saveRequests())
            {
                const auto status = document.saveStatus(request);
                assert(status);
                if (std::holds_alternative<SavePending>(*status) || std::holds_alternative<SaveRetryable>(*status))
                {
                    assert(document.abandonSave(request));
                    return;
                }
            }
            const auto snapshot = document.reviewClose();
            if (!snapshot)
            {
                assert(snapshot.error().code == EEditorError::BUSY);
                return;
            }
            decisions.push_back(
                {summary.handle, snapshot->current, snapshot->revision, EDocumentCloseDecision::DISCARD_THIS_STATE});
        }
        const auto committed = owner_->commitExitReview(review_, decisions);
        assert(committed || committed.error().code == EEditorError::BUSY);
    }

  private:
    lux::editor::Editor *owner_{};
    lux::editor::ExitReviewId review_;
};
