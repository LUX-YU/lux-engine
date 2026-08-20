#include <lux/engine/runtime/entity_scene/EntitySectionLoaderSystem.hpp>

#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/scene_format/Identifiers.hpp>
#include <lux/engine/scene/SceneAssetSerDeser.hpp>
#include <lux/engine/runtime/entity_scene/EntityBatchMaterializer.hpp>
#include <lux/engine/runtime/entity_scene/EntityBatchStager.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>

#include <stdexec/execution.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

namespace lux::runtime::entity_scene
{
    namespace ex = stdexec;

    detail::EntitySectionOwnerControl::EntitySectionOwnerControl(
        EntitySectionLoaderSystem* value) noexcept
        : owner(value), owner_thread(std::this_thread::get_id())
    {}

    namespace
    {
        void requireOwnerThread(
            const detail::EntitySectionOwnerControl& control) noexcept
        {
            if (std::this_thread::get_id() != control.owner_thread)
                std::abort();
        }

        [[nodiscard]] std::uint64_t allocateSectionGeneration() noexcept
        {
            // Generation identity survives loader/Scene reconstruction. Zero
            // is reserved; wrapping the process-wide sequence is an engine
            // invariant failure rather than an ABA-prone restart at one.
            static std::atomic<std::uint64_t> next{1u};
            const auto generation = next.fetch_add(
                1u, std::memory_order_relaxed);
            if (generation == 0u)
                std::abort();
            return generation;
        }

        [[nodiscard]] bool checkedAdd(
            std::size_t& total,
            std::uint64_t value) noexcept
        {
            if (value > std::numeric_limits<std::size_t>::max() ||
                total > std::numeric_limits<std::size_t>::max() -
                    static_cast<std::size_t>(value))
            {
                return false;
            }
            total += static_cast<std::size_t>(value);
            return true;
        }

        template<class Value>
        [[nodiscard]] bool checkedArrayBytes(
            std::size_t& total,
            std::size_t count) noexcept
        {
            if (count > std::numeric_limits<std::size_t>::max() /
                    sizeof(Value))
            {
                return false;
            }
            return checkedAdd(total, count * sizeof(Value));
        }

        [[nodiscard]] std::optional<std::size_t> accountedLoadBytes(
            const lux::scene::SectionRecord& record) noexcept
        {
            std::size_t result = sizeof(LoadEntitySection);
            switch (record.compression)
            {
            case lux::scene::SectionCompression::None:
                if (record.encoded_bytes != record.decoded_bytes ||
                    !checkedAdd(result, record.encoded_bytes))
                {
                    return std::nullopt;
                }
                break;
            case lux::scene::SectionCompression::Zstd:
                if (!checkedAdd(result, record.encoded_bytes) ||
                    !checkedAdd(result, record.decoded_bytes))
                {
                    return std::nullopt;
                }
                break;
            default:
                return std::nullopt;
            }

            if (const auto* stored = std::get_if<
                    lux::scene::StoredSectionSource>(&record.source))
            {
                if (!checkedAdd(result, stored->content_path.size()))
                    return std::nullopt;
            }
            else if (const auto* generated = std::get_if<
                         lux::scene::GeneratedSectionSource>(
                         &record.source))
            {
                if (!checkedAdd(result, generated->generator.name().size()) ||
                    !checkedAdd(result, generated->parameters.size()))
                {
                    return std::nullopt;
                }
            }
            if (!checkedArrayBytes<lux::ecs::scene_format::EntitySectionId>(
                    result, record.dependencies.size()) ||
                !checkedArrayBytes<lux::scene::DemandChannelId>(
                    result, record.demand_channels.size()) ||
                !checkedArrayBytes<lux::scene::RequiredExtension>(
                    result, record.required_extensions.size()) ||
                !checkedArrayBytes<
                    lux::scene::RequiredComponentSchema>(
                    result, record.required_components.size()))
            {
                return std::nullopt;
            }
            for (const auto& channel : record.demand_channels)
                if (!checkedAdd(result, channel.name().size()))
                    return std::nullopt;
            for (const auto& extension : record.required_extensions)
                if (!checkedAdd(result, extension.id.name().size()))
                    return std::nullopt;
            for (const auto& component : record.required_components)
                if (!checkedAdd(result, component.id.name.size()))
                    return std::nullopt;
            return result;
        }
    }

    struct EntitySectionLoaderSystem::Impl final
    {
        struct DependencyPin final
        {
            std::uint32_t slot{~std::uint32_t{0u}};
            std::uint64_t generation{0u};
        };

        struct Slot final
        {
            lux::scene::SectionRecord record;
            std::optional<PreparedEntityBatch> prepared;
            std::optional<EntityBatchFailure> batch_failure;
            std::optional<EEntitySectionLoadError> load_failure;
            EEntitySectionState state{EEntitySectionState::EMPTY};
            std::uint64_t generation{0u};
            std::size_t references{0u};
            std::size_t external_references{0u};
            std::vector<DependencyPin> dependencies;
            bool launch_failed{false};
        };

