#pragma once
#include <array>
#include <lux/engine/editor/desktop/ViewCommands.hpp>
#include <lux/engine/editor/storage/ProjectPublication.hpp>
#include <lux/engine/ui/Pane.hpp>
namespace lux::editor::desktop
{
    struct UiDescriptor;
    struct UiFailure;
    struct UiCreateInfo;
} // namespace lux::editor::desktop
namespace lux::project
{
    class PluginManager;
}
namespace lux::services
{
    class ServiceResolver;
}
namespace lux::editor::project
{
    extern const desktop::UiDescriptor kSettingsView;
    struct SettingsContentInput;
    struct PluginSelectionDraft final
    {
        std::vector<ProjectPluginEntry> based_on, desired;
    };

    struct PluginSelectionRequests final
    {
        cxx::move_only_function<void(const PluginSelectionDraft&)> save;
        cxx::move_only_function<void()> retry, abandon, acknowledge;
    };

    // The view owns a draft only; project publication remains with the application activity owner.
    class SettingsView final : public lux::ui::Pane
    {
    public:
        [[nodiscard]] static cxx::expected<std::unique_ptr<lux::ui::Pane>, desktop::UiFailure>
        createConfigured(services::ServiceResolver&, const desktop::UiCreateInfo&);
        object::TSignal<PluginSelectionDraft> selectionRequested{*this};
        object::TSignal<> retryRequested{*this}, abandonRequested{*this}, acknowledgeRequested{*this};
        SettingsView(
            object::ObjectDispatcherRef,
            lux::ui::PaneId,
            ProjectStorage&,
            const lux::project::PluginManager&,
            std::shared_ptr<SettingsContentInput> = {}
        );
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
        std::array<object::Connection, 4> request_connections_;
    };
    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeSettingsCommand(
        commands::CommandEntry::Query,
        desktop::ToolOpening
    );
} // namespace lux::editor::project
