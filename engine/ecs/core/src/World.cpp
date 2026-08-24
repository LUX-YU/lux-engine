#include <lux/engine/ecs/World.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdlib>
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

    World::World() = default;

    World::~World() noexcept
    {
        detail::require(
            state_ == detail::EWorldState::IDLE ||
            state_ == detail::EWorldState::DESTROYING
        );
        detail::require(schedule_ == nullptr && observer_relations_ == 0);
        state_ = detail::EWorldState::DESTROYING;
    }

    lux::cxx::expected<WorldEdit, WorldEditError> World::edit() noexcept
    {
        if (state_ == detail::EWorldState::DESTROYING)
        {
            return lux::cxx::unexpected(
                WorldEditError{EWorldEditError::DESTROYING}
            );
        }
        if (state_ != detail::EWorldState::IDLE)
        {
            return lux::cxx::unexpected(
                WorldEditError{EWorldEditError::NOT_IDLE}
            );
        }

        state_ = detail::EWorldState::EDITING;
        return WorldEdit(*this, true);
    }

    WorldEdit::WorldEdit(World& world, bool release_to_idle) noexcept
        : world_(&world), release_to_idle_(release_to_idle)
    {
    }

    WorldEdit::WorldEdit(WorldEdit&& other) noexcept
        : world_(std::exchange(other.world_, nullptr)),
          release_to_idle_(std::exchange(other.release_to_idle_, false))
    {
    }

    WorldEdit& WorldEdit::operator=(WorldEdit&& other) noexcept
    {
        if (this != &other)
        {
            release();
            world_ = std::exchange(other.world_, nullptr);
            release_to_idle_ = std::exchange(other.release_to_idle_, false);
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
        return world_->registry_.create();
    }

    Entity WorldEdit::createAt(Entity entity)
    {
        detail::require(world_ != nullptr && entity != NullEntity);
        return world_->registry_.create(entity);
    }

    void WorldEdit::destroy(Entity entity)
    {
        detail::require(world_ != nullptr && world_->valid(entity));
        world_->registry_.destroy(entity);
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
    }
} // namespace lux::ecs
