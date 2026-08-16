#include <lux/engine/editor/extensions/EditorTools.hpp>

#include <lux/engine/events/DomainEvents.hpp>

#include <moodycamel/concurrentqueue.h>

#include <algorithm>
#include <atomic>
#include <utility>

namespace lux::editor
{
    namespace
    {
        [[nodiscard]] bool sameId(
            const lux::extensions::ContributionId& lhs,
            lux::extensions::ContributionIdView rhs) noexcept
        {
            return lux::extensions::sameStableId(lhs.view(), rhs);
        }

        using TicketPublisher = lux::extensions::OperationTicketPublisher<
            EEditorToolPhase,
            EEditorToolError,
            EditorToolResult>;

        enum class ECommandKind : std::uint8_t
        {
            OPEN,
            SET_VISIBLE,
            DEACTIVATE
        };

        struct EditorToolCommand final
        {
            ECommandKind kind{ECommandKind::OPEN};
            lux::extensions::ContributionId id;
            bool visible{true};
            std::uint64_t generation{0u};
            TicketPublisher publisher{EEditorToolPhase::QUEUED, 0u};

            EditorToolCommand(
                ECommandKind command_kind,
                lux::extensions::ContributionId command_id,
                bool command_visible,
                std::uint64_t command_generation)
                : kind(command_kind),
                  id(std::move(command_id)),
                  visible(command_visible),
                  generation(command_generation),
                  publisher(EEditorToolPhase::QUEUED, command_generation)
            {}
        };
    }

    lux::cxx::expected<void, EEditorServiceRegistrationError>
    EditorPanelCreateContext::addErased(Entry entry)
    {
        if (!entry.type || entry.value == nullptr)
        {
            return lux::cxx::unexpected(
                EEditorServiceRegistrationError::INVALID_SERVICE);
        }
        for (const auto& existing : entries_)
        {
            if (existing.type.hash != entry.type.hash)
                continue;
            return lux::cxx::unexpected(
                existing.type.name == entry.type.name
                    ? EEditorServiceRegistrationError::DUPLICATE_TYPE
                    : EEditorServiceRegistrationError::TYPE_HASH_COLLISION);
        }
        entries_.push_back(std::move(entry));
        return {};
    }

    bool EditorPanelCreateContext::contains(
        lux::ecs::TypeToken type) const noexcept
    {
        return std::ranges::any_of(
            entries_,
            [type](const Entry& entry) noexcept
            {
                return lux::ecs::sameTypeToken(entry.type, type);
            });
    }

    lux::cxx::expected<void, EEditorPanelCatalogError>
    EditorPanelCatalog::add(EditorPanelContributionDescriptor descriptor)
    {
        std::vector<EditorPanelContributionDescriptor> batch;
        batch.push_back(std::move(descriptor));
        return addBatch(std::move(batch));
    }

    lux::cxx::expected<void, EEditorPanelCatalogError>
    EditorPanelCatalog::validateBatch(
        std::span<const EditorPanelContributionDescriptor> descriptors)
        const noexcept
    {
        for (std::size_t index = 0u; index < descriptors.size(); ++index)
        {
            const auto& descriptor = descriptors[index];
            if (!descriptor.id.isValid() || !descriptor.provider.isValid() ||
                !lux::extensions::isCanonicalStableName(
                    descriptor.id.name()) ||
                !lux::extensions::isCanonicalStableName(
                    descriptor.provider.name()))
            {
                return lux::cxx::unexpected(
                    EEditorPanelCatalogError::INVALID_DESCRIPTOR);
            }
            if (!descriptor.create)
                return lux::cxx::unexpected(
                    EEditorPanelCatalogError::MISSING_CREATE_CALLBACK);
            for (const auto& current : descriptors_)
            {
                if (lux::extensions::stableIdCollision(
                        current.id.view(), descriptor.id.view()))
                    return lux::cxx::unexpected(
                        EEditorPanelCatalogError::ID_COLLISION);
                if (sameId(current.id, descriptor.id.view()))
                    return lux::cxx::unexpected(
                        EEditorPanelCatalogError::DUPLICATE_CONTRIBUTION);
            }
            for (std::size_t other = 0u; other < index; ++other)
            {
                if (lux::extensions::stableIdCollision(
                        descriptors[other].id.view(), descriptor.id.view()))
                    return lux::cxx::unexpected(
                        EEditorPanelCatalogError::ID_COLLISION);
                if (sameId(descriptors[other].id, descriptor.id.view()))
                    return lux::cxx::unexpected(
                        EEditorPanelCatalogError::DUPLICATE_CONTRIBUTION);
            }
        }
        return {};
    }

