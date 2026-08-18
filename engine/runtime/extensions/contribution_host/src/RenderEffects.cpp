#include <lux/engine/runtime/extensions/RenderEffects.hpp>

#include <lux/engine/runtime/extensions/detail/RenderRequestSender.hpp>

#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

#include <lux/engine/ecs/SceneServices.hpp>
#include <lux/engine/ecs/render/systems/RenderSystem.hpp>
#include <lux/engine/events/DomainEvents.hpp>
#include <lux/engine/function/render/client/FeatureCatalog.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/runtime/extensions/SceneContributions.hpp>

#include <moodycamel/concurrentqueue.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <optional>

namespace lux::runtime
{
    namespace
    {
        using TicketPublisher = lux::extensions::OperationTicketPublisher<
            ERenderEffectActivationPhase,
            ERenderEffectActivationError,
            RenderEffectActivationResult>;

        [[nodiscard]] bool sameEffect(
            const lux::render::RenderEffectId& lhs,
            lux::render::RenderEffectIdView rhs) noexcept
        {
            return lhs.view() == rhs;
        }

        [[nodiscard]] bool sameFeatureType(
            lux::render::FeatureTypeId lhs,
            lux::render::FeatureTypeId rhs) noexcept
        {
            return lhs == lux::render::kInvalidFeatureTypeId ||
                rhs == lux::render::kInvalidFeatureTypeId || lhs == rhs;
        }
    }

    lux::cxx::expected<void, ERenderEffectCatalogError>
    RenderEffectCatalog::add(RenderEffectDescriptor descriptor)
    {
        std::vector<RenderEffectDescriptor> batch;
        batch.push_back(std::move(descriptor));
        return addBatch(std::move(batch));
    }

    lux::cxx::expected<void, ERenderEffectCatalogError>
    RenderEffectCatalog::validateBatch(
        std::span<const RenderEffectDescriptor> descriptors)
        const noexcept
    {
        for (std::size_t index = 0u; index < descriptors.size(); ++index)
        {
            const auto& descriptor = descriptors[index];
            if (!descriptor.id.isValid() || !descriptor.provider.isValid() ||
                !lux::render::isValidRenderEffectIdName(
                    descriptor.id.name()) ||
                !lux::extensions::isCanonicalStableName(
                    descriptor.provider.name()))
            {
                return lux::cxx::unexpected(
                    ERenderEffectCatalogError::INVALID_DESCRIPTOR);
            }
            if (!descriptor.factory.create_fn || !descriptor.factory.name ||
                descriptor.factory.name[0] == '\0')
            {
                return lux::cxx::unexpected(
                    ERenderEffectCatalogError::INVALID_FACTORY);
            }
            for (const auto& existing : descriptors_)
            {
                if (lux::render::renderEffectIdCollision(
                        existing.id.view(), descriptor.id.view()))
                    return lux::cxx::unexpected(
                        ERenderEffectCatalogError::ID_COLLISION);
                if (sameEffect(existing.id, descriptor.id.view()))
                    return lux::cxx::unexpected(
                        ERenderEffectCatalogError::DUPLICATE_EFFECT);
            }
            for (std::size_t other = 0u; other < index; ++other)
            {
                if (lux::render::renderEffectIdCollision(
                        descriptors[other].id.view(), descriptor.id.view()))
                    return lux::cxx::unexpected(
                        ERenderEffectCatalogError::ID_COLLISION);
                if (sameEffect(
                        descriptors[other].id,
                        descriptor.id.view()))
                    return lux::cxx::unexpected(
                        ERenderEffectCatalogError::DUPLICATE_EFFECT);
            }
        }
        return {};
    }

    lux::cxx::expected<void, ERenderEffectCatalogError>
    RenderEffectCatalog::addBatch(
        std::vector<RenderEffectDescriptor> descriptors)
    {
        if (auto checked = validateBatch(descriptors); !checked)
            return checked;
        descriptors_.reserve(descriptors_.size() + descriptors.size());
        for (auto& descriptor : descriptors)
            descriptors_.push_back(std::move(descriptor));
        return {};
    }

