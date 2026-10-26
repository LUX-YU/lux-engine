#include <lux/engine/context/render/RenderAssets.hpp>
#include <lux/engine/context/render/RenderDiagnostics.hpp>

namespace lux::engine::render
{
    using namespace lux::render;

    RenderAssets::RenderAssets(
        RenderExecution& rendering,
        process::asset_loading::AssetReadPort source,
        CapabilityQuery capability,
        RenderAssetLimits limits
    ) noexcept
        : rendering_(rendering), source_(std::move(source)), resource_capability_(std::move(capability)),
          limits_(limits), tasks_(rendering.execution())
    {
    }

    RenderAssets::CreateResult RenderAssets::create(
        RenderExecution& rendering,
        process::asset_loading::AssetReadPort source,
        CapabilityQuery capability,
        RenderAssetLimits limits
    ) noexcept
    {
        const bool invalid_limits =
            limits.in_flight == 0 || limits.decode.max_image_bytes == 0 || limits.decode.max_decoded_bytes == 0;
        if (!source || invalid_limits)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        return std::unique_ptr<RenderAssets>{
            new RenderAssets(rendering, std::move(source), std::move(capability), limits)
        };
    }

    RenderAssets::~RenderAssets() noexcept
    {
        if (!settled() || connection_count_ != 0)
        {
            std::terminate();
        }
    }

    void RenderAssets::awaitTaskCapacity() noexcept
    {
        if (capacity_notification_pending_)
        {
            return;
        }
        capacity_notification_pending_ = true;
        auto completion = stdexec::upon_stopped(
            stdexec::then(tasks_.whenCapacityAvailable(), []() noexcept { return true; }),
            []() noexcept { return false; }
        );
        auto accepted = rendering_.execution().spawn(stdexec::then(
            stdexec::continues_on(std::move(completion), rendering_.execution().mainScheduler()),
            [this](bool available) noexcept
            {
                capacity_notification_pending_ = false;
                if (available)
                {
                    notifyCapacity();
                }
            }
        ));
        if (!accepted)
        {
            std::terminate(); // Shared execution outlives all RenderAssets operations.
        }
    }

    void RenderAssets::notifyVersion(std::vector<AvailabilityConnection*>& observers, RenderAssetVersion key) noexcept
    {
        while (!observers.empty())
        {
            auto* observer = observers.back();
            observers.pop_back();
            std::erase(observer->versions_, &observers);
            observer->completed_(observer->receiver_, key);
        }
    }

    void RenderAssets::notifyCapacity() noexcept
    {
        // Detach this generation before invoking callbacks; a new wait belongs to the next release.
        auto* ready = std::exchange(capacity_waiters_, nullptr);
        if (ready)
        {
            ready->previous_ = &ready;
        }
        while (ready)
        {
            auto* observer = ready;
            observer->disconnectCapacity();
            observer->capacity_available_(observer->receiver_);
        }
    }

    RenderAssets::AvailabilityConnection::AvailabilityConnection(
        RenderAssets& assets,
        void* receiver,
        void (*completed)(void*, RenderAssetVersion) noexcept,
        void (*capacity_available)(void*) noexcept
    ) noexcept
        : assets_(assets), receiver_(receiver), completed_(completed), capacity_available_(capacity_available)
    {
        ++assets_.connection_count_;
    }

    void RenderAssets::AvailabilityConnection::subscribe(
        std::vector<AvailabilityConnection*>& observers,
        RenderAssetVersion key,
        bool loading
    ) noexcept
    {
        if (!loading)
        {
            completed_(receiver_, key);
        }
        else if (std::ranges::find(observers, this) == observers.end())
        {
            observers.push_back(this);
            versions_.push_back(&observers);
        }
    }

    void RenderAssets::AvailabilityConnection::whenCapacityAvailable() noexcept
    {
        if (previous_)
        {
            return;
        }
        auto& head = assets_.capacity_waiters_;
        next_ = head;
        previous_ = &head;
        if (head)
        {
            head->previous_ = &next_;
        }
        head = this;
    }

    void RenderAssets::AvailabilityConnection::disconnectCapacity() noexcept
    {
        if (!previous_)
        {
            return;
        }
        *previous_ = next_;
        if (next_)
        {
            next_->previous_ = previous_;
        }
        previous_ = nullptr;
        next_ = nullptr;
    }

    RenderAssets::AvailabilityConnection::~AvailabilityConnection() noexcept
    {
        disconnectCapacity();
        for (auto* observers : versions_)
        {
            std::erase(*observers, this);
        }
        --assets_.connection_count_;
    }

    void RenderAssets::recordReleaseFailure(RenderError error, std::string_view pipeline) noexcept
    {
        reportRenderFailure(error, pipeline);
        if (release_failure_.type == 0)
        {
            release_failure_ = error;
        }
    }

    void RenderAssets::addInputs(std::span<const CapabilitiesInstalled::Input> inputs) noexcept
    {
        for (const auto& input : inputs)
        {
            const bool duplicate = std::ranges::any_of(
                inputs_,
                [&](const auto& current) noexcept
                { return current.schema == input.schema && current.destination == input.destination; }
            );
            if (!duplicate)
            {
                inputs_.push_back(input);
            }
        }
    }

    std::span<const CapabilitiesInstalled::Input> RenderAssets::inputs() const noexcept
    {
        return inputs_;
    }

    RenderExecution& RenderAssets::executionContext() const noexcept
    {
        return rendering_;
    }

    const CapabilityQuery& RenderAssets::resourceCapability() const noexcept
    {
        return resource_capability_;
    }

    RenderResult<bool> RenderAssets::removeTables(const VRenderScope& scope) noexcept
    {
        for (const auto& installed : tables_)
        {
            const auto& table = *installed.table;
            if (table.destination.scope == scope && !table.empty())
            {
                if (table.settled())
                {
                    return cxx::unexpected(table.releaseFailure());
                }
                return false;
            }
        }
        // Move the whole code-pinned unit out before vector compaction; default move assignment
        // would otherwise release a pin before destroying the table that still uses its code.
        for (auto entry = tables_.begin(); entry != tables_.end();)
        {
            if (entry->table->destination.scope == scope)
            {
                auto removed = std::move(*entry);
                entry = tables_.erase(entry);
            }
            else
            {
                ++entry;
            }
        }
        return true;
    }

    void RenderAssets::close() noexcept
    {
        stopLoads();
        for (const auto& installed : tables_)
        {
            installed.table->close();
        }
    }

    void RenderAssets::stopLoads() noexcept
    {
        closing_ = true;
        stop_.request_stop();
        tasks_.requestStop();
    }

    bool RenderAssets::loadsSettled() const noexcept
    {
        return in_flight_ == 0 && !capacity_notification_pending_ && tasks_.settled();
    }

    bool RenderAssets::settled() const noexcept
    {
        return loadsSettled() &&
               std::ranges::all_of(tables_, [](const auto& installed) noexcept { return installed.table->settled(); });
    }

    RenderError RenderAssets::releaseFailure() const noexcept
    {
        return release_failure_;
    }
} // namespace lux::engine::render