    lux::cxx::expected<void, EEditorPanelCatalogError>
    EditorPanelCatalog::addBatch(
        std::vector<EditorPanelContributionDescriptor> descriptors)
    {
        if (auto checked = validateBatch(descriptors); !checked)
            return checked;
        descriptors_.reserve(descriptors_.size() + descriptors.size());
        for (auto& descriptor : descriptors)
            descriptors_.push_back(std::move(descriptor));
        return {};
    }

    EditorPanelContributionDescriptor* EditorPanelCatalog::find(
        lux::extensions::ContributionIdView id) noexcept
    {
        const auto found = std::ranges::find_if(
            descriptors_,
            [id](const auto& descriptor) noexcept
            {
                return sameId(descriptor.id, id);
            });
        return found == descriptors_.end() ? nullptr : &*found;
    }

    const EditorPanelContributionDescriptor* EditorPanelCatalog::find(
        lux::extensions::ContributionIdView id) const noexcept
    {
        const auto found = std::ranges::find_if(
            descriptors_,
            [id](const auto& descriptor) noexcept
            {
                return sameId(descriptor.id, id);
            });
        return found == descriptors_.end() ? nullptr : &*found;
    }

    std::span<const EditorPanelContributionDescriptor>
    EditorPanelCatalog::all() const noexcept
    {
        return descriptors_;
    }

    namespace detail
    {
        struct EditorToolEndpoint final
        {
            explicit EditorToolEndpoint(std::size_t capacity)
                : queue(std::max<std::size_t>(capacity, 1u)),
                  capacity(std::max<std::size_t>(capacity, 1u))
            {}

            [[nodiscard]] EditorToolTicket submit(
                ECommandKind kind,
                lux::extensions::ContributionIdView id,
                bool visible)
            {
                const auto generation = next_generation.fetch_add(
                    1u,
                    std::memory_order_relaxed);
                EditorToolCommand command{
                    kind,
                    lux::extensions::ContributionId{id.name()},
                    visible,
                    generation};
                auto ticket = command.publisher.ticket();
                if (!admission_open.load(std::memory_order_acquire))
                {
                    command.publisher.fail(EEditorToolError::STOPPING);
                    return ticket;
                }
                if (!id.isValid())
                {
                    command.publisher.fail(
                        EEditorToolError::UNKNOWN_CONTRIBUTION);
                    return ticket;
                }
                auto count = queued.fetch_add(1u, std::memory_order_acq_rel);
                if (count >= capacity)
                {
                    queued.fetch_sub(1u, std::memory_order_release);
                    command.publisher.fail(EEditorToolError::QUEUE_FULL);
                    return ticket;
                }
                auto terminal = command.publisher;
                if (!queue.try_enqueue(std::move(command)))
                {
                    queued.fetch_sub(1u, std::memory_order_release);
                    terminal.fail(EEditorToolError::QUEUE_FULL);
                }
                return ticket;
            }

            moodycamel::ConcurrentQueue<EditorToolCommand> queue;
            const std::size_t capacity;
            std::atomic<std::size_t> queued{0u};
            std::atomic<std::uint64_t> next_generation{1u};
            std::atomic<bool> admission_open{true};
            std::atomic<const EditorToolHost*> host{nullptr};
        };
    }

    EditorTools::EditorTools(
        std::shared_ptr<detail::EditorToolEndpoint> endpoint) noexcept
        : endpoint_(std::move(endpoint))
    {}

    EditorToolTicket EditorTools::requestOpen(
        lux::extensions::ContributionIdView id) const
    {
        if (endpoint_)
            return endpoint_->submit(ECommandKind::OPEN, id, true);
        TicketPublisher publisher(EEditorToolPhase::QUEUED, 0u);
        auto ticket = publisher.ticket();
        publisher.fail(EEditorToolError::STOPPING);
        return ticket;
    }