    RenderEffectDescriptor* RenderEffectCatalog::find(
        lux::render::RenderEffectIdView id) noexcept
    {
        for (auto& descriptor : descriptors_)
            if (sameEffect(descriptor.id, id))
                return &descriptor;
        return nullptr;
    }

    const RenderEffectDescriptor* RenderEffectCatalog::find(
        lux::render::RenderEffectIdView id) const noexcept
    {
        for (const auto& descriptor : descriptors_)
            if (sameEffect(descriptor.id, id))
                return &descriptor;
        return nullptr;
    }

    std::span<const RenderEffectDescriptor> RenderEffectCatalog::all()
        const noexcept
    {
        return descriptors_;
    }

    struct RenderEffectTypeRegistry::Impl final
    {
        struct Record final
        {
            lux::render::RenderEffectId effect;
            std::string feature_name;
            lux::render::FeatureTypeId stable_type{
                lux::render::kInvalidFeatureTypeId};
            ERenderEffectTypePhase phase{ERenderEffectTypePhase::UNSEEN};
            lux::render::RenderRequest<
                lux::render::FeatureTypeRegisteredReply> request;
            RenderEffectTypeSnapshot snapshot;
            lux::extensions::ModuleLease module;
        };

        explicit Impl(lux::render::RenderControlSession& value) noexcept
            : control(value)
        {}

        [[nodiscard]] Record* find(
            lux::render::RenderEffectIdView id) noexcept
        {
            for (auto& record : records)
                if (sameEffect(record.effect, id))
                    return &record;
            return nullptr;
        }

        void poll(Record& record) noexcept
        {
            if (record.phase != ERenderEffectTypePhase::REGISTERING ||
                !record.request.isReady())
                return;
            if (record.request.failed())
            {
                record.phase = ERenderEffectTypePhase::FAILED;
                record.snapshot.phase = record.phase;
                record.snapshot.error = record.request.error();
                return;
            }
            const auto reply = record.request.tryResult();
            if (!reply || reply->get().feature_type_id == 0u ||
                !reply->get().error.ok())
            {
                record.phase = ERenderEffectTypePhase::FAILED;
                record.snapshot.phase = record.phase;
                record.snapshot.error = reply
                    ? reply->get().error
                    : record.request.error();
                return;
            }
            const auto& value = reply->get();
            record.phase = ERenderEffectTypePhase::READY;
            record.snapshot.phase = record.phase;
            record.snapshot.type_id = value.feature_type_id;
            record.snapshot.op_count = std::min(value.op_count, 16u);
            std::copy_n(
                value.ops,
                record.snapshot.op_count,
                record.snapshot.ops);
        }

        lux::render::RenderControlSession& control;
        std::vector<Record> records;
    };

    RenderEffectTypeRegistry::RenderEffectTypeRegistry(lux::render::RenderControlSession& control)
        : impl_(std::make_unique<Impl>(control))
    {}

    RenderEffectTypeRegistry::~RenderEffectTypeRegistry() = default;

    RenderEffectTypeSnapshot RenderEffectTypeRegistry::ensure(
        const RenderEffectDescriptor& descriptor) noexcept
    {
        if (auto* record = impl_->find(descriptor.id.view()))
        {
            if (record->feature_name != descriptor.factory.name ||
                !sameFeatureType(
                    record->stable_type,
                    descriptor.factory.descriptor.type))
            {
                auto failed = record->snapshot;
                failed.phase = ERenderEffectTypePhase::FAILED;
                return failed;
            }
            impl_->poll(*record);
            return record->snapshot;
        }

        Impl::Record record;
        record.effect = descriptor.id;
        record.feature_name = descriptor.factory.name;
        record.stable_type = descriptor.factory.descriptor.type;
        record.phase = ERenderEffectTypePhase::REGISTERING;
        record.snapshot.phase = record.phase;
        record.module = descriptor.module;
        record.request = impl_->control.registerFeatureType(
            descriptor.factory,
            descriptor.module);
        impl_->records.push_back(std::move(record));
        impl_->poll(impl_->records.back());
        return impl_->records.back().snapshot;
    }

