#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <algorithm>

namespace lux::editor::application
{
    namespace
    {
        EditorResult<std::vector<settings::SettingsDocument>> readSettings(
            std::span<const project::SettingsLocation> locations)
        {
            std::vector<settings::SettingsDocument> documents;
            documents.reserve(locations.size());
            for (const auto& location : locations)
            {
                auto document = location.store.readSettings(location.relative, location.scope);
                if (document)
                    documents.push_back(std::move(*document));
                else if (document.error().code != workspace::EWorkspaceError::NOT_FOUND)
                    return applicationFailure("settings.read", document.error());
            }
            return documents;
        }
    }
    EditorResult<project::DesktopSettingsValues> EditorApplication::Impl::prepareDesktopSettings()
    {
        builtin_settings_ = project::makeDesktopSettingsPages([this](const ConfigurationValue& value)
            -> settings::SettingsResult<void> {
            if (!desktop_ || !desktop_->commands())
                return cxx::unexpected(settings::SettingsFailure{settings::ESettingsError::UNAVAILABLE,
                    "Desktop commands are not installed yet"});
            auto applied = desktop_->commands()->setShortcuts(
                static_cast<const project::ShortcutSettings*>(value.data())->overrides);
            if (!applied)
            {
                const auto code = applied.error().code == commands::ECommandError::BUSY
                    ? settings::ESettingsError::BUSY : settings::ESettingsError::INVALID_VALUE;
                return cxx::unexpected(settings::SettingsFailure{code,
                    applied.error().domain + ": " + applied.error().detail});
            }
            return {};
        });
        extensions::ContributionDraft bootstrap;
        bootstrap.reflection.push_back({contracts::CodeLease::builtin(), project::registerDesktopSettings});
        bootstrap.settings = builtin_settings_;
        auto prepared = extensions::ContributionSnapshot::prepare(std::move(bootstrap));
        if (!prepared)
            return applicationFailure("settings.bootstrap.prepare", prepared.error());
        auto queued = contributions_.enqueue(*prepared);
        if (!queued)
            return applicationFailure("settings.bootstrap.enqueue", queued.error());
        auto installed = contributions_.applyPending();
        if (!installed)
            return applicationFailure("settings.bootstrap.install", installed.error());

        settings_content_ = std::make_shared<project::SettingsContentInput>();
        settings_content_->pages = [this] {
            const auto snapshot = contributions_.snapshot();
            return std::vector<settings::SettingsPage>{snapshot.settings().begin(), snapshot.settings().end()};
        };
        settings_content_->locations = std::vector<project::SettingsLocation>{
            {installation_settings_, nullptr, settings::ESettingsScope::INSTALLATION, "share/lux-engine/editor/settings.toml"},
            {project_workspace_, &project_settings_changes_, settings::ESettingsScope::PROJECT, ".lux/settings.toml"},
            {user_settings_, &user_settings_changes_, settings::ESettingsScope::USER, "settings.toml"},
            {workspace_, &workspace_changes_, settings::ESettingsScope::USER_PROJECT, "settings.toml"}
        };
        auto documents = readSettings(settings_content_->locations);
        if (!documents)
            return cxx::unexpected(documents.error());
        auto resolved = project::resolveDesktopSettings(builtin_settings_, *documents);
        if (!resolved)
            return applicationFailure("settings.bootstrap.resolve", resolved.error());
        return std::move(*resolved);
    }
    EditorResult<void> EditorApplication::Impl::activateSettings()
    {
        auto documents = readSettings(settings_content_->locations);
        if (!documents)
            return cxx::unexpected(documents.error());
        const auto snapshot = contributions_.snapshot();
        for (const auto& page : snapshot.settings())
        {
            auto effective = settings::resolveSettings(page.entry, *documents);
            if (!effective)
                return applicationFailure("settings.activate.resolve", effective.error());
            const auto type = effective->desired.type();
            const bool is_builtin = std::ranges::any_of(builtin_settings_, [&](const auto& builtin) {
                return builtin.entry == page.entry;
            });
            if (is_builtin && type == cxx::typeToken<project::AppearanceSettings>())
            {
                auto& actual = *static_cast<project::AppearanceSettings*>(effective->desired.data());
                actual.scale = desktop_->root().scale();
                const auto path = config_.font ? config_.font->generic_u8string() : std::u8string{};
                actual.font.assign(path.begin(), path.end());
            }
            else if (is_builtin && type == cxx::typeToken<project::WindowSettings>())
            {
                if (!window_)
                    continue; // Offscreen has no native placement; do not fabricate an applied fact.
                auto observed = window_->state();
                if (!observed)
                    return applicationFailure("settings.window.observe", observed.error());
                static_cast<project::WindowSettings*>(effective->desired.data())->placement = observed->placement;
            }
            else
            {
                if (!page.entry->hasApply())
                    continue; // Restart-only constructor parameters have no receiver-confirmed fact here.
                auto applied = page.entry->apply(effective->desired);
                if (!applied)
                    return applicationFailure("settings.activate", applied.error());
            }
            std::vector<std::byte> bytes;
            auto encoded = effective->desired.encode(bytes);
            if (!encoded)
                return applicationFailure("settings.applied.encode", encoded.error());
            settings_content_->applied.push_back({page.entry, std::move(bytes)});
        }
        if (window_)
        {
            const auto entry = std::ranges::find_if(builtin_settings_, [](const auto& page) {
                return page.entry->descriptor().configuration->codec.type == cxx::typeToken<project::WindowSettings>();
            });
            auto bound = project::WindowSettingsBinding::create(
                *window_, workspace_, workspace_changes_, entry->entry, *settings_content_);
            if (!bound)
                return cxx::unexpected(bound.error());
            window_settings_ = std::move(*bound);
        }
        return {};
    }
}
