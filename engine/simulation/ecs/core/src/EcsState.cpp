#include <lux/engine/simulation/ecs/EcsState.hpp>

#include <lux/engine/simulation/ecs/core/detail/EcsChangeLog.hpp>

#include <cstdlib>
#include <thread>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace lux::simulation::ecs
{
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
          changes_(std::make_unique<detail::EcsChangeLog>(
              detail::EcsChangeLogConfigValue{
                  config.changes.initial_bytes,
                  config.changes.max_bytes})),
          owner_thread_(std::this_thread::get_id())
    {
        detail::require(
            config.changes.initial_bytes <= config.changes.max_bytes &&
            config.changes.max_bytes >= 4096U
        );
    }

    EcsState::~EcsState() noexcept
    {
        detail::require(
            state_ == detail::EEcsState::IDLE ||
            state_ == detail::EEcsState::DESTROYING
        );
        detail::require(!execution_lease_);
        state_ = detail::EEcsState::DESTROYING;
    }

    lux::cxx::expected<EcsMutation, EcsMutationError> EcsState::mutate() noexcept
    {
        if (std::this_thread::get_id() != owner_thread_)
            return lux::cxx::unexpected(EcsMutationError{EEcsMutationError::WRONG_THREAD});
        if (state_ == detail::EEcsState::DESTROYING)
            return lux::cxx::unexpected(EcsMutationError{EEcsMutationError::DESTROYING});
        if (state_ != detail::EEcsState::IDLE)
            return lux::cxx::unexpected(EcsMutationError{EEcsMutationError::NOT_IDLE});

        state_ = detail::EEcsState::MUTATING;
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
        if (state_ != detail::EEcsState::IDLE || execution_lease_)
        {
            return lux::cxx::unexpected(EcsTaskExecutionError{
                EEcsTaskExecutionError::WORLD_BUSY
            });
        }
        execution_lease_ = true;
        state_ = detail::EEcsState::EXECUTING;
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
        detail::require(world_->state_ == detail::EEcsState::EXECUTING);
        world_->state_ = detail::EEcsState::IDLE;
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
            (void)detail::recordEcsEntityChange(
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
            (void)detail::recordEcsEntityChange(
                *world_, created, EEntityChangeKind::ADDED
            );
        }
        return created;
    }

    void EcsMutation::destroy(Entity entity)
    {
        detail::require(world_ != nullptr && world_->valid(entity));
        bool history_lost = false;
        if (change_emission_ == EChangeEmission::RECORD)
        {
            const auto entity_storage = entt::type_hash<Entity>::value();
            for (auto&& [storage_id, storage] : world_->registry_.storage())
            {
                if (storage_id != entity_storage && storage.contains(entity))
                {
                    if (!history_lost && !detail::recordEcsComponentChange(
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
            if (!history_lost && !detail::recordEcsEntityChange(
                *world_, entity, EEntityChangeKind::DESTROYED
            ))
                history_lost = true;
        }
    }

    void EcsMutation::release() noexcept
    {
        if (world_ != nullptr && release_to_idle_)
        {
            detail::require(
                world_->state_ == detail::EEcsState::MUTATING ||
                world_->state_ == detail::EEcsState::APPLYING_COMMANDS
            );
            world_->state_ = detail::EEcsState::IDLE;
        }
        world_ = nullptr;
        release_to_idle_ = false;
        change_emission_ = EChangeEmission::RECORD;
    }

    detail::ChangeRecorder detail::ecsChangeRecorder(EcsState& world) noexcept
    {
        return EcsChangeAccess::log(world).recorder();
    }

    detail::ChangeStreamBinder detail::ecsChangeStreamBinder(
        EcsState& world
    ) noexcept
    {
        return ChangeStreamBinder{
            .context = &EcsChangeAccess::log(world),
            .bind = [](void* context, std::uint64_t storage) noexcept
            {
                return static_cast<EcsChangeLog*>(context)->bindComponent(
                    storage
                );
            }
        };
    }

    bool detail::recordEcsComponentChange(
        EcsState& world,
        std::uint64_t storage,
        Entity entity,
        EComponentChangeKind kind
    ) noexcept
    {
        return EcsChangeAccess::log(world).recordComponent(
            storage,
            entity,
            kind
        );
    }

    bool detail::recordEcsEntityChange(
        EcsState& world,
        Entity entity,
        EEntityChangeKind kind
    ) noexcept
    {
        return EcsChangeAccess::log(world).recordEntity(entity, kind);
    }

    void detail::establishEcsChangeBaseline(EcsState& world) noexcept
    {
        EcsChangeAccess::log(world).establishBaseline();
    }

    void detail::markEcsChangeHistoryLoss(EcsState& world) noexcept
    {
        EcsChangeAccess::log(world).markHistoryLoss();
    }

    std::uint64_t detail::ecsChangeEpoch(const EcsState& world) noexcept
    {
        return EcsChangeAccess::log(world).epoch();
    }

    detail::ChangeRangeData detail::readEcsComponentChanges(
        const EcsState& world,
        std::uint64_t storage,
        std::uint64_t& cursor_epoch,
        std::uint64_t& cursor_sequence
    ) noexcept
    {
        return EcsChangeAccess::log(world).readComponentRaw(
            storage,
            cursor_epoch,
            cursor_sequence
        );
    }

    detail::ChangeRangeData detail::readEcsEntityChanges(
        const EcsState& world,
        std::uint64_t& cursor_epoch,
        std::uint64_t& cursor_sequence
    ) noexcept
    {
        return EcsChangeAccess::log(world).readEntityRaw(
            cursor_epoch,
            cursor_sequence
        );
    }
} // namespace lux::simulation::ecs