    void RenderEffectTypeRegistry::clear() noexcept
    {
        for (auto& record : impl_->records)
            record.request.cancel();
        impl_->records.clear();
    }

    namespace detail
    {
        enum class ERenderEffectCommandKind : std::uint8_t
        {
            ENABLE,
            DISABLE
        };

        struct RenderEffectCommand final
        {
            RenderEffectCommand(
                ERenderEffectCommandKind value_kind,
                lux::render::RenderEffectId value_id,
                ContributionConfig value_config,
                EActivationPersistence value_persistence,
                std::uint64_t value_generation)
                : kind(value_kind)
                , id(std::move(value_id))
                , config(std::move(value_config))
                , persistence(value_persistence)
                , accounted_bytes(config.bytes.size())
                , publisher(
                      ERenderEffectActivationPhase::QUEUED,
                      value_generation)
            {}

            ERenderEffectCommandKind kind{
                ERenderEffectCommandKind::ENABLE};
            lux::render::RenderEffectId id;
            ContributionConfig config;
            EActivationPersistence persistence{
                EActivationPersistence::SCENE};
            std::size_t accounted_bytes{0u};
            TicketPublisher publisher;
        };

        struct RenderEffectEndpoint final
        {
            explicit RenderEffectEndpoint(RenderEffectQueueConfig value)
                : config(value)
                , queue(value.capacity)
            {}

            [[nodiscard]] RenderEffectOperationTicket submit(
                ERenderEffectCommandKind kind,
                lux::render::RenderEffectIdView id,
                ContributionConfig config_value,
                EActivationPersistence persistence)
            {
                const auto generation = next_generation.fetch_add(
                    1u,
                    std::memory_order_relaxed);
                RenderEffectCommand command{
                    kind,
                    lux::render::RenderEffectId{id.name()},
                    std::move(config_value),
                    persistence,
                    generation};
                auto ticket = command.publisher.ticket();
                if (!admission_open.load(std::memory_order_acquire))
                {
                    command.publisher.fail(
                        ERenderEffectActivationError::STOPPING);
                    return ticket;
                }
                if (!id.isValid())
                {
                    command.publisher.fail(
                        ERenderEffectActivationError::UNKNOWN_EFFECT);
                    return ticket;
                }

                auto current = bytes_inflight.load(std::memory_order_relaxed);
                for (;;)
                {
                    const auto available = config.byte_budget -
                        std::min(current, config.byte_budget);
                    if (command.accounted_bytes > available)
                    {
                        command.publisher.fail(
                            ERenderEffectActivationError::
                                BYTE_BUDGET_EXHAUSTED);
                        return ticket;
                    }
                    if (bytes_inflight.compare_exchange_weak(
                            current,
                            current + command.accounted_bytes,
                            std::memory_order_acq_rel,
                            std::memory_order_relaxed))
                        break;
                }
                const auto accounted = command.accounted_bytes;
                auto terminal = command.publisher;
                if (!queue.try_enqueue(std::move(command)))
                {
                    bytes_inflight.fetch_sub(
                        accounted,
                        std::memory_order_release);
                    terminal.fail(ERenderEffectActivationError::QUEUE_FULL);
                }
                return ticket;
            }

            RenderEffectQueueConfig config;
            moodycamel::ConcurrentQueue<RenderEffectCommand> queue;
            std::atomic<std::size_t> bytes_inflight{0u};
            std::atomic<std::uint64_t> next_generation{1u};
            std::atomic<bool> admission_open{true};
        };
    }

