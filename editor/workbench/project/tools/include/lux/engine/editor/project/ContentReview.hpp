#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/commands/Command.hpp>
#include <lux/engine/editor/persistence/SaveTypes.hpp>
#include <lux/engine/ui/Attachment.hpp>
#include <memory>

namespace lux::ui
{
    class Root;
}
namespace lux::services
{
    struct ServiceDescriptor;
}
namespace lux::editor
{
    class ProjectContentSaving;
    class ProjectContentReloading;
} // namespace lux::editor
namespace lux::editor::sessions
{
    class SessionStore;
    class SessionOpening;
} // namespace lux::editor::sessions
namespace lux::editor::desktop
{
    class UiRegistry;
}
namespace lux::editor::project
{
    struct ContentReviewQuestion final
    {
        sessions::ContentStamp source;
        lux::ui::PaneHandle view;
    };

    // Workbench policy for save naming and reload confirmation. The activity providers own
    // accepted operations/results; Root alone owns the mounted question windows.
    class ContentReview final
    {
    public:
        ContentReview(
            std::shared_ptr<sessions::SessionStore>,
            std::shared_ptr<sessions::SessionOpening>,
            std::shared_ptr<ProjectContentSaving>,
            std::shared_ptr<ProjectContentReloading>,
            lux::ui::Root&,
            desktop::UiRegistry&
        );
        ~ContentReview();
        ContentReview(const ContentReview&) = delete;
        ContentReview& operator=(const ContentReview&) = delete;
        ContentReview(ContentReview&&) = delete;
        ContentReview& operator=(ContentReview&&) = delete;

        [[nodiscard]] EditorResult<persistence::SaveId> save(
            commands::SessionTarget,
            persistence::ESaveMode,
            std::string destination = {}
        );
        [[nodiscard]] EditorResult<void> saveAll();
        [[nodiscard]] EditorResult<void> askSave(commands::SessionTarget, persistence::ESaveMode);
        [[nodiscard]] EditorResult<void> askReload(commands::SessionTarget);
        [[nodiscard]] EditorResult<void> update();
        // Owner-thread value observation, including while a window callback holds the tree stable.
        [[nodiscard]] std::optional<ContentReviewQuestion> question() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    extern const services::ServiceDescriptor kContentReviewService;
} // namespace lux::editor::project
