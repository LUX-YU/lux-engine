#include <lux/engine/simulation/ecs/EcsState.hpp>

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

    EcsState::EcsState() noexcept
        : owner_thread_(std::this_thread::get_id())
    {
    }

    EcsState::~EcsState() noexcept
    {
        detail::require(
            state_ == detail::EEcsState::IDLE ||
            state_ == detail::EEcsState::DESTROYING
        );
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

    EcsMutation::EcsMutation(
        EcsState& world,
        bool release_to_idle
    ) noexcept
        : world_(&world), release_to_idle_(release_to_idle)
    {
    }

    EcsMutation::EcsMutation(EcsMutation&& other) noexcept
        : world_(std::exchange(other.world_, nullptr)),
          release_to_idle_(std::exchange(other.release_to_idle_, false))
    {
    }

    EcsMutation& EcsMutation::operator=(EcsMutation&& other) noexcept
    {
        if (this != &other)
        {
            release();
            world_ = std::exchange(other.world_, nullptr);
            release_to_idle_ = std::exchange(other.release_to_idle_, false);
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
        return world_->registry_.create();
    }

    Entity EcsMutation::createAt(Entity entity)
    {
        detail::require(world_ != nullptr && entity != NullEntity);
        return world_->registry_.create(entity);
    }

    void EcsMutation::destroy(Entity entity)
    {
        detail::require(world_ != nullptr && world_->valid(entity));
        world_->registry_.destroy(entity);
    }

    void EcsMutation::release() noexcept
    {
        if (world_ != nullptr && release_to_idle_)
        {
            detail::require(
                world_->state_ == detail::EEcsState::MUTATING
            );
            world_->state_ = detail::EEcsState::IDLE;
        }
        world_ = nullptr;
        release_to_idle_ = false;
    }
} // namespace lux::simulation::ecs