    RenderEffects::RenderEffects(
        std::shared_ptr<detail::RenderEffectEndpoint> endpoint) noexcept
        : endpoint_(std::move(endpoint))
    {}

    RenderEffectOperationTicket RenderEffects::requestEnable(
        lux::render::RenderEffectIdView id,
        ContributionConfig config,
        EActivationPersistence persistence) const
    {
        if (endpoint_)
        {
            return endpoint_->submit(
                detail::ERenderEffectCommandKind::ENABLE,
                id,
                std::move(config),
                persistence);
        }
        TicketPublisher publisher(
            ERenderEffectActivationPhase::QUEUED,
            0u);
        auto ticket = publisher.ticket();
        publisher.fail(ERenderEffectActivationError::STOPPING);
        return ticket;
    }

    RenderEffectOperationTicket RenderEffects::requestDisable(
        lux::render::RenderEffectIdView id) const
    {
        if (endpoint_)
        {
            return endpoint_->submit(
                detail::ERenderEffectCommandKind::DISABLE,
                id,
                {},
                EActivationPersistence::TRANSIENT);
        }
        TicketPublisher publisher(
            ERenderEffectActivationPhase::QUEUED,
            0u);
        auto ticket = publisher.ticket();
        publisher.fail(ERenderEffectActivationError::STOPPING);
        return ticket;
    }

    RenderEffects::operator bool() const noexcept
    {
        return endpoint_ &&
            endpoint_->admission_open.load(std::memory_order_acquire);
    }

    struct RenderEffectHost::Impl final
    {
        struct Active final
        {
            lux::render::RenderEffectId id;
            ContributionConfig config;
            EActivationPersistence persistence{
                EActivationPersistence::SCENE};
            lux::render::FeatureHandle feature{};
            lux::ecs::InstalledRenderSubsystemBatch extraction;
            std::uint64_t generation{0u};
            lux::extensions::ModuleLease module;
        };

        enum class EPendingStep : std::uint8_t
        {
            BEGIN,
            WAITING_TYPE,
            WAITING_ADD,
            WAITING_COMPENSATION,
            WAITING_REMOVE
        };

        struct Pending final
        {
            explicit Pending(detail::RenderEffectCommand value)
                : command(std::move(value))
            {}

            detail::RenderEffectCommand command;
            EPendingStep step{EPendingStep::BEGIN};
            std::size_t active_index{0u};
            lux::render::FeatureHandle feature{};
            ERenderEffectActivationError deferred_error{
                ERenderEffectActivationError::NONE};
            lux::render::RenderRequest<lux::render::FeatureAddedReply>
                add_request;
            lux::render::RenderRequest<lux::render::GenericOkReply>
                remove_request;
        };

        Impl(
            lux::ecs::RenderSystem& value_render_system,
            lux::ecs::SceneServices& value_services,
            lux::render::RenderSceneId value_scene,
            lux::render::RenderControlSession& value_control,
            lux::render::FeatureCatalog& value_feature_catalog,
            RenderEffectCatalog& value_catalog,
            RenderEffectTypeRegistry& value_type_registry,
            const SceneContributionHost* value_scene_contributions,
            lux::events::DomainEvents* value_events,
            lux::cxx::move_only_function<void()> value_progress,
            RenderEffectQueueConfig queue)
            : render_system(value_render_system)
            , services(value_services)
            , scene(value_scene)
            , control(value_control)
            , feature_catalog(value_feature_catalog)
            , catalog(value_catalog)
            , type_registry(value_type_registry)
            , scene_contributions(value_scene_contributions)
            , events(value_events)
            , progress(std::move(value_progress))
            , endpoint(std::make_shared<detail::RenderEffectEndpoint>(queue))
        {}