    EditorToolTicket EditorTools::requestVisible(
        lux::extensions::ContributionIdView id,
        bool visible) const
    {
        if (endpoint_)
            return endpoint_->submit(ECommandKind::SET_VISIBLE, id, visible);
        TicketPublisher publisher(EEditorToolPhase::QUEUED, 0u);
        auto ticket = publisher.ticket();
        publisher.fail(EEditorToolError::STOPPING);
        return ticket;
    }

    EditorToolTicket EditorTools::requestDeactivate(
        lux::extensions::ContributionIdView id) const
    {
        if (endpoint_)
            return endpoint_->submit(ECommandKind::DEACTIVATE, id, false);
        TicketPublisher publisher(EEditorToolPhase::QUEUED, 0u);
        auto ticket = publisher.ticket();
        publisher.fail(EEditorToolError::STOPPING);
        return ticket;
    }

    std::vector<EditorToolSnapshot> EditorTools::snapshot() const
    {
        const auto* host = endpoint_
            ? endpoint_->host.load(std::memory_order_acquire)
            : nullptr;
        return host ? host->snapshot() : std::vector<EditorToolSnapshot>{};
    }

    EditorTools::operator bool() const noexcept
    {
        return endpoint_ &&
            endpoint_->admission_open.load(std::memory_order_acquire);
    }

    struct EditorToolHost::Impl final
    {
        struct Active final
        {
            // The module lease is declared first and therefore destroyed last.
            // Panel vtables and the UI registration disappear before code can.
            lux::extensions::ModuleLease module;
            lux::extensions::ContributionId id;
            std::unique_ptr<lux::ui::Panel> panel;
            lux::ui::PanelRegistration registration;
            std::uint64_t generation{0u};
            bool supports_deactivation{true};
        };

        Impl(
            lux::ui::UISystem& ui_value,
            EditorPanelCatalog& catalog_value,
            EditorPanelCreateContext context_value,
            lux::events::DomainEvents* events_value,
            std::size_t capacity)
            : ui(ui_value),
              catalog(catalog_value),
              context(std::move(context_value)),
              events(events_value),
              endpoint(std::make_shared<detail::EditorToolEndpoint>(capacity))
        {}

        [[nodiscard]] std::size_t findActive(
            lux::extensions::ContributionIdView id) const noexcept
        {
            for (std::size_t index = 0u; index < active.size(); ++index)
                if (sameId(active[index]->id, id))
                    return index;
            return active.size();
        }

        void finish(
            EditorToolCommand& command,
            bool is_active,
            bool visible)
        {
            command.publisher.succeed(EditorToolResult{
                command.id,
                command.generation,
                is_active,
                visible});
            if (events)
                events->publish(EditorToolStateChanged{
                    command.id,
                    command.generation,
                    is_active,
                    visible});
        }

        void process(EditorToolCommand& command)
        {
            const auto index = findActive(command.id.view());
            if (command.kind == ECommandKind::OPEN)
            {
                if (index != active.size())
                {
                    active[index]->panel->setVisible(true);
                    finish(command, true, true);
                    return;
                }
                auto* descriptor = catalog.find(command.id.view());
                if (!descriptor)
                {
                    command.publisher.fail(
                        EEditorToolError::UNKNOWN_CONTRIBUTION);
                    return;
                }
                for (const auto service : descriptor->required_editor_services)
                {
                    if (!context.contains(service))
                    {
                        command.publisher.fail(
                            EEditorToolError::REQUIRED_SERVICE_MISSING);
                        return;
                    }
                }
                command.publisher.setPhase(EEditorToolPhase::CREATING);
                auto created = descriptor->create(context);
                if (!created || !*created)
                {
                    command.publisher.fail(EEditorToolError::CREATE_FAILED);
                    return;
                }
                (*created)->setVisible(descriptor->default_visible);
                command.publisher.setPhase(EEditorToolPhase::REGISTERING_UI);
                auto registered = ui.registerPanel(**created);
                if (!registered)
                {
                    command.publisher.fail(
                        EEditorToolError::UI_REGISTRATION_FAILED);
                    return;
                }
                auto value = std::make_unique<Active>();
                value->id = descriptor->id;
                value->panel = std::move(*created);
                value->registration = std::move(*registered);
                value->generation = command.generation;
                value->supports_deactivation =
                    descriptor->supports_deactivation;
                value->module = descriptor->module;
                const bool visible = value->panel->isVisible();
                active.push_back(std::move(value));
                command.publisher.setPhase(EEditorToolPhase::ACTIVE);
                finish(command, true, visible);
                return;
            }

            if (index == active.size())
            {
                command.publisher.fail(EEditorToolError::NOT_ACTIVE);
                return;
            }
            if (command.kind == ECommandKind::SET_VISIBLE)
            {
                command.publisher.setPhase(
                    EEditorToolPhase::UPDATING_VISIBILITY);
                active[index]->panel->setVisible(command.visible);
                finish(command, true, command.visible);
                return;
            }

            if (!active[index]->supports_deactivation)
            {
                command.publisher.fail(
                    EEditorToolError::DEACTIVATION_NOT_SUPPORTED);
                return;
            }
            command.publisher.setPhase(EEditorToolPhase::DEACTIVATING);
            active.erase(active.begin() + static_cast<std::ptrdiff_t>(index));
            command.publisher.setPhase(EEditorToolPhase::INACTIVE);
            finish(command, false, false);
        }

