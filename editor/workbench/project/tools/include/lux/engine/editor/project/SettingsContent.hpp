#pragma once
#include <lux/engine/editor/configuration/Settings.hpp>
#include <lux/engine/editor/settings/SettingsPage.hpp>
#include <lux/engine/editor/workspace/WorkspaceChanges.hpp>
#include <lux/engine/ui/Element.hpp>

namespace lux::editor::project
{
    struct SettingsLocation final
    {
        workspace::WorkspaceStore& store;
        workspace::WorkspaceChanges* changes{}; // Null for an explicitly read-only source.
        settings::ESettingsScope scope;
        std::string relative;
    };
    struct AppliedSetting final
    {
        std::shared_ptr<const settings::SettingsEntry> entry;
        std::vector<std::byte> bytes;
    };
    // Specific page wiring, shared only by settings pages and their assembly owner. Applied values
    // are confirmed presentation facts; the setting receiver remains the actual functional owner.
    struct SettingsContentInput final
    {
        SettingsContentInput() = default;
        SettingsContentInput(const SettingsContentInput&) = delete;
        SettingsContentInput& operator=(const SettingsContentInput&) = delete;
        SettingsContentInput(SettingsContentInput&&) = delete;
        SettingsContentInput& operator=(SettingsContentInput&&) = delete;
        cxx::move_only_function<std::vector<settings::SettingsPage>()> pages;
        std::vector<SettingsLocation> locations;
        std::vector<AppliedSetting> applied;
    };
    enum class ESettingsAction : std::uint8_t
    {
        APPLY,
        SAVE,
        REVERT,
        DEFAULTS
    };
    class SettingsContent final : public lux::ui::Element
    {
    public:
        SettingsContent(lux::ui::Element&, lux::ui::ElementId, std::shared_ptr<SettingsContentInput>);
        ~SettingsContent() override;
        SettingsContent(const SettingsContent&) = delete;
        SettingsContent& operator=(const SettingsContent&) = delete;
        SettingsContent(SettingsContent&&) = delete;
        SettingsContent& operator=(SettingsContent&&) = delete;
        // Owner safe-point entry points also used by non-mouse accessibility/SDK consumers.
        // Refresh choices only: an active draft retains its declaration, source and values.
        [[nodiscard]] EditorResult<void> refreshPages();
        [[nodiscard]] std::span<const settings::SettingsPage> pages() const noexcept;
        [[nodiscard]] EditorResult<void> select(settings::SettingsIdView, settings::ESettingsScope);
        [[nodiscard]] EditorResult<void> request(ESettingsAction);
        [[nodiscard]] const settings::SettingsDraft* draft() const noexcept;
        [[nodiscard]] const std::optional<EditorFailure>& failure() const noexcept;
        void finishEdit(bool cancel = false) noexcept override;

    private:
        void draw() noexcept override;
        void update() noexcept override;
        void arrangeContent() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::project