        template <class Reply>
        void observe(lux::render::RenderRequest<Reply>& request)
        {
            if (request.isReady())
                return;

            const auto notify = [this]() noexcept
            {
                if (pending)
                    (void)advancePending();
                if (progress)
                    progress();
            };
            auto completion = render_detail::asSender(request)
                | stdexec::then(
                      [notify](Reply) noexcept
                      {
                          notify();
                      })
                | stdexec::upon_error(
                      [notify](lux::render::RenderError) noexcept
                      {
                          notify();
                      });
            ::experimental::execution::start_detached(
                std::move(completion));
        }

        [[nodiscard]] std::size_t findActive(
            lux::render::RenderEffectIdView id) const noexcept
        {
            for (std::size_t index = 0u; index < active.size(); ++index)
                if (sameEffect(active[index].id, id))
                    return index;
            return active.size();
        }

        void publishFact(const Active& value, bool is_active)
        {
            if (events)
            {
                events->publish(RenderEffectStateChanged{
                    value.id,
                    value.generation,
                    is_active});
            }
        }

        void failPending(ERenderEffectActivationError error) noexcept
        {
            pending->command.publisher.fail(error);
            pending.reset();
        }

        void succeedPending(bool is_active)
        {
            auto id = pending->command.id;
            const auto generation = pending->command.publisher.ticket()
                .snapshot().generation;
            pending->command.publisher.succeed(RenderEffectActivationResult{
                std::move(id),
                generation,
                is_active});
            pending.reset();
        }

        [[nodiscard]] bool adoptFeatureType(
            const RenderEffectDescriptor& descriptor,
            const RenderEffectTypeSnapshot& registration)
        {
            const auto current = feature_catalog.typeId(
                descriptor.factory.name);
            if (current != 0u)
            {
                const auto* current_descriptor = feature_catalog.descriptor(
                    descriptor.factory.name);
                return current == registration.type_id &&
                    (!current_descriptor || sameFeatureType(
                        current_descriptor->type,
                        descriptor.factory.descriptor.type));
            }
            feature_catalog.add(
                descriptor.factory,
                registration.type_id,
                std::span<const lux::render::TypeId>{
                    registration.ops,
                    registration.op_count});
            return feature_catalog.typeId(descriptor.factory.name) ==
                registration.type_id;
        }

        [[nodiscard]] bool dependenciesReady(
            const RenderEffectDescriptor& descriptor) const
            noexcept
        {
            if (descriptor.required_scene_features.empty())
                return true;
            if (!scene_contributions)
                return false;
            return std::ranges::all_of(
                descriptor.required_scene_features,
                [this](const auto& dependency) noexcept
                {
                    return scene_contributions->active(dependency.view());
                });
        }