        lux::ui::UISystem& ui;
        EditorPanelCatalog& catalog;
        EditorPanelCreateContext context;
        lux::events::DomainEvents* events{nullptr};
        std::shared_ptr<detail::EditorToolEndpoint> endpoint;
        std::vector<std::unique_ptr<Active>> active;
    };

    EditorToolHost::EditorToolHost(
        lux::ui::UISystem& ui,
        EditorPanelCatalog& catalog,
        EditorPanelCreateContext context,
        lux::events::DomainEvents* events,
        std::size_t queue_capacity)
        : impl_(std::make_unique<Impl>(
              ui,
              catalog,
              std::move(context),
              events,
              queue_capacity))
    {
        impl_->endpoint->host.store(this, std::memory_order_release);
    }

    EditorToolHost::~EditorToolHost() noexcept
    {
        (void)close();
    }

    EditorTools EditorToolHost::facade() const noexcept
    {
        return EditorTools{impl_->endpoint};
    }

    lux::cxx::expected<void, EEditorServiceRegistrationError>
    EditorToolHost::addServiceErased(
        lux::ecs::TypeToken type,
        void* service)
    {
        if (!impl_ || service == nullptr)
        {
            return lux::cxx::unexpected(
                EEditorServiceRegistrationError::INVALID_SERVICE);
        }
        return impl_->context.addErased(
            EditorPanelCreateContext::Entry{type, service, {}});
    }

    std::size_t EditorToolHost::processSafePoint(std::size_t budget) noexcept
    {
        std::size_t processed = 0u;
        EditorToolCommand command{
            ECommandKind::OPEN,
            lux::extensions::ContributionId{},
            false,
            0u};
        while (processed < budget && impl_->endpoint->queue.try_dequeue(command))
        {
            impl_->endpoint->queued.fetch_sub(1u, std::memory_order_release);
            impl_->process(command);
            ++processed;
        }
        return processed;
    }

    std::vector<EditorToolSnapshot> EditorToolHost::snapshot() const
    {
        std::vector<EditorToolSnapshot> values;
        values.reserve(impl_->catalog.all().size());
        for (const auto& descriptor : impl_->catalog.all())
        {
            const auto index = impl_->findActive(descriptor.id.view());
            const bool is_active = index != impl_->active.size();
            values.push_back(EditorToolSnapshot{
                descriptor.id,
                descriptor.display_name,
                descriptor.default_visible,
                is_active,
                is_active && impl_->active[index]->panel->isVisible()});
        }
        return values;
    }

    lux::ui::Panel* EditorToolHost::activePanel(
        lux::extensions::ContributionIdView id) noexcept
    {
        const auto index = impl_->findActive(id);
        return index == impl_->active.size()
            ? nullptr
            : impl_->active[index]->panel.get();
    }

    std::size_t EditorToolHost::close() noexcept
    {
        if (!impl_)
            return 0u;
        impl_->endpoint->admission_open.store(false, std::memory_order_release);
        impl_->endpoint->host.store(nullptr, std::memory_order_release);
        EditorToolCommand command{
            ECommandKind::OPEN,
            lux::extensions::ContributionId{},
            false,
            0u};
        while (impl_->endpoint->queue.try_dequeue(command))
        {
            impl_->endpoint->queued.fetch_sub(1u, std::memory_order_release);
            command.publisher.fail(EEditorToolError::STOPPING);
        }
        const auto removed = impl_->active.size();
        impl_->active.clear();
        return removed;
    }
}
