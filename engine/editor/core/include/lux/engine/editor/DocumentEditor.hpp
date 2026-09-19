#pragma once

#include <lux/engine/editor/DocumentRequests.hpp>
#include <lux/engine/editor/core/visibility.h>
#include <lux/engine/editor/editing/EditHistoryTarget.hpp>
#include <memory>
#include <span>

namespace lux::editor
{
    class Editor;

    struct DocumentSummary final
    {
        DocumentHandle handle;
        DocumentKey key;
        std::string title;
        bool read_only{true};
        std::string read_only_reason;
    };

    class LUX_EDITOR_CORE_PUBLIC DocumentView
    {
      public:
        virtual ~DocumentView();
        [[nodiscard]] virtual std::string_view id() const noexcept = 0;
        virtual void requestClose() noexcept = 0;
        virtual void poll(PollBudget &) = 0;
        [[nodiscard]] virtual CloseStatus closeStatus() const = 0;
    };

    class LUX_EDITOR_CORE_PUBLIC DocumentEditor : public editing::EditHistoryTarget
    {
      public:
        ~DocumentEditor() override;

        [[nodiscard]] DocumentHandle handle() const noexcept
        {
            return handle_;
        }

        [[nodiscard]] virtual DocumentSummary summary() const = 0;
        [[nodiscard]] virtual EditorResult<SaveRequestId> requestSave(std::string origin);
        [[nodiscard]] virtual std::span<const SaveRequestId> saveRequests() const noexcept;
        [[nodiscard]] virtual EditorResult<SaveRequestStatus> saveStatus(SaveRequestId) const;
        [[nodiscard]] virtual EditorResult<void> retrySave(SaveRequestId);
        [[nodiscard]] virtual EditorResult<void> abandonSave(SaveRequestId);
        [[nodiscard]] virtual EditorResult<void> acknowledgeSave(SaveRequestId);
        // Read-only: never stops work, finishes gestures, or publishes callbacks.
        [[nodiscard]] virtual EditorResult<editing::HistorySnapshot> reviewClose() const;
        virtual void requestClose() noexcept = 0;
        [[nodiscard]] virtual CloseStatus closeStatus() const = 0;
        virtual void poll(PollBudget &) = 0;
        [[nodiscard]] virtual std::span<const std::unique_ptr<DocumentView>> views() const noexcept = 0;

      private:
        friend class Editor;
        DocumentHandle handle_;
    };

    // An admitted open owns its work and unpublished result until completion is consumed.
    class LUX_EDITOR_CORE_PUBLIC DocumentOpening
    {
      public:
        virtual ~DocumentOpening();
        virtual void cancel() noexcept = 0;
        virtual void poll(PollBudget &) = 0;
        [[nodiscard]] virtual bool settled() const noexcept = 0;
        [[nodiscard]] virtual EditorResult<std::unique_ptr<DocumentEditor>> take() = 0;
    };
} // namespace lux::editor