        [[nodiscard]] bool advanceEnable()
        {
            auto* descriptor = catalog.find(pending->command.id.view());
            if (!descriptor)
            {
                failPending(
                    ERenderEffectActivationError::UNKNOWN_EFFECT);
                return true;
            }

            if (pending->step == EPendingStep::BEGIN)
            {
                if (findActive(descriptor->id.view()) != active.size())
                {
                    succeedPending(true);
                    return true;
                }
                auto& config = pending->command.config;
                if (config.schema_version == 0u && config.bytes.empty())
                    config = descriptor->default_config;
                if (config.schema_version != descriptor->config_schema_version)
                {
                    failPending(
                        ERenderEffectActivationError::
                            CONFIG_VERSION_MISMATCH);
                    return true;
                }
                if (!dependenciesReady(*descriptor))
                {
                    failPending(
                        ERenderEffectActivationError::
                            MISSING_SCENE_FEATURE);
                    return true;
                }
                pending->command.publisher.setPhase(
                    ERenderEffectActivationPhase::REGISTERING_TYPE);
                pending->step = EPendingStep::WAITING_TYPE;
            }

            if (pending->step == EPendingStep::WAITING_TYPE)
            {
                const auto registration = type_registry.ensure(*descriptor);
                if (registration.phase ==
                    ERenderEffectTypePhase::REGISTERING)
                    return false;
                if (registration.phase != ERenderEffectTypePhase::READY)
                {
                    failPending(
                        ERenderEffectActivationError::
                            FEATURE_TYPE_REGISTRATION_FAILED);
                    return true;
                }
                if (!adoptFeatureType(*descriptor, registration))
                {
                    failPending(
                        ERenderEffectActivationError::
                            FEATURE_CATALOG_CONFLICT);
                    return true;
                }
                pending->command.publisher.setPhase(
                    ERenderEffectActivationPhase::ADDING_RENDER_INSTANCE);
                pending->add_request = control.addFeatureRaw(
                    scene,
                    registration.type_id,
                    pending->command.config.bytes);
                pending->step = EPendingStep::WAITING_ADD;
                observe(pending->add_request);
                return false;
            }

            if (pending->step == EPendingStep::WAITING_ADD)
            {
                if (!pending->add_request.isReady())
                    return false;
                if (pending->add_request.failed())
                {
                    failPending(
                        ERenderEffectActivationError::ADD_FEATURE_FAILED);
                    return true;
                }
                const auto reply = pending->add_request.tryResult();
                if (!reply || !reply->get().feature.isValid() ||
                    !reply->get().error.ok())
                {
                    failPending(
                        ERenderEffectActivationError::ADD_FEATURE_FAILED);
                    return true;
                }
                pending->feature = reply->get().feature;
                render_system.bindFeature(
                    descriptor->factory.name,
                    pending->feature);
                pending->command.publisher.setPhase(
                    ERenderEffectActivationPhase::INSTALLING_EXTRACTION);

                lux::ecs::InstalledRenderSubsystemBatch installed;
                if (descriptor->build_extraction)
                {
                    lux::ecs::RenderSubsystemMutationBatch batch;
                    const auto built = descriptor->build_extraction(
                        batch,
                        RenderEffectBuildContext{services},
                        pending->command.config);
                    if (!built)
                    {
                        pending->deferred_error =
                            ERenderEffectActivationError::
                                EXTRACTION_BUILD_FAILED;
                    }
                    else if (!batch.empty())
                    {
                        auto result = render_system.installSubsystemBatch(
                            std::move(batch));
                        if (result)
                            installed = std::move(*result);
                        else
                            pending->deferred_error =
                                ERenderEffectActivationError::
                                    EXTRACTION_INSTALL_FAILED;
                    }
                }

                if (pending->deferred_error !=
                    ERenderEffectActivationError::NONE)
                {
                    render_system.unbindFeature(
                        descriptor->factory.name,
                        pending->feature);
                    pending->command.publisher.setPhase(
                        ERenderEffectActivationPhase::COMPENSATING);
                    pending->remove_request = control.removeFeature(
                        scene,
                        pending->feature);
                    pending->step = EPendingStep::WAITING_COMPENSATION;
                    observe(pending->remove_request);
                    return false;
                }

                Active value;
                value.id = descriptor->id;
                value.config = pending->command.config;
                value.persistence = pending->command.persistence;
                value.feature = pending->feature;
                value.extraction = std::move(installed);
                value.generation = pending->command.publisher.ticket()
                    .snapshot().generation;
                value.module = descriptor->module;
                active.push_back(std::move(value));
                publishFact(active.back(), true);
                pending->command.publisher.setPhase(
                    ERenderEffectActivationPhase::ACTIVE);
                succeedPending(true);
                return true;
            }

            if (pending->step == EPendingStep::WAITING_COMPENSATION)
            {
                if (!pending->remove_request.isReady())
                    return false;
                const auto removed = pending->remove_request.tryResult();
                if (pending->remove_request.failed() || !removed ||
                    removed->get().code != 0u)
                {
                    Active orphan;
                    orphan.id = descriptor->id;
                    orphan.config = pending->command.config;
                    orphan.persistence = pending->command.persistence;
                    orphan.feature = pending->feature;
                    orphan.generation = pending->command.publisher.ticket()
                        .snapshot().generation;
                    orphan.module = descriptor->module;
                    active.push_back(std::move(orphan));
                    failPending(
                        ERenderEffectActivationError::COMPENSATION_FAILED);
                    return true;
                }
                failPending(pending->deferred_error);
                return true;
            }
            return false;
        }

