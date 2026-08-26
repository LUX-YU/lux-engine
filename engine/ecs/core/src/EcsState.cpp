#include <lux/engine/ecs/EcsState.hpp>

#include <lux/engine/ecs/core/detail/WorldChangeLog.hpp>
#include <lux/engine/ecs/core/detail/SectionMembershipDirectory.hpp>

#include <atomic>
#include <cstdlib>
#include <limits>
#include <thread>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace lux::ecs
{
    namespace
    {
        std::atomic<std::uint64_t> next_world_identity{1U};

        [[nodiscard]] std::uint64_t allocateWorldIdentity() noexcept
        {
            const std::uint64_t value = next_world_identity.fetch_add(
                1U,
                std::memory_order_relaxed
            );
            detail::require(
                value != 0U &&
                value != std::numeric_limits<std::uint64_t>::max()
            );
            return value;
        }
    } // namespace

    [[noreturn]] void detail::contractFailure() noexcept
    {
#if defined(_MSC_VER)
        __fastfail(7u);
#else
        std::abort();
#endif
    }

    EcsState::EcsState(EcsStateConfig config)
        : config_(config),
          changes_(std::make_unique<detail::WorldChangeLog>(
              detail::WorldChangeLogConfigValue{
                  config.changes.initial_bytes,
                  config.changes.max_bytes})),
          section_memberships_(
              std::make_unique<detail::SectionMembershipDirectory>()
          ),
          owner_thread_(std::this_thread::get_id()),
          identity_(allocateWorldIdentity())
    {
        detail::require(
            config.changes.initial_bytes <= config.changes.max_bytes &&
            config.changes.max_bytes >= 4096U
        );
    }

    EcsState::~EcsState() noexcept
    {
        detail::require(
            state_ == detail::EWorldState::IDLE ||
            state_ == detail::EWorldState::DESTROYING
        );
        detail::require(!execution_lease_);
        detail::require(active_section_count_ == 0U);
        state_ = detail::EWorldState::DESTROYING;
    }

    lux::cxx::expected<EcsMutation, EcsMutationError> EcsState::mutate() noexcept
    {
        if (std::this_thread::get_id() != owner_thread_)
            return lux::cxx::unexpected(EcsMutationError{EEcsMutationError::WRONG_THREAD});
        if (state_ == detail::EWorldState::DESTROYING)
            return lux::cxx::unexpected(EcsMutationError{EEcsMutationError::DESTROYING});
        if (state_ != detail::EWorldState::IDLE)
            return lux::cxx::unexpected(EcsMutationError{EEcsMutationError::NOT_IDLE});

        state_ = detail::EWorldState::MUTATING;
        return EcsMutation(*this, true);
    }

    lux::cxx::expected<
        EcsTaskExecutionLease,
        EcsTaskExecutionError>
    EcsState::beginTaskExecution() noexcept
    {
        if (std::this_thread::get_id() != owner_thread_)
        {
            return lux::cxx::unexpected(EcsTaskExecutionError{
                EEcsTaskExecutionError::WRONG_THREAD
            });
        }
        if (state_ != detail::EWorldState::IDLE || execution_lease_)
        {
            return lux::cxx::unexpected(EcsTaskExecutionError{
                EEcsTaskExecutionError::WORLD_BUSY
            });
        }
        execution_lease_ = true;
        state_ = detail::EWorldState::EXECUTING;
        return EcsTaskExecutionLease(*this);
    }

    EcsTaskExecutionLease::EcsTaskExecutionLease(EcsState& world) noexcept
        : world_(&world)
    {
    }

    EcsTaskExecutionLease::~EcsTaskExecutionLease() noexcept
    {
        release();
    }

    EcsTaskExecutionLease::EcsTaskExecutionLease(
        EcsTaskExecutionLease&& other
    ) noexcept
        : world_(std::exchange(other.world_, nullptr))
    {
    }

    EcsTaskExecutionLease& EcsTaskExecutionLease::operator=(
        EcsTaskExecutionLease&& other
    ) noexcept
    {
        if (this != std::addressof(other))
        {
            release();
            world_ = std::exchange(other.world_, nullptr);
        }
        return *this;
    }

    void EcsTaskExecutionLease::release() noexcept
    {
        if (world_ == nullptr)
            return;
        detail::require(world_->execution_lease_);
        detail::require(world_->state_ == detail::EWorldState::EXECUTING);
        world_->state_ = detail::EWorldState::IDLE;
        world_->execution_lease_ = false;
        world_ = nullptr;
    }

    EcsMutation::EcsMutation(
        EcsState& world,
        bool release_to_idle,
        EChangeEmission change_emission
    ) noexcept
        : world_(&world), release_to_idle_(release_to_idle),
          change_emission_(change_emission)
    {
    }

    EcsMutation::EcsMutation(EcsMutation&& other) noexcept
        : world_(std::exchange(other.world_, nullptr)),
          release_to_idle_(std::exchange(other.release_to_idle_, false)),
          change_emission_(std::exchange(
              other.change_emission_, EChangeEmission::RECORD
          ))
    {
    }

    EcsMutation& EcsMutation::operator=(EcsMutation&& other) noexcept
    {
        if (this != &other)
        {
            release();
            world_ = std::exchange(other.world_, nullptr);
            release_to_idle_ = std::exchange(other.release_to_idle_, false);
            change_emission_ = std::exchange(
                other.change_emission_, EChangeEmission::RECORD
            );
        }
        return *this;
    }

    EcsMutation::~EcsMutation() noexcept
    {
        release();
    }

    Entity EcsMutation::create()
    {
        detail::require(world_ != nullptr);
        const Entity entity = world_->registry_.create();
        if (change_emission_ == EChangeEmission::RECORD)
        {
            (void)detail::recordWorldEntityChange(
                *world_, entity, EEntityChangeKind::ADDED
            );
        }
        return entity;
    }

    Entity EcsMutation::createAt(Entity entity)
    {
        detail::require(world_ != nullptr && entity != NullEntity);
        const Entity created = world_->registry_.create(entity);
        if (change_emission_ == EChangeEmission::RECORD)
        {
            (void)detail::recordWorldEntityChange(
                *world_, created, EEntityChangeKind::ADDED
            );
        }
        return created;
    }

    void EcsMutation::destroy(Entity entity)
    {
        detail::require(world_ != nullptr && world_->valid(entity));
        bool history_lost = false;
        if (world_->section_memberships_->tracked(entity))
        {
            world_->section_memberships_->forEachStorage(
                entity,
                [&](std::uint64_t storage_id) noexcept
                {
                    auto* storage = world_->registry_.storage(storage_id);
                    detail::require(storage != nullptr);
                    if (change_emission_ == EChangeEmission::RECORD)
                    {
                        if (!history_lost && !detail::recordWorldComponentChange(
                            *world_, storage_id, entity,
                            EComponentChangeKind::REMOVED
                        ))
                            history_lost = true;
                    }
                    storage->remove(entity);
                }
            );
            world_->section_memberships_->deactivate(entity);
            world_->registry_.template storage<Entity>().erase(entity);
            if (change_emission_ == EChangeEmission::RECORD)
            {
                if (!history_lost && !detail::recordWorldEntityChange(
                    *world_, entity, EEntityChangeKind::DESTROYED
                ))
                    history_lost = true;
            }
            return;
        }
        if (change_emission_ == EChangeEmission::RECORD)
        {
            const auto entity_storage = entt::type_hash<Entity>::value();
            for (auto&& [storage_id, storage] : world_->registry_.storage())
            {
                if (storage_id != entity_storage && storage.contains(entity))
                {
                    if (!history_lost && !detail::recordWorldComponentChange(
                        *world_, storage_id, entity,
                        EComponentChangeKind::REMOVED
                    ))
                        history_lost = true;
                }
            }
        }
        world_->registry_.destroy(entity);
        if (change_emission_ == EChangeEmission::RECORD)
        {
            if (!history_lost && !detail::recordWorldEntityChange(
                *world_, entity, EEntityChangeKind::DESTROYED
            ))
                history_lost = true;
        }
    }

    std::uint32_t detail::WorldMembershipAccess::prepareAdd(
        EcsState& world,
        Entity entity,
        std::uint64_t storage
    )
    {
        return world.section_memberships_->prepareAdd(entity, storage);
    }

    void detail::WorldMembershipAccess::commitAdd(
        EcsState& world,
        Entity entity,
        std::uint32_t token
    ) noexcept
    {
        world.section_memberships_->commitAdd(entity, token);
    }

    void detail::WorldMembershipAccess::cancelAdd(
        EcsState& world,
        std::uint32_t token
    ) noexcept
    {
        world.section_memberships_->cancelAdd(token);
    }

    void detail::WorldMembershipAccess::remove(
        EcsState& world,
        Entity entity,
        std::uint64_t storage
    ) noexcept
    {
        world.section_memberships_->remove(entity, storage);
    }

    void EcsMutation::release() noexcept
    {
        if (world_ != nullptr && release_to_idle_)
        {
            detail::require(
                world_->state_ == detail::EWorldState::MUTATING ||
                world_->state_ == detail::EWorldState::APPLYING_COMMANDS
            );
            world_->state_ = detail::EWorldState::IDLE;
        }
        world_ = nullptr;
        release_to_idle_ = false;
        change_emission_ = EChangeEmission::RECORD;
    }

    detail::ChangeRecorder detail::worldChangeRecorder(EcsState& world) noexcept
    {
        return WorldChangeAccess::log(world).recorder();
    }

    detail::ChangeStreamBinder detail::worldChangeStreamBinder(
        EcsState& world
    ) noexcept
    {
        return ChangeStreamBinder{
            .context = &WorldChangeAccess::log(world),
            .bind = [](void* context, std::uint64_t storage) noexcept
            {
                return static_cast<WorldChangeLog*>(context)->bindComponent(
                    storage
                );
            }
        };
    }

    bool detail::recordWorldComponentChange(
        EcsState& world,
        std::uint64_t storage,
        Entity entity,
        EComponentChangeKind kind
    ) noexcept
    {
        return WorldChangeAccess::log(world).recordComponent(
            storage,
            entity,
            kind
        );
    }

    bool detail::recordWorldEntityChange(
        EcsState& world,
        Entity entity,
        EEntityChangeKind kind
    ) noexcept
    {
        return WorldChangeAccess::log(world).recordEntity(entity, kind);
    }

    void detail::establishWorldChangeBaseline(EcsState& world) noexcept
    {
        WorldChangeAccess::log(world).establishBaseline();
    }

    void detail::markWorldChangeHistoryLoss(EcsState& world) noexcept
    {
        WorldChangeAccess::log(world).markHistoryLoss();
    }

    std::uint64_t detail::worldChangeEpoch(const EcsState& world) noexcept
    {
        return WorldChangeAccess::log(world).epoch();
    }

    detail::ChangeRangeData detail::readWorldComponentChanges(
        const EcsState& world,
        std::uint64_t storage,
        std::uint64_t& cursor_epoch,
        std::uint64_t& cursor_sequence
    ) noexcept
    {
        return WorldChangeAccess::log(world).readComponentRaw(
            storage,
            cursor_epoch,
            cursor_sequence
        );
    }

    detail::ChangeRangeData detail::readWorldEntityChanges(
        const EcsState& world,
        std::uint64_t& cursor_epoch,
        std::uint64_t& cursor_sequence
    ) noexcept
    {
        return WorldChangeAccess::log(world).readEntityRaw(
            cursor_epoch,
            cursor_sequence
        );
    }
} // namespace lux::ecs
