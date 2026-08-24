#include <lux/engine/ecs/World.hpp>

#include <lux/engine/ecs/core/detail/ChangeJournal.hpp>

#include <cstdlib>
#include <thread>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace lux::ecs
{
    [[noreturn]] void detail::contractFailure() noexcept
    {
#if defined(_MSC_VER)
        __fastfail(7u);
#else
        std::abort();
#endif
    }

    World::World(WorldConfig config)
        : config_(config),
          changes_(std::make_unique<detail::ChangeJournal>(
              detail::ChangeJournalConfigValue{
                  config.changes.initial_bytes,
                  config.changes.max_bytes})),
          owner_thread_(std::this_thread::get_id())
    {
        detail::require(
            config.changes.initial_bytes <= config.changes.max_bytes &&
            config.changes.max_bytes >= 4096U
        );
    }

    World::~World() noexcept
    {
        detail::require(
            state_ == detail::EWorldState::IDLE ||
            state_ == detail::EWorldState::DESTROYING
        );
        detail::require(schedule_ == nullptr);
        state_ = detail::EWorldState::DESTROYING;
    }

    lux::cxx::expected<WorldEdit, WorldEditError> World::edit() noexcept
    {
        if (std::this_thread::get_id() != owner_thread_)
            return lux::cxx::unexpected(WorldEditError{EWorldEditError::WRONG_THREAD});
        if (state_ == detail::EWorldState::DESTROYING)
            return lux::cxx::unexpected(WorldEditError{EWorldEditError::DESTROYING});
        if (state_ != detail::EWorldState::IDLE)
            return lux::cxx::unexpected(WorldEditError{EWorldEditError::NOT_IDLE});

        state_ = detail::EWorldState::EDITING;
        return WorldEdit(*this, true);
    }

    WorldEdit::WorldEdit(
        World& world,
        bool release_to_idle,
        EChangeEmission change_emission
    ) noexcept
        : world_(&world), release_to_idle_(release_to_idle),
          change_emission_(change_emission)
    {
    }

    WorldEdit::WorldEdit(WorldEdit&& other) noexcept
        : world_(std::exchange(other.world_, nullptr)),
          release_to_idle_(std::exchange(other.release_to_idle_, false)),
          change_emission_(std::exchange(
              other.change_emission_, EChangeEmission::RECORD
          ))
    {
    }

    WorldEdit& WorldEdit::operator=(WorldEdit&& other) noexcept
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

    WorldEdit::~WorldEdit() noexcept
    {
        release();
    }

    Entity WorldEdit::create()
    {
        detail::require(world_ != nullptr);
        const Entity entity = world_->registry_.create();
        if (change_emission_ == EChangeEmission::RECORD)
        {
            detail::recordWorldEntityChange(
                *world_, entity, EEntityChangeKind::ADDED
            );
        }
        return entity;
    }

    Entity WorldEdit::createAt(Entity entity)
    {
        detail::require(world_ != nullptr && entity != NullEntity);
        const Entity created = world_->registry_.create(entity);
        if (change_emission_ == EChangeEmission::RECORD)
        {
            detail::recordWorldEntityChange(
                *world_, created, EEntityChangeKind::ADDED
            );
        }
        return created;
    }

    void WorldEdit::destroy(Entity entity)
    {
        detail::require(world_ != nullptr && world_->valid(entity));
        if (change_emission_ == EChangeEmission::RECORD)
        {
            const auto entity_storage = entt::type_hash<Entity>::value();
            for (auto&& [storage_id, storage] : world_->registry_.storage())
            {
                if (storage_id != entity_storage && storage.contains(entity))
                {
                    detail::recordWorldComponentChange(
                        *world_, storage_id, entity,
                        EComponentChangeKind::REMOVED
                    );
                }
            }
        }
        world_->registry_.destroy(entity);
        if (change_emission_ == EChangeEmission::RECORD)
        {
            detail::recordWorldEntityChange(
                *world_, entity, EEntityChangeKind::DESTROYED
            );
        }
    }

    void WorldEdit::release() noexcept
    {
        if (world_ != nullptr && release_to_idle_)
        {
            detail::require(
                world_->state_ == detail::EWorldState::EDITING ||
                world_->state_ == detail::EWorldState::APPLYING_COMMANDS
            );
            world_->state_ = detail::EWorldState::IDLE;
        }
        world_ = nullptr;
        release_to_idle_ = false;
        change_emission_ = EChangeEmission::RECORD;
    }

    detail::ChangeRecorder detail::worldChangeRecorder(World& world) noexcept
    {
        return WorldChangeAccess::journal(world).recorder();
    }

    void detail::recordWorldComponentChange(
        World& world,
        std::uint64_t storage,
        Entity entity,
        EComponentChangeKind kind
    ) noexcept
    {
        WorldChangeAccess::journal(world).recordComponent(storage, entity, kind);
    }

    void detail::recordWorldEntityChange(
        World& world,
        Entity entity,
        EEntityChangeKind kind
    ) noexcept
    {
        WorldChangeAccess::journal(world).recordEntity(entity, kind);
    }

    void detail::establishWorldChangeBaseline(World& world) noexcept
    {
        WorldChangeAccess::journal(world).establishBaseline();
    }

    void detail::markWorldChangeHistoryLoss(World& world) noexcept
    {
        WorldChangeAccess::journal(world).markHistoryLoss();
    }

    detail::ChangeRangeData detail::readWorldComponentChanges(
        const World& world,
        std::uint64_t storage,
        std::uint64_t& cursor_epoch,
        std::uint64_t& cursor_sequence
    ) noexcept
    {
        return WorldChangeAccess::journal(world).readComponentRaw(
            storage,
            cursor_epoch,
            cursor_sequence
        );
    }

    detail::ChangeRangeData detail::readWorldEntityChanges(
        const World& world,
        std::uint64_t& cursor_epoch,
        std::uint64_t& cursor_sequence
    ) noexcept
    {
        return WorldChangeAccess::journal(world).readEntityRaw(
            cursor_epoch,
            cursor_sequence
        );
    }
} // namespace lux::ecs