        Impl(
            EntitySectionLoaderSystem& owner_value,
            lux::exec::AsyncRuntime& runtime,
            EntitySectionLoadClient loading_value,
            std::shared_ptr<const lux::asset::AssetVfs> vfs_value,
            const lux::ecs::ComponentTypeCatalog& components,
            lux::ecs::PersistentEntityIndex& persistent_entities,
            EntitySectionLoaderConfig config_value)
            : runtime_owner(&runtime),
              scope(runtime),
              close_barrier_admission(scope.tryAcquireAdmission()),
              loading(std::move(loading_value)),
              vfs(std::move(vfs_value)),
              stager(components),
              materializer(persistent_entities),
              config(config_value),
              components(&components),
              persistent_entities(&persistent_entities),
              control(std::make_shared<detail::EntitySectionOwnerControl>(
                  &owner_value))
        {
            if (!close_barrier_admission)
                std::abort();
            owner_thread = control->owner_thread;
        }

        [[nodiscard]] bool validSlot(
            std::uint32_t index,
            std::uint64_t generation) const noexcept
        {
            return index < slots.size() && generation != 0u &&
                slots[index].generation == generation;
        }

        enum class EDependencyState : std::uint8_t
        {
            READY,
            WAITING,
            FAILED
        };

        [[nodiscard]] EDependencyState dependencyState(
            const Slot& slot) const noexcept
        {
            auto result = EDependencyState::READY;
            for (const auto& dependency : slot.dependencies)
            {
                if (!validSlot(dependency.slot, dependency.generation))
                    return EDependencyState::FAILED;
                switch (slots[dependency.slot].state)
                {
                case EEntitySectionState::ACTIVE:
                    break;
                case EEntitySectionState::FAILED:
                case EEntitySectionState::EMPTY:
                case EEntitySectionState::CANCELLED:
                    return EDependencyState::FAILED;
                default:
                    result = EDependencyState::WAITING;
                    break;
                }
            }
            return result;
        }

        [[nodiscard]] std::optional<EEntitySectionRequestError>
        validateRequirements(
            const lux::scene::SectionRecord& record) const
            noexcept
        {
            // Extension availability belongs to scene assembly. Until that
            // snapshot is injected here, a requirement cannot be proven.
            if (!record.required_extensions.empty())
            {
                return EEntitySectionRequestError::REQUIREMENT_UNAVAILABLE;
            }
            for (const auto& requirement : record.required_components)
            {
                if (!lux::ecs::isValidComponentSchemaId(requirement.id))
                {
                    return EEntitySectionRequestError::REQUIREMENT_UNAVAILABLE;
                }
                const auto* descriptor = components->findBySchema(
                    requirement.id.name);
                if (!descriptor ||
                    descriptor->schema_id.hash != requirement.id.hash ||
                    descriptor->schema_version != requirement.schema_version)
                {
                    return EEntitySectionRequestError::REQUIREMENT_UNAVAILABLE;
                }
            }
            return std::nullopt;
        }

        void acceptOutcome(
            std::uint32_t index,
            std::uint64_t generation,
            lux::exec::AsyncOutcome<LoadEntitySection> outcome) noexcept
        {
            if (std::this_thread::get_id() != owner_thread)
                std::abort();
            if (!validSlot(index, generation) ||
                slots[index].state !=
                    EEntitySectionState::WAITING_BACKGROUND)
            {
                ++stale_completions;
                return;
            }
            auto& slot = slots[index];
            if (!outcome)
            {
                if (outcome.error().isRuntime())
                {
                    const auto error = outcome.error().runtimeError();
                    if (error == lux::exec::EAsyncSubmitError::QUEUE_FULL ||
                        error == lux::exec::
                            EAsyncSubmitError::BYTE_BUDGET_EXHAUSTED)
                    {
                        ++queue_backpressure;
                        slot.state =
                            EEntitySectionState::WAITING_ADMISSION;
                        return;
                    }
                }
                else
                {
                    slot.load_failure = outcome.error().domainError();
                }
                slot.state = EEntitySectionState::FAILED;
                return;
            }
            if (outcome->request_generation != generation)
            {
                ++stale_completions;
                slot.state = EEntitySectionState::FAILED;
                return;
            }
            auto prepared = stager.begin(
                std::move(outcome->decoded), blobs);
            if (!prepared)
            {
                slot.batch_failure = std::move(prepared.error());
                slot.state = EEntitySectionState::FAILED;
                return;
            }
            slot.prepared.emplace(std::move(*prepared));
            slot.state = EEntitySectionState::STAGING;
        }

        void acceptStopped(
            std::uint32_t index,
            std::uint64_t generation) noexcept
        {
            if (std::this_thread::get_id() != owner_thread)
                std::abort();
            if (!validSlot(index, generation) ||
                slots[index].state !=
                    EEntitySectionState::WAITING_BACKGROUND)
            {
                ++stale_completions;
                return;
            }
            slots[index].state = EEntitySectionState::FAILED;
        }