        [[nodiscard]] bool advanceDisable()
        {
            if (pending->step == EPendingStep::BEGIN)
            {
                pending->active_index = findActive(
                    pending->command.id.view());
                if (pending->active_index == active.size())
                {
                    failPending(ERenderEffectActivationError::NOT_ACTIVE);
                    return true;
                }
                auto& value = active[pending->active_index];
                auto* descriptor = catalog.find(value.id.view());
                if (!descriptor)
                {
                    failPending(
                        ERenderEffectActivationError::UNKNOWN_EFFECT);
                    return true;
                }
                pending->command.publisher.setPhase(
                    ERenderEffectActivationPhase::REMOVING_EXTRACTION);
                if (value.extraction.valid())
                {
                    auto removed = render_system.removeSubsystemBatch(
                        std::move(value.extraction));
                    if (!removed)
                    {
                        failPending(
                            ERenderEffectActivationError::
                                EXTRACTION_REMOVE_FAILED);
                        return true;
                    }
                }
                render_system.unbindFeature(
                    descriptor->factory.name,
                    value.feature);
                pending->command.publisher.setPhase(
                    ERenderEffectActivationPhase::REMOVING_RENDER_INSTANCE);
                pending->remove_request = control.removeFeature(
                    scene,
                    value.feature);
                pending->step = EPendingStep::WAITING_REMOVE;
                observe(pending->remove_request);
                return false;
            }

            if (pending->step == EPendingStep::WAITING_REMOVE)
            {
                if (!pending->remove_request.isReady())
                    return false;
                const auto removed = pending->remove_request.tryResult();
                if (pending->remove_request.failed() || !removed ||
                    removed->get().code != 0u)
                {
                    failPending(
                        ERenderEffectActivationError::REMOVE_FEATURE_FAILED);
                    return true;
                }
                auto value = std::move(active[pending->active_index]);
                active.erase(
                    active.begin() + static_cast<std::ptrdiff_t>(
                        pending->active_index));
                publishFact(value, false);
                pending->command.publisher.setPhase(
                    ERenderEffectActivationPhase::INACTIVE);
                succeedPending(false);
                return true;
            }
            return false;
        }

        [[nodiscard]] bool advancePending()
        {
            return pending->command.kind ==
                    detail::ERenderEffectCommandKind::ENABLE
                ? advanceEnable()
                : advanceDisable();
        }

        lux::ecs::RenderSystem& render_system;
        lux::ecs::SceneServices& services;
        lux::render::RenderSceneId scene;
        lux::render::RenderControlSession& control;
        lux::render::FeatureCatalog& feature_catalog;
        RenderEffectCatalog& catalog;
        RenderEffectTypeRegistry& type_registry;
        const SceneContributionHost* scene_contributions{nullptr};
        lux::events::DomainEvents* events{nullptr};
        lux::cxx::move_only_function<void()> progress;
        std::shared_ptr<detail::RenderEffectEndpoint> endpoint;
        std::vector<Active> active;
        std::optional<Pending> pending;
        std::size_t close_failures{0u};
    };

    RenderEffectHost::RenderEffectHost(
        lux::ecs::RenderSystem& render_system,
        lux::ecs::SceneServices& services,
        lux::render::RenderSceneId scene,
        lux::render::RenderControlSession& control,
        lux::render::FeatureCatalog& feature_catalog,
        RenderEffectCatalog& catalog,
        RenderEffectTypeRegistry& type_registry,
        const SceneContributionHost* scene_contributions,
        lux::events::DomainEvents* events,
        lux::cxx::move_only_function<void()> progress,
        RenderEffectQueueConfig queue)
        : impl_(std::make_unique<Impl>(
              render_system,
              services,
              scene,
              control,
              feature_catalog,
              catalog,
              type_registry,
              scene_contributions,
              events,
              std::move(progress),
              queue))
    {}

