#include <lux/engine/editor/project/DesktopSettings.hpp>
#include <lux/engine/editor/desktop/CommandMenu.hpp>
#include <cmath>
#include <algorithm>

namespace lux::editor::project
{
    namespace
    {
        template <class Value> void registerValue(meta::ReflectionRegistry& registry)
        {
            if (registry.findClass(cxx::type_name<Value>()))
                return;
            auto type = std::make_unique<meta::RefClass>();
            type->name = cxx::type_name<Value>();
            type->full_name = type->name;
            type->hash = cxx::type_hash<Value>();
            type->type = meta::ref_type_of_v<Value>;
            type->type.ptr = type.get();
            type->construct = [](void* storage) { std::construct_at(static_cast<Value*>(storage)); };
            type->destruct = [](void* storage) { std::destroy_at(static_cast<Value*>(storage)); };
            registry.registerClass(std::move(type));
        }

        template <class Value> ConfigurationDescriptor configuration(const char* name)
        {
            return {
                name,
                1,
                serialization::makePortableValueCodec<Value>(),
                +[](meta::ReflectionRegistry& registry) noexcept { return registry.findClass(cxx::type_name<Value>()); }
            };
        }

        const auto appearance_value = configuration<AppearanceSettings>("lux.desktop.appearance");
        const auto window_value = configuration<WindowSettings>("lux.desktop.window");
        const auto shortcut_value = configuration<ShortcutSettings>("lux.desktop.shortcuts");
        constexpr settings::SettingsDescriptor appearance{
            settings::SettingsIdView{"lux.desktop.appearance"},
            "Font and UI scale",
            &appearance_value,
            settings::kPersonalScopes,
            settings::ESettingsApply::RESTART,
            +[](const ConfigurationValue& input) noexcept -> settings::SettingsResult<void>
            {
                const auto& value = *static_cast<const AppearanceSettings*>(input.data());
                const bool invalid_scale = !std::isfinite(value.scale) || value.scale < 0.5f || value.scale > 4.f;
                const bool invalid_font = value.font.size() > 4096 || value.font.find('\0') != std::string::npos;
                if (invalid_scale || invalid_font)
                    return cxx::unexpected(settings::SettingsFailure{
                        settings::ESettingsError::INVALID_VALUE,
                        "UI scale must be 0.5..4 and font must be a valid bounded path"
                    });
                return {};
            }
        };
        constexpr settings::SettingsDescriptor window_descriptor{
            settings::SettingsIdView{"lux.desktop.window"},
            "Window placement",
            &window_value,
            settings::kPersonalScopes,
            settings::ESettingsApply::RESTART,
            +[](const ConfigurationValue& input) noexcept -> settings::SettingsResult<void>
            {
                const auto& value = *static_cast<const WindowSettings*>(input.data());
                const auto& rect = value.placement.normal;
                const bool invalid_size =
                    value.restore && (rect.width <= 0 || rect.height <= 0 || rect.width > 32768 || rect.height > 32768);
                const auto mode = value.placement.mode;
                const bool invalid_mode = mode != window::EWindowMode::ORDINARY &&
                                          mode != window::EWindowMode::MAXIMIZED &&
                                          mode != window::EWindowMode::FULLSCREEN;
                if (invalid_size || invalid_mode)
                    return cxx::unexpected(settings::SettingsFailure{
                        settings::ESettingsError::INVALID_VALUE,
                        "Invalid restored window size or mode"
                    });
                return {};
            }
        };
        constexpr settings::SettingsDescriptor shortcuts{
            settings::SettingsIdView{"lux.desktop.shortcuts"},
            "Command shortcuts",
            &shortcut_value,
            settings::kPersonalScopes,
            settings::ESettingsApply::SAFE_POINT,
            +[](const ConfigurationValue& input) noexcept -> settings::SettingsResult<void>
            {
                auto checked =
                    desktop::validateShortcutOverrides(static_cast<const ShortcutSettings*>(input.data())->overrides);
                if (!checked)
                    return cxx::unexpected(settings::SettingsFailure{
                        settings::ESettingsError::INVALID_VALUE,
                        checked.error().domain + ": " + checked.error().detail
                    });
                return {};
            }
        };
    } // namespace

    settings::SettingsResult<DesktopSettingsValues> resolveDesktopSettings(
        std::span<const settings::SettingsPage> pages,
        std::span<const settings::SettingsDocument> documents
    )
    {
        const auto resolve = [&]<class Value>(const settings::SettingsDescriptor& descriptor
                             ) -> settings::SettingsResult<Value>
        {
            const auto found = std::ranges::find_if(
                pages,
                [&](const auto& page)
                { return page.entry && page.entry->descriptor().id.name() == descriptor.id.name(); }
            );
            if (found == pages.end())
                return cxx::unexpected(
                    settings::SettingsFailure{settings::ESettingsError::UNAVAILABLE, std::string{descriptor.id.name()}}
                );
            const auto* configuration = found->entry->descriptor().configuration;
            if (!configuration || configuration->codec.type != cxx::typeToken<Value>())
                return cxx::unexpected(settings::SettingsFailure{
                    settings::ESettingsError::INVALID_DESCRIPTOR,
                    std::string{descriptor.id.name()}
                });
            auto resolved = settings::resolveSettings(found->entry, documents);
            if (!resolved)
                return cxx::unexpected(resolved.error());
            return *static_cast<const Value*>(resolved->desired.data());
        };
        auto display = resolve.template operator()<AppearanceSettings>(appearance);
        if (!display)
            return cxx::unexpected(display.error());
        auto placement = resolve.template operator()<WindowSettings>(window_descriptor);
        if (!placement)
            return cxx::unexpected(placement.error());
        auto bindings = resolve.template operator()<ShortcutSettings>(shortcuts);
        if (!bindings)
            return cxx::unexpected(bindings.error());
        return DesktopSettingsValues{std::move(*display), std::move(*placement), std::move(*bindings)};
    }

    void registerDesktopSettings(meta::ReflectionRegistry& registry, meta::qual_type_index_fix_list&)
    {
        const auto add_enum = [&](auto tag, std::vector<meta::RefEnumValue> values)
        {
            using Enum = decltype(tag);
            if (registry.findEnum(cxx::type_name<Enum>()))
                return;
            auto type = std::make_unique<meta::RefEnum>();
            type->name = type->full_name = cxx::type_name<Enum>();
            type->is_scoped = true;
            type->values = std::move(values);
            registry.registerEnum(std::move(type));
        };
        add_enum(window::EWindowMode::ORDINARY, {{"Ordinary", 0}, {"Maximized", 1}, {"Fullscreen", 2}});
        add_enum(commands::ECommandScope::APPLICATION, {{"Application", 0}, {"Session", 1}, {"View", 2}});
        registerValue<AppearanceSettings>(registry);
        registerValue<WindowSettings>(registry);
        registerValue<ShortcutSettings>(registry);
    }

    std::shared_ptr<settings::SettingsEntry> makeAppearanceSetting()
    {
        return settings::SettingsEntry::bind<appearance>(lux::object::CodeLease::builtin());
    }
    std::shared_ptr<settings::SettingsEntry> makeWindowSetting()
    {
        return settings::SettingsEntry::bind<window_descriptor>(lux::object::CodeLease::builtin());
    }
    std::shared_ptr<settings::SettingsEntry> makeShortcutSetting(settings::SettingsEntry::Apply apply)
    {
        return settings::SettingsEntry::bind<shortcuts>(lux::object::CodeLease::builtin(), std::move(apply));
    }
} // namespace lux::editor::project