        void launch(std::uint32_t index) noexcept
        {
            auto& slot = slots[index];
            if (slot.state != EEntitySectionState::WAITING_ADMISSION)
                std::abort();
            slot.state = EEntitySectionState::WAITING_BACKGROUND;
            slot.launch_failed = false;
            const auto generation = slot.generation;
            const auto accounted = accountedLoadBytes(slot.record);
            if (!accounted)
            {
                slot.state = EEntitySectionState::FAILED;
                return;
            }
            auto pipeline = lux::exec::execute(
                    loading.operation(),
                    loading.loadOperation(vfs, slot.record, generation),
                    lux::exec::AsyncSubmitOptions{
                        .accounted_bytes = *accounted})
                | ex::continues_on(
                      lux::exec::mainThreadScheduler(*runtime_owner))
                | ex::then(
                      [control = std::weak_ptr{control}, index, generation](
                          lux::exec::AsyncOutcome<LoadEntitySection> outcome)
                          mutable noexcept
                      {
                          const auto locked = control.lock();
                          if (!locked)
                              return;
                          requireOwnerThread(*locked);
                          auto* owner_value = locked->owner.load(
                              std::memory_order_acquire);
                          if (owner_value)
                          {
                              owner_value->impl_->acceptOutcome(
                                  index, generation, std::move(outcome));
                          }
                      })
                | ex::upon_stopped(
                      [control = std::weak_ptr{control}, index, generation]()
                          noexcept
                      {
                          const auto locked = control.lock();
                          if (!locked)
                              return;
                          requireOwnerThread(*locked);
                          auto* owner_value = locked->owner.load(
                              std::memory_order_acquire);
                          if (owner_value)
                          {
                              owner_value->impl_->acceptStopped(
                                  index, generation);
                          }
                      });
            if (!lux::exec::spawn(scope, std::move(pipeline)))
            {
                // update() adopts this on the owner thread; never complete a
                // ticket inline from the submitter's call stack.
                slot.launch_failed = true;
            }
        }

        [[nodiscard]] bool enqueue(
            std::uint32_t index,
            EEntitySectionCommandAction action) noexcept
        {
            const auto result = commands.push(EntitySectionCommand{
                index, slots[index].generation, action});
            if (!result)
            {
                ++command_rejections;
                return false;
            }
            return true;
        }

        void releaseLastReference(std::uint32_t index) noexcept
        {
            auto& slot = slots[index];
            if (slot.references != 0u)
                return;
            switch (slot.state)
            {
            case EEntitySectionState::ARMED:
                if (registry && slot.prepared)
                {
                    const auto cancelled = materializer.cancelArmed(
                        *slot.prepared, *registry);
                    if (!cancelled)
                        std::abort();
                }
                slot.prepared.reset();
                ++cancelled_requests;
                recycle(index);
                break;
            case EEntitySectionState::WAITING_ADMISSION:
            case EEntitySectionState::WAITING_BACKGROUND:
            case EEntitySectionState::STAGING:
            case EEntitySectionState::FAILED:
                slot.prepared.reset();
                ++cancelled_requests;
                recycle(index);
                break;
            case EEntitySectionState::ACTIVE:
                if (enqueue(index, EEntitySectionCommandAction::DEACTIVATE))
                    slot.state = EEntitySectionState::DEACTIVATE_QUEUED;
                break;
            default:
                break;
            }
        }

        void recycle(
            std::uint32_t index,
            bool release_dependencies = true) noexcept
        {
            auto& slot = slots[index];
            if (slot.state == EEntitySectionState::EMPTY)
                return;
            auto dependencies = std::move(slot.dependencies);
            const auto mapped = section_slots.find(slot.record.id.value());
            if (mapped != section_slots.end() && mapped->second == index)
                section_slots.erase(mapped);
            slot = Slot{};
            free_slots.push_back(index);
            if (!release_dependencies)
                return;
            for (const auto& dependency : dependencies)
            {
                if (!validSlot(dependency.slot, dependency.generation))
                    std::abort();
                auto& dependency_slot = slots[dependency.slot];
                if (dependency_slot.references == 0u)
                    std::abort();
                --dependency_slot.references;
                releaseLastReference(dependency.slot);
            }
        }

        void tryCompleteClose() noexcept
        {
            if (!control->closing.load(std::memory_order_acquire) ||
                !close_barrier_admission)
            {
                return;
            }
            const auto snapshot = materializer.snapshot();
            const auto blob_snapshot = blobs.snapshot();
            if (snapshot.active_sections == 0u &&
                snapshot.armed_sections == 0u &&
                section_slots.empty() &&
                blob_snapshot.current_bytes == 0u &&
                blob_snapshot.allocation_count == 0u)
            {
                close_barrier_admission = {};
            }
        }

        lux::exec::AsyncRuntime* runtime_owner{nullptr};
        lux::exec::AsyncScope scope;
        lux::exec::AsyncScope::AdmissionTicket close_barrier_admission;
        EntitySectionLoadClient loading;
        std::shared_ptr<const lux::asset::AssetVfs> vfs;
        EntityBatchStager stager;
        SectionBlobStore blobs;
        EntityBatchMaterializer materializer;
        EntitySectionLoaderConfig config;
        const lux::ecs::ComponentTypeCatalog* components{nullptr};
        lux::ecs::PersistentEntityIndex* persistent_entities{nullptr};
        std::shared_ptr<detail::EntitySectionOwnerControl> control;
        lux::meta::EntityRegistry* registry{nullptr};
        lux::ecs::EcsCommandWriter commands;
        std::vector<Slot> slots;
        std::map<uuids::uuid, std::uint32_t> section_slots;
        std::vector<std::uint32_t> free_slots;
        std::size_t staging_cursor{0u};
        std::thread::id owner_thread;
        std::uint64_t stale_completions{0u};
        std::uint64_t cancelled_requests{0u};
        std::uint64_t queue_backpressure{0u};
        std::uint64_t command_rejections{0u};
        bool scope_close_subscribed{false};
        bool scope_closed{false};
        lux::ecs::SystemCloseProgressSink close_progress;
    };

    EntitySectionTicket::EntitySectionTicket(
        std::weak_ptr<detail::EntitySectionOwnerControl> control,
        std::uint32_t slot,
        std::uint64_t generation) noexcept
        : control_(std::move(control)), slot_(slot), generation_(generation)
    {}

