#include <lux/engine/events/DomainEvents.hpp>

namespace lux::events
{
    DomainEvents::DomainEvents() noexcept : owner_thread_(std::this_thread::get_id())
    {
    }

    DomainEvents::~DomainEvents()
    {
        ownerCheck();
        for (const auto& [type, channel] : channels_)
        {
            (void)type;
            if (channel->liveSubscriptions() != 0u)
                std::terminate();
        }
    }

    EventPump& DomainEvents::createPump(std::string_view name)
    {
        ownerCheck();
        return pumps_.emplace_back(EventPump::Badge{}, *this, std::string(name));
    }

    std::vector<ChannelDiag> DomainEvents::diagnostics() const
    {
        ownerCheck();
        std::vector<ChannelDiag> output;
        for (const auto& [type, channel] : channels_)
        {
            (void)type;
            channel->collectDiagnostics(output);
        }
        return output;
    }

    std::size_t EventPump::drainRound()
    {
        events_->ownerCheck();
        if (draining_)
            std::terminate();
        draining_ = true;
        std::size_t processed = 0u;
        for (const auto& entry : entries_)
            processed += entry.drain(*entry.channel, entry.per_pump);
        draining_ = false;
        return processed;
    }

    void EventPump::drain()
    {
        (void)drainRound();
    }

    void EventPump::drainUntilEmpty(std::size_t max_rounds)
    {
        for (std::size_t round = 0u; round < max_rounds; ++round)
        {
            if (drainRound() == 0u)
                return;
        }
    }
}
