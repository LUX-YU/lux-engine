#include <lux/engine/editor/extensions/Contributions.hpp>
#include <lux/engine/editor/configuration/EditorReflection.hpp>
#include <deque>
#include <algorithm>
#include <thread>

namespace lux::editor::extensions
{
    struct ContributionSnapshot::Data final
    {
        std::vector<lux::object::CodeLease> code;
        std::shared_ptr<const void> reflection_lifetime;
        std::vector<ReflectionContribution> reflection;
        commands::CommandRegistrySnapshot commands;
        sessions::SessionFactorySnapshot sessions;
        views::ViewFactorySnapshot views;
        std::vector<scene::ConfigurationEditor> configurations;
        std::vector<scene::InspectorComponent> components;
        std::vector<settings::SettingsPage> settings;
        std::vector<std::pair<std::uint64_t, std::size_t>> setting_index;
    };
    ContributionResult<ContributionSnapshot> ContributionSnapshot::prepare(
        ContributionDraft draft,
        std::size_t capacity
    )
    {
        if (draft.configurations.size() > capacity || draft.components.size() > capacity ||
            draft.reflection.size() > capacity)
            return cxx::unexpected(ContributionFailure{EContributionError::CAPACITY, "contributions"});
        std::vector<std::shared_ptr<settings::SettingsEntry>> setting_entries;
        setting_entries.reserve(draft.settings.size());
        for (const auto& item : draft.settings)
            setting_entries.push_back(item.entry);
        auto valid_settings = settings::validateSettingsEntries(setting_entries, capacity);
        if (!valid_settings)
            return cxx::unexpected(ContributionFailure{
                EContributionError::INVALID_ARGUMENT,
                "settings",
                static_cast<std::uint64_t>(valid_settings.error().code),
                valid_settings.error().detail
            });
        std::vector<std::pair<std::uint64_t, std::size_t>> setting_index;
        setting_index.reserve(draft.settings.size());
        for (std::size_t i{}; i < draft.settings.size(); ++i)
        {
            auto& item = draft.settings[i];
            const auto code = item.entry->code();
            item.entry = lux::object::pinCodeOwner(code, std::move(item.entry));
            setting_index.emplace_back(item.entry->descriptor().id.hash(), i);
        }
        std::ranges::sort(setting_index);
        auto commands = commands::CommandRegistrySnapshot::create(std::move(draft.commands), capacity);
        if (!commands)
            return cxx::unexpected(ContributionFailure{
                EContributionError::INVALID_ARGUMENT,
                "commands",
                static_cast<std::uint64_t>(commands.error().code),
                commands.error().detail
            });
        auto sessions = sessions::SessionFactorySnapshot::create(std::move(draft.sessions), capacity);
        if (!sessions)
            return cxx::unexpected(ContributionFailure{
                EContributionError::INVALID_ARGUMENT,
                "sessions",
                static_cast<std::uint64_t>(sessions.error().code),
                sessions.error().detail
            });
        auto views = views::ViewFactorySnapshot::create(std::move(draft.views), capacity);
        if (!views)
            return cxx::unexpected(ContributionFailure{
                EContributionError::INVALID_ARGUMENT,
                "views",
                static_cast<std::uint64_t>(views.error().code),
                views.error().detail
            });
        for (std::size_t i{}; i < draft.configurations.size(); ++i)
        {
            const auto& item = draft.configurations[i];
            const bool invalid = !item.code.valid() || !item.create || item.value.schema_name.empty() ||
                                 !item.value.schema_version || !item.value.codec.valid() || !item.value.reflection;
            if (invalid)
                return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "configuration"});
            for (std::size_t j{}; j < i; ++j)
                if (draft.configurations[j].value.schema_name == item.value.schema_name &&
                    draft.configurations[j].value.schema_version == item.value.schema_version)
                    return cxx::unexpected(ContributionFailure{EContributionError::DUPLICATE, "configuration"});
        }
        for (std::size_t i{}; i < draft.components.size(); ++i)
        {
            const auto& item = draft.components[i];
            if (!item.type.isValid() || !item.create || item.label.empty())
                return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "component"});
            for (std::size_t j{}; j < i; ++j)
                if (draft.components[j].type == item.type)
                    return cxx::unexpected(ContributionFailure{EContributionError::DUPLICATE, "component"});
        }
        for (const auto& entry : draft.reflection)
            if (!entry.code.valid() || !entry.register_types)
                return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "reflection"});
        auto lifetime = draft.reflection.empty() && draft.configurations.empty() && draft.settings.empty()
                            ? std::shared_ptr<const void>{}
                            : acquireEditorReflection();
        ContributionSnapshot result;
        result.data_ = std::make_shared<Data>(
            std::move(draft.code),
            std::move(lifetime),
            std::move(draft.reflection),
            std::move(*commands),
            std::move(*sessions),
            std::move(*views),
            std::move(draft.configurations),
            std::move(draft.components),
            std::move(draft.settings),
            std::move(setting_index)
        );
        return result;
    }
    const commands::CommandRegistrySnapshot& ContributionSnapshot::commands() const noexcept
    {
        return data_->commands;
    }
    const sessions::SessionFactorySnapshot& ContributionSnapshot::sessions() const noexcept
    {
        return data_->sessions;
    }
    const views::ViewFactorySnapshot& ContributionSnapshot::views() const noexcept
    {
        return data_->views;
    }
    std::span<const scene::ConfigurationEditor> ContributionSnapshot::configurations() const noexcept
    {
        return data_->configurations;
    }
    std::span<const scene::InspectorComponent> ContributionSnapshot::components() const noexcept
    {
        return data_->components;
    }
    std::span<const settings::SettingsPage> ContributionSnapshot::settings() const noexcept
    {
        return data_->settings;
    }
    const settings::SettingsPage* ContributionSnapshot::findSetting(settings::SettingsIdView id) const noexcept
    {
        if (!data_)
            return nullptr;
        auto found =
            std::ranges::lower_bound(data_->setting_index, id.hash(), {}, [](const auto& row) { return row.first; });
        const bool is_missing = found == data_->setting_index.end() || found->first != id.hash();
        if (is_missing)
            return nullptr;
        const auto& item = data_->settings[found->second];
        return item.entry->descriptor().id.name() == id.name() ? &item : nullptr;
    }
    bool ContributionSnapshot::valid() const noexcept
    {
        return bool(data_);
    }
    struct ContributionRegistry::Impl final
    {
        commands::CommandRegistry& commands;
        std::size_t capacity;
        std::thread::id owner{std::this_thread::get_id()};
        std::deque<ContributionSnapshot> pending;
        ContributionSnapshot current;
        bool active{};
        std::uint64_t revision{};
        ContributionResult<void> admission() const
        {
            if (std::this_thread::get_id() != owner)
                return cxx::unexpected(ContributionFailure{EContributionError::WRONG_THREAD, "contributions"});
            if (active)
                return cxx::unexpected(ContributionFailure{EContributionError::BUSY, "contributions"});
            return {};
        }
        struct Scope final
        {
            bool& value;
            explicit Scope(bool& value) : value(value)
            {
                value = true;
            }
            ~Scope()
            {
                value = false;
            }
            Scope(const Scope&) = delete;
            Scope& operator=(const Scope&) = delete;
        };
    };
    ContributionRegistry::ContributionRegistry(
        object::ObjectDispatcherRef dispatcher,
        commands::CommandRegistry& commands,
        std::size_t capacity
    )
        : LuxObject(dispatcher), impl_(std::make_unique<Impl>(commands, capacity))
    {
    }
    ContributionRegistry::~ContributionRegistry() = default;
    ContributionResult<void> ContributionRegistry::enqueue(ContributionSnapshot& candidate)
    {
        if (std::this_thread::get_id() != impl_->owner)
            return cxx::unexpected(ContributionFailure{EContributionError::WRONG_THREAD, "contributions"});
        if (!candidate.valid())
            return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "contributions"});
        if (impl_->pending.size() >= impl_->capacity)
            return cxx::unexpected(ContributionFailure{EContributionError::CAPACITY, "contributions"});
        impl_->pending.push_back(std::move(candidate));
        return {};
    }
    ContributionResult<object::SignalDelivery> ContributionRegistry::applyPending()
    {
        auto ready = impl_->admission();
        if (!ready)
            return cxx::unexpected(ready.error());
        if (impl_->pending.empty())
            return object::SignalDelivery{};
        if (impl_->revision == UINT64_MAX)
            return cxx::unexpected(ContributionFailure{EContributionError::CAPACITY, "revision"});
        Impl::Scope scope{impl_->active};
        // Acquire the participant's one-use publication permission before any foreign callback. Its
        // scope is nested inside ours, so abandoned command owners also clean up under both guards.
        auto publication = impl_->commands.preparePublication(impl_->pending.front().commands());
        if (!publication)
            return cxx::unexpected(ContributionFailure{
                publication.error().code == commands::ECommandError::WRONG_THREAD ? EContributionError::WRONG_THREAD
                : publication.error().code == commands::ECommandError::BUSY       ? EContributionError::BUSY
                                                                                  : EContributionError::CAPACITY,
                "commands",
                static_cast<std::uint64_t>(publication.error().code)
            });
        {
            auto candidate = std::move(impl_->pending.front());
            impl_->pending.pop_front();
            // Foreign reflection callbacks prepare only the original registry's isolated draft. They
            // may queue a later contribution, but cannot publish during this batch. Failure discards
            // the whole candidate and leaves every live catalog unchanged.
            if (candidate.data_->reflection_lifetime)
            {
                auto reflection = meta::ReflectionRegistry::beginDraft();
                for (const auto& entry : candidate.data_->reflection)
                {
                    auto appended =
                        reflection.appendOnce(entry.register_types, std::make_shared<lux::object::CodeLease>(entry.code));
                    if (!appended)
                        return cxx::unexpected(ContributionFailure{
                            EContributionError::CALLBACK,
                            "reflection.register",
                            static_cast<std::uint64_t>(appended.error().error)
                        });
                }
                for (const auto& entry : candidate.data_->configurations)
                {
                    const auto* type = entry.value.reflection(*reflection.registry());
                    const bool mismatch = !type || type->type.ptr != type ||
                                          type->type.hash != entry.value.codec.type.hash() ||
                                          type->type.name != entry.value.codec.type.name();
                    if (mismatch)
                        return cxx::unexpected(ContributionFailure{
                            EContributionError::INVALID_ARGUMENT,
                            "configuration.reflection",
                            0,
                            entry.value.schema_name
                        });
                }
                for (const auto& item : candidate.data_->settings)
                {
                    // Default decode/validation is foreign code too: all participating guards remain
                    // active until the temporary value and any rejected candidate are destroyed.
                    auto value = item.entry->validateDefault(*reflection.registry());
                    if (!value)
                        return cxx::unexpected(ContributionFailure{
                            EContributionError::INVALID_ARGUMENT,
                            "settings.default",
                            static_cast<std::uint64_t>(value.error().code),
                            value.error().detail
                        });
                }
                auto committed = reflection.commit();
                if (!committed)
                    return cxx::unexpected(ContributionFailure{
                        EContributionError::CALLBACK,
                        "reflection.commit",
                        static_cast<std::uint64_t>(committed.error().error)
                    });
            }
            // Prepared, non-allocating swaps. Old callbacks cannot run between the two publications.
            auto previous_commands = publication->commit();
            auto previous = std::exchange(impl_->current, std::move(candidate));
            ++impl_->revision;
            // Cleanup under scope; destructors can enqueue, but cannot recursively publish.
        }
        return emit(changed, impl_->revision);
    }
    ContributionResult<void> ContributionRegistry::withSnapshot(
        cxx::function_ref<ContributionResult<void>(const ContributionSnapshot&)> callback
    )
    {
        auto ready = impl_->admission();
        if (!ready)
            return ready;
        if (!impl_->current.valid())
            return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "contributions.empty"});
        Impl::Scope scope{impl_->active};
        auto commands = impl_->commands.readBatch();
        if (!commands)
            return cxx::unexpected(ContributionFailure{
                commands.error().code == commands::ECommandError::WRONG_THREAD ? EContributionError::WRONG_THREAD
                                                                               : EContributionError::BUSY,
                "commands",
                static_cast<std::uint64_t>(commands.error().code)
            });
        auto pinned = impl_->current;
        return callback(pinned);
    }
    ContributionSnapshot ContributionRegistry::snapshot() const noexcept
    {
        return impl_->current;
    }
    std::uint64_t ContributionRegistry::revision() const noexcept
    {
        return impl_->revision;
    }
} // namespace lux::editor::extensions