    EntitySectionTicket::~EntitySectionTicket()
    {
        reset();
    }

    EntitySectionTicket::EntitySectionTicket(
        EntitySectionTicket&& other) noexcept
        : control_(std::move(other.control_)),
          slot_(std::exchange(other.slot_, ~std::uint32_t{0u})),
          generation_(std::exchange(other.generation_, 0u))
    {
        if (const auto control = control_.lock())
            requireOwnerThread(*control);
    }

    EntitySectionTicket& EntitySectionTicket::operator=(
        EntitySectionTicket&& other) noexcept
    {
        if (this == &other)
            return *this;
        reset();
        control_ = std::move(other.control_);
        slot_ = std::exchange(other.slot_, ~std::uint32_t{0u});
        generation_ = std::exchange(other.generation_, 0u);
        if (const auto control = control_.lock())
            requireOwnerThread(*control);
        return *this;
    }

    void EntitySectionTicket::reset() noexcept
    {
        const auto control = control_.lock();
        if (control)
        {
            requireOwnerThread(*control);
            auto* owner = control->owner.load(std::memory_order_acquire);
            if (owner && generation_ != 0u)
                owner->release(slot_, generation_);
        }
        control_.reset();
        slot_ = ~std::uint32_t{0u};
        generation_ = 0u;
    }

    EntitySectionTicket::operator bool() const noexcept
    {
        const auto control = control_.lock();
        if (!control)
            return false;
        requireOwnerThread(*control);
        auto* owner = control->owner.load(std::memory_order_acquire);
        return owner && generation_ != 0u &&
            owner->state(slot_, generation_) !=
                EEntitySectionState::EMPTY;
    }

    EEntitySectionState EntitySectionTicket::state() const noexcept
    {
        const auto control = control_.lock();
        if (!control)
            return EEntitySectionState::EMPTY;
        requireOwnerThread(*control);
        auto* owner = control->owner.load(std::memory_order_acquire);
        return owner ? owner->state(slot_, generation_)
                     : EEntitySectionState::EMPTY;
    }

    lux::cxx::expected<EntitySectionTicket, EEntitySectionRequestError>
    EntitySectionClient::acquire(
        lux::scene::SectionRecord record) const noexcept
    {
        const auto control = control_.lock();
        if (!control)
        {
            return lux::cxx::unexpected(
                EEntitySectionRequestError::OWNER_CLOSED);
        }
        requireOwnerThread(*control);
        auto* owner = control->owner.load(std::memory_order_acquire);
        if (!owner ||
            control->closing.load(std::memory_order_acquire))
        {
            return lux::cxx::unexpected(
                EEntitySectionRequestError::OWNER_CLOSED);
        }
        return owner->acquire(std::move(record));
    }

    EntitySectionClient::operator bool() const noexcept
    {
        const auto control = control_.lock();
        if (!control)
            return false;
        requireOwnerThread(*control);
        return control->owner.load(std::memory_order_acquire) &&
            !control->closing.load(std::memory_order_acquire);
    }

    lux::cxx::expected<void, EEntitySectionRequestError>
    EntitySectionClient::validate(
        const lux::scene::SectionRecord& record) const noexcept
    {
        const auto control = control_.lock();
        if (!control)
        {
            return lux::cxx::unexpected(
                EEntitySectionRequestError::OWNER_CLOSED);
        }
        requireOwnerThread(*control);
        auto* owner = control->owner.load(std::memory_order_acquire);
        if (!owner || control->closing.load(std::memory_order_acquire))
        {
            return lux::cxx::unexpected(
                EEntitySectionRequestError::OWNER_CLOSED);
        }
        return owner->validate(record);
    }

    lux::cxx::expected<void, EEntitySectionRequestError>
    EntitySectionClient::validateRequirements(
        std::span<const lux::scene::RequiredExtension> extensions,
        std::span<const lux::scene::RequiredComponentSchema>
            components) const noexcept
    {
        const auto control = control_.lock();
        if (!control)
        {
            return lux::cxx::unexpected(
                EEntitySectionRequestError::OWNER_CLOSED);
        }
        requireOwnerThread(*control);
        auto* owner = control->owner.load(std::memory_order_acquire);
        if (!owner || control->closing.load(std::memory_order_acquire))
        {
            return lux::cxx::unexpected(
                EEntitySectionRequestError::OWNER_CLOSED);
        }
        return owner->validateRequirements(extensions, components);
    }

    bool EntitySectionClient::boundTo(
        const lux::meta::EntityRegistry& registry) const noexcept
    {
        const auto control = control_.lock();
        if (!control)
            return false;
        requireOwnerThread(*control);
        auto* owner = control->owner.load(std::memory_order_acquire);
        return owner &&
            !control->closing.load(std::memory_order_acquire) &&
            owner->impl_->registry == &registry;
    }

    bool EntitySectionClient::releaseSettled(
        const lux::ecs::scene_format::EntitySectionId& section,
        std::uint64_t generation) const noexcept
    {
        const auto control = control_.lock();
        if (!control)
            return true;
        requireOwnerThread(*control);
        const auto* owner = control->owner.load(std::memory_order_acquire);
        return !owner || owner->releaseSettled(section, generation);
    }

