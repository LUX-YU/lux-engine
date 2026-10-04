#include <algorithm>
#include <deque>
#include <lux/engine/editor/configuration/EditorReflection.hpp>
#include <lux/engine/editor/extensions/Contributions.hpp>
#include <thread>

namespace lux::editor::extensions
{
    struct ContributionSnapshot::Data final
    {
        std::vector<lux::object::CodeLease> code;
        std::shared_ptr<const void> reflection_lifetime;
        std::vector<ReflectionContribution> reflection;
        std::vector<std::shared_ptr<const services::ServiceEntry>> services;
        desktop::UiCatalog ui;
        commands::CommandRegistrySnapshot commands;
        sessions::SessionFactorySnapshot sessions;
        views::ViewFactorySnapshot views;
        std::vector<settings::SettingsPage> settings;
        std::vector<std::pair<std::uint64_t, std::size_t>> setting_index;
    };
    ContributionResult<ContributionSnapshot> ContributionSnapshot::prepare(
        ContributionDraft draft,
        std::size_t capacity
    )
    {
        const bool is_over_capacity = draft.reflection.size() > capacity || draft.services.size() > capacity;
        if (is_over_capacity)
        {
            return cxx::unexpected(ContributionFailure{EContributionError::CAPACITY, "contributions"});
        }
        if (auto valid = services::validateServiceEntries(draft.services, capacity); !valid)
        {
            return cxx::unexpected(ContributionFailure{
                EContributionError::INVALID_ARGUMENT,
                "services",
                static_cast<std::uint64_t>(valid.error().code),
                std::move(valid.error().detail)
            });
        }
        auto ui = desktop::UiCatalog::prepare(std::move(draft.ui), capacity);
        if (!ui)
        {
            return cxx::unexpected(ContributionFailure{
                EContributionError::INVALID_ARGUMENT,
                "ui",
                static_cast<std::uint64_t>(ui.error().code),
                std::move(ui.error().detail)
            });
        }
        std::vector<std::shared_ptr<settings::SettingsEntry>> setting_entries;
        setting_entries.reserve(draft.settings.size());
        for (const auto& item : draft.settings)
        {
            setting_entries.push_back(item.entry);
        }
        auto valid_settings = settings::validateSettingsEntries(setting_entries, capacity);
        if (!valid_settings)
        {
            return cxx::unexpected(ContributionFailure{
                EContributionError::INVALID_ARGUMENT,
                "settings",
                static_cast<std::uint64_t>(valid_settings.error().code),
                valid_settings.error().detail
            });
        }
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
        {
            return cxx::unexpected(ContributionFailure{
                EContributionError::INVALID_ARGUMENT,
                "commands",
                static_cast<std::uint64_t>(commands.error().code),
                commands.error().detail
            });
        }
        auto sessions = sessions::SessionFactorySnapshot::create(std::move(draft.sessions), capacity);
        if (!sessions)
        {
            return cxx::unexpected(ContributionFailure{
                EContributionError::INVALID_ARGUMENT,
                "sessions",
                static_cast<std::uint64_t>(sessions.error().code),
                sessions.error().detail
            });
        }
        auto views = views::ViewFactorySnapshot::create(std::move(draft.views), capacity);
        if (!views)
        {
            return cxx::unexpected(ContributionFailure{
                EContributionError::INVALID_ARGUMENT,
                "views",
                static_cast<std::uint64_t>(views.error().code),
                views.error().detail
            });
        }
        for (const auto& entry : draft.reflection)
        {
            const bool is_invalid_entry = !entry.code.valid() || (!entry.register_types && !entry.validate);
            if (is_invalid_entry)
            {
                return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "reflection"});
            }
        }
        const bool needs_reflection = !draft.reflection.empty() || !draft.settings.empty();
        auto lifetime = needs_reflection ? acquireEditorReflection() : std::shared_ptr<const void>{};
        ContributionSnapshot result;
        result.data_ = std::make_shared<Data>(
            std::move(draft.code),
            std::move(lifetime),
            std::move(draft.reflection),
            std::move(draft.services),
            std::move(*ui),
            std::move(*commands),
            std::move(*sessions),
            std::move(*views),
            std::move(draft.settings),
            std::move(setting_index)
        );
        return result;
    }
    const commands::CommandRegistrySnapshot& ContributionSnapshot::commands() const noexcept
    {
        return data_->commands;
    }
    std::span<const std::shared_ptr<const services::ServiceEntry>> ContributionSnapshot::services() const noexcept
    {
        return data_->services;
    }
    const desktop::UiCatalog& ContributionSnapshot::ui() const noexcept
    {
        return data_->ui;
    }
    const sessions::SessionFactorySnapshot& ContributionSnapshot::sessions() const noexcept
    {
        return data_->sessions;
    }
    const views::ViewFactorySnapshot& ContributionSnapshot::views() const noexcept
    {
        return data_->views;
    }
    std::span<const settings::SettingsPage> ContributionSnapshot::settings() const noexcept
    {
        return data_->settings;
    }
    const settings::SettingsPage* ContributionSnapshot::findSetting(settings::SettingsIdView id) const noexcept
    {
        if (!data_)
        {
            return nullptr;
        }
        auto found =
            std::ranges::lower_bound(data_->setting_index, id.hash(), {}, [](const auto& row) { return row.first; });
        const bool is_missing = found == data_->setting_index.end() || found->first != id.hash();
        if (is_missing)
        {
            return nullptr;
        }
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
        services::ServiceRegistry& services;
        desktop::UiRegistry& ui;
        std::size_t capacity;
        std::thread::id owner{std::this_thread::get_id()};
        std::deque<ContributionSnapshot> pending;
        ContributionSnapshot current;
        bool active{};
        std::uint64_t revision{};
        ContributionResult<void> admission() const
        {
            if (std::this_thread::get_id() != owner)
            {
                return cxx::unexpected(ContributionFailure{EContributionError::WRONG_THREAD, "contributions"});
            }
            if (active)
            {
                return cxx::unexpected(ContributionFailure{EContributionError::BUSY, "contributions"});
            }
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
        desktop::EditorContext& context,
        std::size_t capacity
    )
        : LuxObject(dispatcher),
          impl_(std::make_unique<Impl>(context.commands(), context.services(), context.ui(), capacity))
    {
    }
    ContributionRegistry::~ContributionRegistry() = default;
    ContributionResult<void> ContributionRegistry::enqueue(ContributionSnapshot& candidate)
    {
        if (std::this_thread::get_id() != impl_->owner)
        {
            return cxx::unexpected(ContributionFailure{EContributionError::WRONG_THREAD, "contributions"});
        }
        if (!candidate.valid())
        {
            return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "contributions"});
        }
        if (impl_->pending.size() >= impl_->capacity)
        {
            return cxx::unexpected(ContributionFailure{EContributionError::CAPACITY, "contributions"});
        }
        impl_->pending.push_back(std::move(candidate));
        return {};
    }
    ContributionResult<object::SignalDelivery> ContributionRegistry::applyPending()
    {
        auto ready = impl_->admission();
        if (!ready)
        {
            return cxx::unexpected(ready.error());
        }
        if (impl_->pending.empty())
        {
            return object::SignalDelivery{};
        }
        if (impl_->revision == UINT64_MAX)
        {
            return cxx::unexpected(ContributionFailure{EContributionError::CAPACITY, "revision"});
        }
        Impl::Scope scope{impl_->active};
        // Acquire the participant's one-use publication permission before any foreign callback. Its
        // scope is nested inside ours, so abandoned command owners also clean up under both guards.
        auto publication = impl_->commands.preparePublication(impl_->pending.front().commands());
        if (!publication)
        {
            return cxx::unexpected(ContributionFailure{
                publication.error().code == commands::ECommandError::WRONG_THREAD ? EContributionError::WRONG_THREAD
                : publication.error().code == commands::ECommandError::BUSY       ? EContributionError::BUSY
                                                                                  : EContributionError::CAPACITY,
                "commands",
                static_cast<std::uint64_t>(publication.error().code)
            });
        }
        const auto& pending = impl_->pending.front();
        auto services = impl_->services.preparePublication({pending.services().begin(), pending.services().end()});
        if (!services)
        {
            return cxx::unexpected(ContributionFailure{
                services.error().code == services::EServiceError::BUSY ? EContributionError::BUSY
                                                                       : EContributionError::INVALID_ARGUMENT,
                "services",
                static_cast<std::uint64_t>(services.error().code),
                std::move(services.error().detail)
            });
        }
        auto ui = impl_->ui.preparePublication(pending.ui());
        if (!ui)
        {
            return cxx::unexpected(ContributionFailure{
                ui.error().code == desktop::EUiError::BUSY ? EContributionError::BUSY
                                                           : EContributionError::INVALID_ARGUMENT,
                "ui",
                static_cast<std::uint64_t>(ui.error().code),
                std::move(ui.error().detail)
            });
        }
        // All three permissions remain held while any retired or rejected callback is destroyed.
        // The pending snapshot pins inputs until all guards have been acquired.
        struct Cleanup final
        {
            commands::CommandRegistry::Batch& commands;
            services::ServiceRegistry::Publication& services;
            desktop::UiRegistry::Publication& ui;
            ~Cleanup()
            {
                commands.clearRetained();
                ui.clearRetained();
                services.clearRetained();
            }
        } cleanup{*publication, *services, *ui};
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
                    if (!entry.register_types)
                    {
                        continue;
                    }
                    auto appended = reflection.appendOnce(
                        entry.register_types,
                        std::make_shared<lux::object::CodeLease>(entry.code)
                    );
                    if (!appended)
                    {
                        return cxx::unexpected(ContributionFailure{
                            EContributionError::CALLBACK,
                            "reflection.register",
                            static_cast<std::uint64_t>(appended.error().error)
                        });
                    }
                }
                for (const auto& entry : candidate.data_->reflection)
                {
                    if (!entry.validate)
                    {
                        continue;
                    }
                    auto valid = entry.validate(*reflection.registry(), candidate.services());
                    if (!valid)
                    {
                        return cxx::unexpected(ContributionFailure{
                            EContributionError::INVALID_ARGUMENT,
                            std::move(valid.error().domain),
                            static_cast<std::uint64_t>(valid.error().code),
                            std::move(valid.error().detail)
                        });
                    }
                }
                for (const auto& item : candidate.data_->settings)
                {
                    // Default decode/validation is foreign code too: all participating guards remain
                    // active until the temporary value and any rejected candidate are destroyed.
                    auto value = item.entry->validateDefault(*reflection.registry());
                    if (!value)
                    {
                        return cxx::unexpected(ContributionFailure{
                            EContributionError::INVALID_ARGUMENT,
                            "settings.default",
                            static_cast<std::uint64_t>(value.error().code),
                            value.error().detail
                        });
                    }
                }
                auto committed = reflection.commit();
                if (!committed)
                {
                    return cxx::unexpected(ContributionFailure{
                        EContributionError::CALLBACK,
                        "reflection.commit",
                        static_cast<std::uint64_t>(committed.error().error)
                    });
                }
            }
            // Prepared, non-allocating swaps. Old callbacks cannot run between participating publications.
            auto previous_commands = publication->commit();
            services->commit();
            ui->commit();
            auto previous = std::exchange(impl_->current, std::move(candidate));
            ++impl_->revision;
            // Cleanup under scope; destructors can enqueue, but cannot recursively publish.
        }
        publication->clearRetained();
        ui->clearRetained();
        services->clearRetained();
        return emit(changed, impl_->revision);
    }
    ContributionResult<void> ContributionRegistry::withSnapshot(
        cxx::function_ref<ContributionResult<void>(const ContributionSnapshot&)> callback
    )
    {
        auto ready = impl_->admission();
        if (!ready)
        {
            return ready;
        }
        if (!impl_->current.valid())
        {
            return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "contributions.empty"});
        }
        Impl::Scope scope{impl_->active};
        auto commands = impl_->commands.readBatch();
        if (!commands)
        {
            return cxx::unexpected(ContributionFailure{
                commands.error().code == commands::ECommandError::WRONG_THREAD ? EContributionError::WRONG_THREAD
                                                                               : EContributionError::BUSY,
                "commands",
                static_cast<std::uint64_t>(commands.error().code)
            });
        }
        auto services = impl_->services.readScope();
        if (!services)
        {
            return cxx::unexpected(ContributionFailure{
                EContributionError::BUSY,
                "services",
                static_cast<std::uint64_t>(services.error().code)
            });
        }
        auto ui = impl_->ui.readScope();
        if (!ui)
        {
            return cxx::unexpected(
                ContributionFailure{EContributionError::BUSY, "ui", static_cast<std::uint64_t>(ui.error().code)}
            );
        }
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
