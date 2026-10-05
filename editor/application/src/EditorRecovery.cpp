#include <lux/engine/editor/application/EditorApplicationImpl.hpp>

namespace lux::editor::application
{
    EditorResult<void> EditorApplication::Impl::captureRecovery()
    {
        auto views = editor_context_.ui().describe(desktop_->root());
        if (!views)
        {
            return applicationFailure("recovery.views", views.error());
        }
        return restoration_->capture(*views);
    }
    EditorResult<void> EditorApplication::Impl::restoreRecovery()
    {
        return restoration_->start();
    }
    EditorResult<void> EditorApplication::Impl::settleRecovery()
    {
        auto progress = ERestorationProgress::SUSPENDED;
        if (phase_ == EApplicationPhase::RUNNING)
        {
            progress = ERestorationProgress::ACTIVE;
        }
        else if (phase_ == EApplicationPhase::DRAINING)
        {
            progress = ERestorationProgress::CLOSING;
        }
        auto present = [&](views::ViewContent content,
                           const extensions::ContributionSnapshot& catalog,
                           views::ViewRestoreKey key,
                           views::ViewTypeId type)
        { return makeContentView(std::move(content), true, catalog, std::move(key), std::move(type)); };
        return restoration_->update(progress, present);
    }
} // namespace lux::editor::application