    EntitySectionLoaderSystem::EntitySectionLoaderSystem(
        lux::exec::AsyncRuntime& runtime,
        EntitySectionLoadClient loading,
        std::shared_ptr<const lux::asset::AssetVfs> vfs,
        const lux::ecs::ComponentTypeCatalog& components,
        lux::ecs::PersistentEntityIndex& persistent_entities,
        EntitySectionLoaderConfig config)
        : impl_(std::make_unique<Impl>(
              *this,
              runtime,
              std::move(loading),
              std::move(vfs),
              components,
              persistent_entities,
              config))
    {}

    EntitySectionLoaderSystem::~EntitySectionLoaderSystem()
    {
        requireOwnerThread(*impl_->control);
        impl_->control->closing.store(true, std::memory_order_release);
        impl_->control->owner.store(nullptr, std::memory_order_release);
        impl_->scope.requestStop();
        const auto materialized = impl_->materializer.snapshot();
        const auto blobs = impl_->blobs.snapshot();
        if (materialized.active_sections != 0u ||
            materialized.armed_sections != 0u ||
            !impl_->section_slots.empty() ||
            blobs.current_bytes != 0u ||
            blobs.allocation_count != 0u)
        {
            std::abort();
        }
    }

    EntitySectionClient EntitySectionLoaderSystem::client() const noexcept
    {
        requireOwnerThread(*impl_->control);
        return impl_->control->closing.load(std::memory_order_acquire)
            ? EntitySectionClient{}
            : EntitySectionClient{impl_->control};
    }

    ContentBlobClient EntitySectionLoaderSystem::contentBlobs() const noexcept
    {
        requireOwnerThread(*impl_->control);
        return impl_->blobs.client();
    }

    lux::cxx::expected<EntitySectionTicket, EEntitySectionRequestError>
    EntitySectionLoaderSystem::acquire(
        lux::scene::SectionRecord record) noexcept
    {
        requireOwnerThread(*impl_->control);
        if (impl_->control->closing.load(std::memory_order_acquire))
        {
            return lux::cxx::unexpected(
                EEntitySectionRequestError::OWNER_CLOSED);
        }
        if (!impl_->registry || !impl_->commands.valid())
        {
            return lux::cxx::unexpected(
                EEntitySectionRequestError::OWNER_NOT_ADDED);
        }
        if (!impl_->config.valid() || !impl_->loading ||
            !lux::scene::validateSectionRecord(record) ||
            (std::holds_alternative<
                 lux::scene::StoredSectionSource>(record.source) &&
             !impl_->vfs))
        {
            return lux::cxx::unexpected(
                EEntitySectionRequestError::INVALID_REQUEST);
        }
        if (!impl_->loading.supports(record))
        {
            return lux::cxx::unexpected(
                EEntitySectionRequestError::SOURCE_UNAVAILABLE);
        }
        const auto accounted = accountedLoadBytes(record);
        if (!accounted || *accounted > kEntitySectionLoadByteBudget)
        {
            return lux::cxx::unexpected(
                EEntitySectionRequestError::INVALID_REQUEST);
        }
        if (const auto requirement = impl_->validateRequirements(record))
            return lux::cxx::unexpected(*requirement);

        static_assert(std::is_nothrow_move_constructible_v<Impl::Slot>);
        static_assert(std::is_nothrow_move_assignable_v<Impl::Slot>);
        constexpr auto invalid_slot = ~std::uint32_t{0u};
        const auto section_key = record.id.value();
        std::uint32_t committed_slot = invalid_slot;
        std::vector<Impl::DependencyPin> dependency_pins;
            dependency_pins.reserve(record.dependencies.size());
            for (const auto& dependency : record.dependencies)
            {
                const auto mapped = impl_->section_slots.find(
                    dependency.value());
                if (mapped == impl_->section_slots.end())
                {
                    return lux::cxx::unexpected(
                        EEntitySectionRequestError::MISSING_DEPENDENCY);
                }
                auto& dependency_slot = impl_->slots[mapped->second];
                if (dependency_slot.state == EEntitySectionState::EMPTY ||
                    dependency_slot.state == EEntitySectionState::CANCELLED ||
                    dependency_slot.state == EEntitySectionState::FAILED ||
                    dependency_slot.references ==
                        std::numeric_limits<std::size_t>::max())
                {
                    return lux::cxx::unexpected(
                        EEntitySectionRequestError::MISSING_DEPENDENCY);
                }
                dependency_pins.push_back(Impl::DependencyPin{
                    mapped->second, dependency_slot.generation});
            }

            const auto found = impl_->section_slots.find(section_key);
            if (found != impl_->section_slots.end())
            {
                auto& slot = impl_->slots[found->second];
                if (slot.state == EEntitySectionState::EMPTY ||
                    slot.state == EEntitySectionState::CANCELLED)
                {
                    return lux::cxx::unexpected(
                        EEntitySectionRequestError::RECORD_CONFLICT);
                }
                if (slot.record != record)
                {
                    return lux::cxx::unexpected(
                        EEntitySectionRequestError::RECORD_CONFLICT);
                }
                if (slot.references ==
                        std::numeric_limits<std::size_t>::max() ||
                    slot.external_references ==
                        std::numeric_limits<std::size_t>::max())
                {
                    return lux::cxx::unexpected(
                        EEntitySectionRequestError::INVALID_REQUEST);
                }
                ++slot.references;
                ++slot.external_references;
                if (slot.state == EEntitySectionState::DEACTIVATE_QUEUED)
                    slot.state = EEntitySectionState::ACTIVE;
                return EntitySectionTicket{
                    impl_->control, found->second, slot.generation};
            }

            Impl::Slot candidate;
            candidate.generation = allocateSectionGeneration();
            candidate.record = std::move(record);
            candidate.references = 1u;
            candidate.external_references = 1u;
            candidate.dependencies = std::move(dependency_pins);
            candidate.state = EEntitySectionState::WAITING_ADMISSION;

            if (!impl_->free_slots.empty())
            {
                committed_slot = impl_->free_slots.back();
                const auto [mapped, inserted] = impl_->section_slots.emplace(
                    section_key, committed_slot);
                static_cast<void>(mapped);
                if (!inserted)
                {
                    return lux::cxx::unexpected(
                        EEntitySectionRequestError::RECORD_CONFLICT);
                }
                impl_->slots[committed_slot] = std::move(candidate);
                impl_->free_slots.pop_back();
            }
            else
            {
                if (impl_->slots.size() >= invalid_slot)
                {
                    return lux::cxx::unexpected(
                        EEntitySectionRequestError::INVALID_REQUEST);
                }
                committed_slot = static_cast<std::uint32_t>(
                    impl_->slots.size());
                impl_->slots.emplace_back(std::move(candidate));
                const auto [mapped, inserted] = impl_->section_slots.emplace(
                    section_key, committed_slot);
                static_cast<void>(mapped);
                if (!inserted)
                {
                    impl_->slots.pop_back();
                    return lux::cxx::unexpected(
                        EEntitySectionRequestError::RECORD_CONFLICT);
                }
            }

            auto& slot = impl_->slots[committed_slot];
            for (const auto& dependency : slot.dependencies)
            {
                auto& dependency_slot = impl_->slots[dependency.slot];
                ++dependency_slot.references;
                if (dependency_slot.state ==
                    EEntitySectionState::DEACTIVATE_QUEUED)
                {
                    dependency_slot.state = EEntitySectionState::ACTIVE;
                }
            }
            const auto generation = slot.generation;
            impl_->launch(committed_slot);
            return EntitySectionTicket{
                impl_->control, committed_slot, generation};
    }