    RenderEffectHost::~RenderEffectHost() noexcept
    {
        impl_->endpoint->admission_open.store(
            false,
            std::memory_order_release);
    }

    RenderEffects RenderEffectHost::facade() const noexcept
    {
        return RenderEffects{impl_->endpoint};
    }

    std::size_t RenderEffectHost::processSafePoint(std::size_t budget) noexcept
    {
        std::size_t processed = 0u;
        while (processed < budget)
        {
            if (impl_->pending)
            {
                if (!impl_->advancePending())
                    break;
                ++processed;
                continue;
            }
            detail::RenderEffectCommand command{
                detail::ERenderEffectCommandKind::DISABLE,
                {},
                {},
                EActivationPersistence::TRANSIENT,
                0u};
            if (!impl_->endpoint->queue.try_dequeue(command))
                break;
            impl_->endpoint->bytes_inflight.fetch_sub(
                command.accounted_bytes,
                std::memory_order_release);
            impl_->pending.emplace(std::move(command));
        }
        return processed;
    }

    bool RenderEffectHost::active(
        lux::render::RenderEffectIdView id) const noexcept
    {
        return impl_->findActive(id) != impl_->active.size();
    }

    std::vector<RenderEffectActivationSnapshot>
    RenderEffectHost::activationSnapshot() const
    {
        std::vector<RenderEffectActivationSnapshot> result;
        result.reserve(impl_->active.size());
        for (const auto& active : impl_->active)
        {
            const auto* descriptor = impl_->catalog.find(active.id.view());
            if (!descriptor)
                continue;
            result.push_back(RenderEffectActivationSnapshot{
                active.id,
                active.config,
                active.persistence,
                descriptor->provider,
                active.generation});
        }
        return result;
    }

    RenderEffectCloseReport RenderEffectHost::close() noexcept
    {
        RenderEffectCloseReport report;
        impl_->endpoint->admission_open.store(
            false,
            std::memory_order_release);

        detail::RenderEffectCommand queued{
            detail::ERenderEffectCommandKind::DISABLE,
            {},
            {},
            EActivationPersistence::TRANSIENT,
            0u};
        while (impl_->endpoint->queue.try_dequeue(queued))
        {
            impl_->endpoint->bytes_inflight.fetch_sub(
                queued.accounted_bytes,
                std::memory_order_release);
            queued.publisher.fail(ERenderEffectActivationError::STOPPING);
            ++report.rejected_queued;
        }

        if (impl_->pending &&
            impl_->pending->command.kind ==
                detail::ERenderEffectCommandKind::ENABLE &&
            (impl_->pending->step == Impl::EPendingStep::BEGIN ||
             impl_->pending->step == Impl::EPendingStep::WAITING_TYPE))
        {
            impl_->failPending(ERenderEffectActivationError::STOPPING);
        }

        if (impl_->pending)
        {
            const auto before = impl_->active.size();
            (void)processSafePoint(1u);
            report.removed += before - impl_->active.size();
        }
        if (!impl_->pending && !impl_->active.empty())
        {
            const auto& value = impl_->active.back();
            detail::RenderEffectCommand command{
                detail::ERenderEffectCommandKind::DISABLE,
                value.id,
                {},
                EActivationPersistence::TRANSIENT,
                value.generation};
            impl_->pending.emplace(std::move(command));
            const auto before = impl_->active.size();
            (void)processSafePoint(1u);
            report.removed += before - impl_->active.size();
        }
        report.failed = impl_->close_failures;
        report.pending = impl_->active.size() +
            static_cast<std::size_t>(impl_->pending.has_value());
        return report;
    }

}
