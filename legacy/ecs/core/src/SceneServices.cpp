#include <lux/engine/ecs/SceneServices.hpp>

#include <algorithm>
#include <array>

namespace lux::ecs
{
    detail::SceneServiceOwner::~SceneServiceOwner() noexcept = default;
    SceneServiceTransaction::DeferredEdit::~DeferredEdit() noexcept = default;

    SceneServiceLease::SceneServiceLease(
        std::shared_ptr<detail::SceneServiceState> state) noexcept
        : state_(std::move(state))
        , generation_(state_ ? state_->generation : 0u)
    {
    }

    SceneServiceLease::~SceneServiceLease() noexcept
    {
        reset();
    }

    SceneServiceLease::SceneServiceLease(SceneServiceLease&& other) noexcept
        : state_(std::move(other.state_))
        , generation_(std::exchange(other.generation_, 0u))
    {
    }

    SceneServiceLease& SceneServiceLease::operator=(
        SceneServiceLease&& other) noexcept
    {
        if (this == &other)
            return *this;
        reset();
        state_ = std::move(other.state_);
        generation_ = std::exchange(other.generation_, 0u);
        return *this;
    }

    void SceneServiceLease::reset() noexcept
    {
        if (state_ && state_->active && state_->generation == generation_)
            SceneServices::retire(*state_);
        state_.reset();
        generation_ = 0u;
    }

    SceneServiceLease::operator bool() const noexcept
    {
        return state_ && state_->active &&
            state_->generation == generation_ && state_->ptr;
    }

    InstalledSceneServiceBatch::~InstalledSceneServiceBatch() noexcept
    {
        reset();
    }

    void InstalledSceneServiceBatch::reset() noexcept
    {
        while (!leases_.empty())
        {
            leases_.back().reset();
            leases_.pop_back();
        }
    }

    void SceneServices::retire(detail::SceneServiceState& state) noexcept
    {
        if (!state.active)
            return;
        state.active = false;
        state.ptr = nullptr;
        state.owner.reset();
        ++state.generation;
        if (state.generation == 0u)
            ++state.generation;
    }

    void SceneServices::pruneRetired() noexcept
    {
        std::erase_if(
            slots_,
            [](const auto& state) noexcept
            {
                return !state || !state->active;
            });
    }

    SceneServiceResult<InstalledSceneServiceBatch>
    SceneServices::install(SceneServiceMutationBatch&& batch)
    {
        const std::size_t partition_size = batch.states_.size();
        const std::array partitions{partition_size};
        auto installed = installPartitioned(
            std::move(batch),
            partitions);
        if (!installed)
            return lux::cxx::unexpected(installed.error());
        return std::move(installed->front());
    }

    SceneServiceResult<std::vector<InstalledSceneServiceBatch>>
    SceneServices::installPartitioned(
        SceneServiceMutationBatch&& batch,
        std::span<const std::size_t> partition_sizes)
    {
        if (state_ != EState::Open)
        {
            return lux::cxx::unexpected(
                ESceneServiceRegistrationError::MutationUnavailable);
        }
        if (batch.states_.empty())
        {
            return lux::cxx::unexpected(
                ESceneServiceRegistrationError::NullService);
        }

        std::size_t partition_total = 0u;
        for (const auto size : partition_sizes)
        {
            if (size > batch.states_.size() -
                    std::min(partition_total, batch.states_.size()))
            {
                return lux::cxx::unexpected(
                    ESceneServiceRegistrationError::MutationUnavailable);
            }
            partition_total += size;
        }
        if (partition_sizes.empty() ||
            partition_total != batch.states_.size())
        {
            return lux::cxx::unexpected(
                ESceneServiceRegistrationError::MutationUnavailable);
        }
        for (const auto& candidate : batch.states_)
        {
            if (!candidate || !candidate->ptr)
            {
                return lux::cxx::unexpected(
                    ESceneServiceRegistrationError::NullService);
            }
            if (contains(candidate->type))
            {
                return lux::cxx::unexpected(
                    ESceneServiceRegistrationError::DuplicateType);
            }
        }

        pruneRetired();
        std::vector<InstalledSceneServiceBatch> installed;
        installed.resize(partition_sizes.size());
        for (std::size_t index = 0u; index < partition_sizes.size(); ++index)
            installed[index].leases_.reserve(partition_sizes[index]);
        slots_.reserve(slots_.size() + batch.states_.size());

        std::size_t partition = 0u;
        std::size_t in_partition = 0u;
        for (auto& candidate : batch.states_)
        {
            while (partition < partition_sizes.size() &&
                   in_partition == partition_sizes[partition])
            {
                ++partition;
                in_partition = 0u;
            }
            candidate->active = true;
            slots_.push_back(candidate);
            installed[partition].leases_.push_back(
                SceneServiceLease{candidate});
            ++in_partition;
        }
        batch.states_.clear();
        return installed;
    }

    SceneServices::~SceneServices() noexcept
    {
        // Later services may borrow earlier ones, so teardown is explicitly
        // reverse-registration order. The terminal state prevents a service
        // destructor from traversing or extending slots_ while pop_back owns
        // the vector's active element.
        state_ = EState::Destroying;
        while (!slots_.empty())
        {
            retire(*slots_.back());
            slots_.pop_back();
        }
    }

    lux::cxx::expected<void, SceneServiceType>
    SceneServiceTransaction::publish()
    {
        if (state_ == EState::Published)
            return {};
        if (state_ != EState::Open)
            return lux::cxx::unexpected(SceneServiceType{});
        if (base_.state_ != SceneServices::EState::Open)
            return lux::cxx::unexpected(SceneServiceType{});

        for (const auto& slot : staged_.slots_)
            if (base_.contains(slot->type))
                return lux::cxx::unexpected(slot->type);

        // Reserve before touching an adopted external object. Once edits
        // begin, every remaining operation is a no-fail ownership transfer.
        base_.slots_.reserve(base_.slots_.size() + staged_.slots_.size());
        published_.reserve(staged_.slots_.size());
        published_claimed_.assign(staged_.slots_.size(), false);

        SceneServices::OperationGuard base_state{
            base_,
            SceneServices::EState::MutationBlocked,
        };
        state_ = EState::Publishing;
        for (auto& edit : deferred_edits_)
            edit->apply();
        deferred_edits_.clear();

        for (auto& slot : staged_.slots_)
        {
            published_.push_back(slot);
            base_.slots_.push_back(std::move(slot));
        }
        staged_.slots_.clear();
        state_ = EState::Published;
        return {};
    }

    bool SceneServiceTransaction::canClaimPublished(
        std::size_t first,
        std::size_t last) const noexcept
    {
        if (state_ != EState::Published || first > last ||
            last > published_.size())
        {
            return false;
        }
        return std::ranges::none_of(
            published_claimed_.begin() + static_cast<std::ptrdiff_t>(first),
            published_claimed_.begin() + static_cast<std::ptrdiff_t>(last),
            [](bool claimed) noexcept { return claimed; });
    }

    InstalledSceneServiceBatch SceneServiceTransaction::claimPublished(
        std::size_t first,
        std::size_t last) noexcept
    {
        InstalledSceneServiceBatch result;
        if (!canClaimPublished(first, last))
            return result;
        result.leases_.reserve(last - first);
        for (auto index = first; index < last; ++index)
        {
            published_claimed_[index] = true;
            result.leases_.push_back(SceneServiceLease{published_[index]});
        }
        return result;
    }

} // namespace lux::ecs