    lux::cxx::expected<void, EEntitySectionRequestError>
    EntitySectionLoaderSystem::validate(
        const lux::scene::SectionRecord& record) const noexcept
    {
        requireOwnerThread(*impl_->control);
        if (impl_->control->closing.load(std::memory_order_acquire))
        {
            return lux::cxx::unexpected(
                EEntitySectionRequestError::OWNER_CLOSED);
        }
        if (!impl_->registry || !impl_->commands.valid())
        {
            return lux::cxx::unexpected(
                EEntitySectionRequestError::OWNER_NOT_ADDED);
        }
        if (!impl_->config.valid() || !impl_->loading ||
            !lux::scene::validateSectionRecord(record) ||
            (std::holds_alternative<
                 lux::scene::StoredSectionSource>(record.source) &&
             !impl_->vfs))
        {
            return lux::cxx::unexpected(
                EEntitySectionRequestError::INVALID_REQUEST);
        }
        if (!impl_->loading.supports(record))
        {
            return lux::cxx::unexpected(
                EEntitySectionRequestError::SOURCE_UNAVAILABLE);
        }
        const auto accounted = accountedLoadBytes(record);
        if (!accounted || *accounted > kEntitySectionLoadByteBudget)
        {
            return lux::cxx::unexpected(
                EEntitySectionRequestError::INVALID_REQUEST);
        }
        if (const auto requirement = impl_->validateRequirements(record))
            return lux::cxx::unexpected(*requirement);
        return {};
    }

    lux::cxx::expected<void, EEntitySectionRequestError>
    EntitySectionLoaderSystem::validateRequirements(
        std::span<const lux::scene::RequiredExtension> extensions,
        std::span<const lux::scene::RequiredComponentSchema>
            components) const noexcept
    {
        requireOwnerThread(*impl_->control);
        lux::scene::SectionRecord requirements;
        requirements.required_extensions.assign(
            extensions.begin(), extensions.end());
        requirements.required_components.assign(
            components.begin(), components.end());
        if (const auto failure = impl_->validateRequirements(requirements))
            return lux::cxx::unexpected(*failure);
        return {};
    }

    void EntitySectionLoaderSystem::release(
        std::uint32_t index,
        std::uint64_t generation) noexcept
    {
        requireOwnerThread(*impl_->control);
        if (!impl_->validSlot(index, generation))
            return;
        auto& slot = impl_->slots[index];
        if (slot.external_references == 0u || slot.references == 0u)
            return;
        --slot.external_references;
        --slot.references;
        if (slot.references != 0u)
            return;
        impl_->releaseLastReference(index);
        impl_->tryCompleteClose();
    }

    EEntitySectionState EntitySectionLoaderSystem::state(
        std::uint32_t index,
        std::uint64_t generation) const noexcept
    {
        requireOwnerThread(*impl_->control);
        return impl_->validSlot(index, generation)
            ? impl_->slots[index].state
            : EEntitySectionState::EMPTY;
    }

    bool EntitySectionLoaderSystem::releaseSettled(
        const lux::ecs::scene_format::EntitySectionId& section,
        std::uint64_t generation) const noexcept
    {
        requireOwnerThread(*impl_->control);
        const auto mapped = impl_->section_slots.find(section.value());
        if (mapped == impl_->section_slots.end() ||
            !impl_->validSlot(mapped->second, generation))
        {
            return true;
        }
        // A non-zero refcount after this selector released its ticket means a
        // distinct external or dependent owner deliberately shares the exact
        // generation. Otherwise its DEACTIVATE must cross the ECS barrier
        // before close is settled.
        return impl_->slots[mapped->second].references != 0u;
    }

