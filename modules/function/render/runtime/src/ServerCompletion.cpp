#include "ServerImpl.hpp"

namespace lux::render
{
    void runtime_detail::CommandCompletion::deliver(execution::RunLoop::Work& work) noexcept
    {
        auto& slot = static_cast<CommandCompletion&>(work);
        slot.outbox->enqueue(slot.response_route_, std::move(slot.result_message_));
        delete &slot;
    }

    std::size_t RenderServer::Impl::drainCompletions(std::size_t budget) noexcept
    {
        const auto consumed = render_loop_.executeReady(budget);
        return consumed;
    }
} // namespace lux::render
