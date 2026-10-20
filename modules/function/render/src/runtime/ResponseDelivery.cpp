#include <lux/engine/render/runtime/ResponseDelivery.hpp>

namespace lux::render::runtime_detail
{
    platform::ipc::IpcMessage& completionResult(ResponseDelivery& record) noexcept
    {
        return record.result_message_;
    }

    void publishCompletion(ResponseDelivery& record) noexcept
    {
        auto* loop = record.loop;
        loop->post(record);
        // No record access after publication. RunLoop protects the wakeup/publisher tail.
    }

    void ResponseDelivery::deliver(execution::RunLoop::Work& work) noexcept
    {
        auto& slot = static_cast<ResponseDelivery&>(work);
        slot.outbox->enqueue(slot.response_route_, std::move(slot.result_message_));
        delete &slot;
    }

} // namespace lux::render::runtime_detail