    void EntitySectionLoaderSystem::onAdded(
        const lux::ecs::SystemSetupContext& setup)
    {
        requireOwnerThread(*impl_->control);
        if (!impl_->persistent_entities->boundTo(setup.registry()))
            std::abort();
        impl_->registry = &setup.registry();
        impl_->commands = setup.commands();
    }

    void EntitySectionLoaderSystem::update(
        const lux::ecs::SystemUpdateContext& context)
    {
        requireOwnerThread(*impl_->control);
        if (impl_->registry != &context.registry())
            std::abort();

        std::size_t work = 0u;
        const auto count = impl_->slots.size();
        for (std::size_t visited = 0u;
             visited < count &&
             work < impl_->config.staging_work_items_per_tick;
             ++visited)
        {
            const auto index = count == 0u
                ? 0u
                : (impl_->staging_cursor + visited) % count;
            auto& slot = impl_->slots[index];
            if (slot.state == EEntitySectionState::WAITING_ADMISSION)
            {
                impl_->launch(static_cast<std::uint32_t>(index));
                ++work;
                continue;
            }
            if (slot.state == EEntitySectionState::WAITING_BACKGROUND &&
                slot.launch_failed)
            {
                slot.launch_failed = false;
                impl_->acceptStopped(
                    static_cast<std::uint32_t>(index), slot.generation);
                continue;
            }
            if (slot.state == EEntitySectionState::ACTIVE &&
                slot.references == 0u)
            {
                if (impl_->enqueue(
                        static_cast<std::uint32_t>(index),
                        EEntitySectionCommandAction::DEACTIVATE))
                {
                    slot.state = EEntitySectionState::DEACTIVATE_QUEUED;
                }
                continue;
            }
            if (slot.state != EEntitySectionState::STAGING ||
                !slot.prepared)
            {
                continue;
            }

            auto staged = impl_->stager.advance(
                *slot.prepared,
                EntityBatchStageBudget{1u});
            ++work;
            if (!staged)
            {
                slot.batch_failure = std::move(staged.error());
                slot.prepared.reset();
                slot.state = EEntitySectionState::FAILED;
                continue;
            }
            if (staged->state != EPreparedEntityBatchState::READY)
                continue;

            const auto dependencies = impl_->dependencyState(slot);
            if (dependencies == Impl::EDependencyState::FAILED)
            {
                slot.prepared.reset();
                slot.state = EEntitySectionState::FAILED;
                continue;
            }
            if (dependencies != Impl::EDependencyState::READY)
                continue;

            auto armed = impl_->materializer.arm(
                *slot.prepared, context.registry());
            if (!armed)
            {
                slot.batch_failure = std::move(armed.error());
                slot.prepared.reset();
                slot.state = EEntitySectionState::FAILED;
                continue;
            }
            slot.state = EEntitySectionState::ARMED;
            if (!impl_->enqueue(
                    static_cast<std::uint32_t>(index),
                    EEntitySectionCommandAction::ACTIVATE))
            {
                const auto cancelled = impl_->materializer.cancelArmed(
                    *slot.prepared, context.registry());
                if (!cancelled)
                    std::abort();
                slot.prepared.reset();
                slot.state = EEntitySectionState::FAILED;
            }
        }
        if (count != 0u)
            impl_->staging_cursor = (impl_->staging_cursor + 1u) % count;
        impl_->blobs.pruneExpired();
        impl_->tryCompleteClose();
    }

    void EntitySectionCommand::apply(
        lux::meta::EntityRegistry& registry,
        EntitySectionLoaderSystem& owner) const noexcept
    {
        owner.applyCommand(*this, registry);
    }

    void EntitySectionLoaderSystem::applyCommand(
        const EntitySectionCommand& command,
        lux::meta::EntityRegistry& registry) noexcept
    {
        requireOwnerThread(*impl_->control);
        if (&registry != impl_->registry ||
            !impl_->validSlot(command.slot, command.generation))
        {
            return;
        }
        auto& slot = impl_->slots[command.slot];
        if (command.action == EEntitySectionCommandAction::ACTIVATE)
        {
            if (slot.state != EEntitySectionState::ARMED ||
                slot.references == 0u || !slot.prepared)
            {
                return;
            }
            static_cast<void>(impl_->materializer.publishAtBarrier(
                *slot.prepared, registry));
            slot.prepared.reset();
            slot.state = EEntitySectionState::ACTIVE;
            return;
        }
        if (slot.state != EEntitySectionState::DEACTIVATE_QUEUED ||
            slot.references != 0u)
        {
            return;
        }
        const auto removed = impl_->materializer.deactivate(
            slot.record.id,
            slot.generation,
            registry);
        if (!removed)
            std::abort();
        impl_->recycle(command.slot);
        impl_->tryCompleteClose();
    }

    void EntitySectionLoaderSystem::requestClose() noexcept
    {
        requestClose({});
    }

