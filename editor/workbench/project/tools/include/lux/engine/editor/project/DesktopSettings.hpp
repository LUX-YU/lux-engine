#pragma once

#include <lux/engine/editor/configuration/Settings.hpp>
#include <lux/engine/editor/commands/Command.hpp>
#include <lux/engine/window/WindowPlacement.hpp>
#include <lux/engine/meta/TypeStaticInfo.hpp>

namespace lux::editor::project
{
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
    // Bootstrap and contributions call the same fixed reflection contribution.
    void registerDesktopSettings(meta::ReflectionRegistry&, meta::qual_type_index_fix_list&);
    [[nodiscard]] std::shared_ptr<settings::SettingsEntry> makeAppearanceSetting();
    [[nodiscard]] std::shared_ptr<settings::SettingsEntry> makeWindowSetting();
    [[nodiscard]] std::shared_ptr<settings::SettingsEntry> makeShortcutSetting(settings::SettingsEntry::Apply);
}

namespace lux::meta
{
    template <> struct TTypeStaticInfo<window::WindowRect>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&window::WindowRect::x>("x"), typeStaticField<&window::WindowRect::y>("y"),
            typeStaticField<&window::WindowRect::width>("width"), typeStaticField<&window::WindowRect::height>("height")
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
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&editor::project::ShortcutSettings::overrides>("overrides")
        );
    };
}
