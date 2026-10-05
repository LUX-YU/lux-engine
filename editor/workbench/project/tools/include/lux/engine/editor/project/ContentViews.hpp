#pragma once
#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/project/AssetCatalog.hpp>
#include <lux/engine/editor/sessions/SessionOpening.hpp>
#include <lux/engine/editor/views/ViewContent.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::services
{
    class ServiceScope;
    struct ServiceDescriptor;
    struct ServiceContract;
} // namespace lux::services
namespace lux::ui
{
    class Root;
}
namespace lux::editor
{
    class ProjectStorage;
}
namespace lux::editor::persistence
{
    class IArtifactStore;
}
namespace lux::editor::desktop
{
    class UiRegistry;
}
namespace lux::editor::extensions
{
    class ContributionRegistry;
    class ContributionSnapshot;
} // namespace lux::editor::extensions
namespace lux::editor::project
{
    struct OpenAndShowResult final
    {
        sessions::OpenAssetStatus content;
        std::optional<lux::ui::PaneHandle> view;
        std::optional<EditorFailure> presentation_failure;
    };
    // Workbench open/show policy only. SessionOpening owns reads and installation; Root owns windows.
    class ContentViews final
    {
    public:
        static const services::ServiceDescriptor service;
        ContentViews(
            std::shared_ptr<sessions::SessionStore>,
            std::shared_ptr<sessions::SessionOpening>,
            std::shared_ptr<persistence::IArtifactStore>,
            ProjectStorage&,
            lux::ui::Root&,
            desktop::UiRegistry&,
            services::ServiceScope&,
            extensions::ContributionRegistry&
        );
        ~ContentViews();
        ContentViews(const ContentViews&) = delete;
        ContentViews& operator=(const ContentViews&) = delete;
        ContentViews(ContentViews&&) = delete;
        ContentViews& operator=(ContentViews&&) = delete;
        [[nodiscard]] EditorResult<sessions::OpenAssetId> open(AssetReference);
        [[nodiscard]] EditorResult<sessions::OpenAssetId> create(sessions::SessionPreparation);
        [[nodiscard]] EditorResult<OpenAndShowResult> status(sessions::OpenAssetId) const;
        [[nodiscard]] EditorResult<void> cancel(sessions::OpenAssetId);
        [[nodiscard]] EditorResult<void> acknowledge(sessions::OpenAssetId);
        [[nodiscard]] EditorResult<void> enqueue(AssetReference);
        [[nodiscard]] EditorResult<lux::ui::PaneHandle> show(sessions::SessionId, bool another_view = false);
        // Restoration supplies the catalog already protected by its original contribution batch.
        [[nodiscard]] EditorResult<lux::ui::PaneHandle> restore(
            views::ViewContent,
            const extensions::ContributionSnapshot&,
            views::ViewRestoreKey,
            views::ViewTypeId
        );
        [[nodiscard]] EditorResult<void> update();
        // Suspends new presentation during reversible close review, not accepted IO completion.
        [[nodiscard]] EditorResult<void> suspend();
        [[nodiscard]] EditorResult<void> resume();
        [[nodiscard]] bool hasCapacity() const noexcept;

    private:
        static const services::ServiceContract contracts_[];
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::project