    void EntitySectionLoaderSystem::requestClose(
        lux::ecs::SystemCloseProgressSink progress) noexcept
    {
        requireOwnerThread(*impl_->control);
        if (progress)
            impl_->close_progress = progress;
        const bool first_close = !impl_->control->closing.exchange(
            true, std::memory_order_acq_rel);
        if (first_close)
        {
            impl_->scope.requestStop();
            // Close tears down the complete dependency graph as one owner.
            // Drop its internal edges first; normal recycle then cannot
            // recursively decrement a peer whose forced close reference is
            // already zero.
            for (auto& slot : impl_->slots)
            {
                slot.references = 0u;
                slot.external_references = 0u;
                slot.dependencies.clear();
            }
            for (std::uint32_t index = 0u; index < impl_->slots.size();
                 ++index)
            {
                auto& slot = impl_->slots[index];
                if (slot.state == EEntitySectionState::ARMED &&
                    slot.prepared)
                {
                    const auto cancelled =
                        impl_->materializer.cancelArmed(
                            *slot.prepared, *impl_->registry);
                    if (!cancelled)
                        std::abort();
                    slot.prepared.reset();
                    impl_->recycle(index);
                }
                else if (slot.state == EEntitySectionState::ACTIVE)
                {
                    if (impl_->enqueue(
                            index,
                            EEntitySectionCommandAction::DEACTIVATE))
                    {
                        slot.state =
                            EEntitySectionState::DEACTIVATE_QUEUED;
                    }
                }
                else if (slot.state ==
                             EEntitySectionState::WAITING_ADMISSION ||
                         slot.state ==
                             EEntitySectionState::WAITING_BACKGROUND ||
                         slot.state == EEntitySectionState::STAGING)
                {
                    slot.prepared.reset();
                    impl_->recycle(index);
                }
                else if (slot.state == EEntitySectionState::FAILED ||
                         slot.state == EEntitySectionState::CANCELLED)
                {
                    impl_->recycle(index);
                }
            }
            impl_->tryCompleteClose();
        }
        if (!impl_->scope_close_subscribed)
        {
            impl_->scope_close_subscribed = true;
            lux::exec::detail::subscribeScopeClose(
                impl_->scope,
                [weak = std::weak_ptr{impl_->control}]() noexcept
                {
                    const auto control = weak.lock();
                    if (!control)
                        return;
                    auto* owner = control->owner.load(
                        std::memory_order_acquire);
                    if (owner)
                        owner->acceptCloseScopeClosed();
                });
        }
    }

    bool EntitySectionLoaderSystem::closeComplete() const noexcept
    {
        requireOwnerThread(*impl_->control);
        return impl_->control->closing.load(std::memory_order_acquire) &&
            !impl_->close_barrier_admission && impl_->scope_closed;
    }

    bool EntitySectionLoaderSystem::closeNeedsOwnerTick() const noexcept
    {
        requireOwnerThread(*impl_->control);
        if (!impl_->control->closing.load(std::memory_order_acquire) ||
            !impl_->close_barrier_admission)
        {
            return false;
        }

        const auto materialized = impl_->materializer.snapshot();
        if (materialized.active_sections != 0u ||
            materialized.armed_sections != 0u ||
            !impl_->section_slots.empty())
        {
            return true;
        }

        // External ContentBlobLease owners change the ledger themselves and
        // wake through their owning scope/system. Re-ticking the loader while
        // bytes remain cannot make progress. Once the last lease is gone, one
        // owner tick is actionable to prune/settle the close barrier.
        const auto blobs = impl_->blobs.snapshot();
        return blobs.current_bytes == 0u &&
            blobs.allocation_count == 0u;
    }

    lux::exec::AsyncScopeCloseSender EntitySectionLoaderSystem::closeAsync()
        noexcept
    {
        requestClose();
        return impl_->scope.closeAsync();
    }

    void EntitySectionLoaderSystem::acceptCloseScopeClosed() noexcept
    {
        requireOwnerThread(*impl_->control);
        impl_->scope_closed = true;
        if (impl_->close_progress)
            impl_->close_progress.notify();
    }

    EntitySectionLoaderSnapshot EntitySectionLoaderSystem::snapshot()
        const noexcept
    {
        requireOwnerThread(*impl_->control);
        EntitySectionLoaderSnapshot result;
        result.stale_completions = impl_->stale_completions;
        result.cancelled_requests = impl_->cancelled_requests;
        result.queue_backpressure = impl_->queue_backpressure;
        result.command_rejections = impl_->command_rejections;
        result.already_destroyed_entities =
            impl_->materializer.snapshot().already_destroyed_entities;
        result.allocated_slots = impl_->slots.size();
        result.free_slots = impl_->free_slots.size();
        result.section_mappings = impl_->section_slots.size();
        result.closing = impl_->control->closing.load(
            std::memory_order_acquire);
        result.scope_closed = impl_->scope_closed;
        result.closed = closeComplete();
        result.blobs = impl_->blobs.snapshot();
        for (const auto& slot : impl_->slots)
        {
            result.outstanding_tickets += slot.external_references;
            switch (slot.state)
            {
            case EEntitySectionState::WAITING_ADMISSION:
                ++result.waiting_admission_sections;
                ++result.waiting_sections;
                break;
            case EEntitySectionState::WAITING_BACKGROUND:
                ++result.waiting_sections;
                break;
            case EEntitySectionState::STAGING:
                ++result.staging_sections;
                break;
            case EEntitySectionState::ARMED:
                ++result.armed_sections;
                break;
            case EEntitySectionState::ACTIVE:
            case EEntitySectionState::DEACTIVATE_QUEUED:
                ++result.active_sections;
                break;
            case EEntitySectionState::FAILED:
                ++result.failed_sections;
                break;
            default:
                break;
            }
        }
        return result;
    }

}
