#pragma once
#include <lux/engine/editor/storage/ProjectPublication.hpp>
#include <lux/engine/ui/Pane.hpp>
namespace lux::project
{
    class PluginManager;
}
namespace lux::editor::project
{
    struct PluginSelectionDraft final
    {
        std::vector<ProjectPluginEntry> based_on, desired;
    };

    // The view owns a draft only; project publication remains with the application activity owner.
    class SettingsView final : public lux::ui::Pane
    {
    public:
        object::TSignal<PluginSelectionDraft> selectionRequested{*this};
        object::TSignal<> retryRequested{*this}, abandonRequested{*this}, acknowledgeRequested{*this};
        SettingsView(object::ObjectDispatcherRef, lux::ui::PaneId, ProjectStorage&, const lux::project::PluginManager&);
        ~SettingsView() noexcept override;
        SettingsView(const SettingsView&) = delete;
        SettingsView& operator=(const SettingsView&) = delete;
        SettingsView(SettingsView&&) = delete;
        SettingsView& operator=(SettingsView&&) = delete;
        [[nodiscard]] EditorResult<void> requestSave(std::vector<ProjectPluginEntry>);
        void setPublicationStatus(std::optional<VPublicationStatus>);

    private:
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
