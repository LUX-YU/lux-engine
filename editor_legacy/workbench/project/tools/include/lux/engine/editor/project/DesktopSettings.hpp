#pragma once

#include <lux/engine/editor/configuration/Settings.hpp>
#include <lux/engine/editor/commands/Command.hpp>
#include <lux/engine/window/WindowPlacement.hpp>
#include <lux/engine/meta/TypeStaticInfo.hpp>
#include <lux/engine/editor/settings/SettingsPage.hpp>

namespace lux::window
{
    class LuxWindow;
}
namespace lux::editor::workspace
{
    class WorkspaceStore;
    class WorkspaceChanges;
} // namespace lux::editor::workspace

namespace lux::editor::project
{
    struct SettingsContentInput;
    struct AppearanceSettings final
    {
        std::string font;
        float scale{1.f};
    };
    struct WindowSettings final
    {
        bool restore{};
        window::WindowPlacement placement;
    };
    struct ShortcutSettings final
    {
        std::vector<commands::ShortcutOverride> overrides;
    };
    struct DesktopSettingsValues final
    {
        AppearanceSettings appearance;
        WindowSettings window;
        ShortcutSettings shortcuts;
    };
    [[nodiscard]] settings::SettingsResult<DesktopSettingsValues>
        resolveDesktopSettings(std::span<const settings::SettingsPage>, std::span<const settings::SettingsDocument>);
    [[nodiscard]] std::vector<settings::SettingsPage> makeDesktopSettingsPages(settings::SettingsEntry::Apply);
    // Bootstrap and contributions call the same fixed reflection contribution.
    void registerDesktopSettings(meta::ReflectionRegistry&, meta::qual_type_index_fix_list&);
    [[nodiscard]] std::shared_ptr<settings::SettingsEntry> makeAppearanceSetting();
    [[nodiscard]] std::shared_ptr<settings::SettingsEntry> makeWindowSetting();
    [[nodiscard]] std::shared_ptr<settings::SettingsEntry> makeShortcutSetting(settings::SettingsEntry::Apply);

    // Owns one window observation connection and coalesces its personal persistence intent.
    // The original Changes/Coordinator retain accepted writes, including Unknown, after this dies.
    class WindowSettingsBinding final
    {
    public:
        [[nodiscard]] static EditorResult<std::unique_ptr<WindowSettingsBinding>>
        create(window::LuxWindow&, workspace::WorkspaceStore&, workspace::WorkspaceChanges&, std::shared_ptr<const settings::SettingsEntry>, SettingsContentInput&);
        ~WindowSettingsBinding();
        WindowSettingsBinding(const WindowSettingsBinding&) = delete;
        WindowSettingsBinding& operator=(const WindowSettingsBinding&) = delete;
        WindowSettingsBinding(WindowSettingsBinding&&) = delete;
        WindowSettingsBinding& operator=(WindowSettingsBinding&&) = delete;
        void update(bool allow_new_work = true);
        void retry() noexcept;
        [[nodiscard]] const EditorFailure* failure() const noexcept;

    private:
        struct Impl;
        explicit WindowSettingsBinding(std::unique_ptr<Impl>);
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::project

namespace lux::meta
{
    template <> struct TTypeStaticInfo<window::WindowRect>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&window::WindowRect::x>("x"),
            typeStaticField<&window::WindowRect::y>("y"),
            typeStaticField<&window::WindowRect::width>("width"),
            typeStaticField<&window::WindowRect::height>("height")
        );
    };
    template <> struct TTypeStaticInfo<window::DisplayHint>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&window::DisplayHint::name>("name"),
            typeStaticField<&window::DisplayHint::work_area>("work_area")
        );
    };
    template <> struct TTypeStaticInfo<window::WindowPlacement>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&window::WindowPlacement::normal>("normal"),
            typeStaticField<&window::WindowPlacement::mode>("mode"),
            typeStaticField<&window::WindowPlacement::display>("display")
        );
    };
    template <> struct TTypeStaticInfo<editor::project::AppearanceSettings>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&editor::project::AppearanceSettings::font>("font"),
            typeStaticField<&editor::project::AppearanceSettings::scale>("scale")
        );
    };
    template <> struct TTypeStaticInfo<editor::project::WindowSettings>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&editor::project::WindowSettings::restore>("restore"),
            typeStaticField<&editor::project::WindowSettings::placement>("placement")
        );
    };
    template <> struct TTypeStaticInfo<editor::commands::ShortcutOverride>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&editor::commands::ShortcutOverride::command>("command"),
            typeStaticField<&editor::commands::ShortcutOverride::binding>("binding"),
            typeStaticField<&editor::commands::ShortcutOverride::scope>("scope"),
            typeStaticField<&editor::commands::ShortcutOverride::input_version>("input_version")
        );
    };
    template <> struct TTypeStaticInfo<editor::project::ShortcutSettings>
    {
        static constexpr bool available = true;
        static constexpr auto fields =
            std::make_tuple(typeStaticField<&editor::project::ShortcutSettings::overrides>("overrides"));
    };
} // namespace lux::meta
