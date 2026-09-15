#pragma once

#include <imgui.h>
#include <lux/engine/editor/DocumentEditor.hpp>
#include <lux/engine/ui/Frame.hpp>

namespace lux::editor::gui
{
    // Display and request identity only. The document owns all publication work.
    class PublicationControls final
    {
      public:
        void poll(DocumentEditor &document)
        {
            if (!published_save_.serial)
            {
                return;
            }
            const auto status = document.saveStatus(published_save_);
            if (!status)
            {
                publication_message_ = status.error().domain + ": " + status.error().message;
                return;
            }
            if (const auto *done = std::get_if<SaveSucceeded>(&*status))
            {
                publication_message_ = done->cleanup
                                           ? "Source and compiled asset published"
                                           : "Published; cleanup requires attention: " + done->cleanup.error().message;
            }
            else if (std::holds_alternative<SaveAbandoned>(*status))
            {
                publication_message_ = "Publication abandoned";
            }
            else
            {
                return;
            }
            if (document.acknowledgeSave(published_save_))
            {
                published_save_ = {};
            }
        }
        void track(SaveRequestId request)
        {
            published_save_ = request;
            publication_message_.clear();
        }
        void draw(DocumentEditor &document, lux::ui::Frame &frame)
        {
            if (!publication_message_.empty())
            {
                frame.text(publication_message_);
            }
            if (!published_save_.serial)
            {
                return;
            }
            const auto status = document.saveStatus(published_save_);
            if (!status)
            {
                frame.text(status.error().message);
                return;
            }
            if (const auto *failed = std::get_if<SaveRetryable>(&*status))
            {
                ImGui::TextWrapped("Publication failed: %s (%llu) %s", failed->failure.domain.c_str(),
                                   static_cast<unsigned long long>(failed->failure.reason),
                                   failed->failure.message.c_str());
                ImGui::BeginDisabled(!failed->retry_allowed);
                if (ImGui::SmallButton("Retry publication"))
                {
                    const auto retried = document.retrySave(published_save_);
                    if (!retried)
                    {
                        publication_message_ = retried.error().domain + ": " + retried.error().message;
                    }
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::SmallButton("Abandon publication"))
                {
                    const auto abandoned = document.abandonSave(published_save_);
                    if (!abandoned)
                    {
                        publication_message_ = abandoned.error().domain + ": " + abandoned.error().message;
                    }
                }
            }
            else
            {
                frame.textMuted("Publishing captured source and compiled asset...");
            }
        }

      private:
        SaveRequestId published_save_;
        std::string publication_message_;
    };
}
