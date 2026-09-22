#pragma once
#include <lux/engine/function/render/client/RenderUploadClient.hpp>
#include <lux/engine/function/render/client/RenderUploadSession.hpp>
#include <mutex>
namespace lux::render::detail
{
class UploadQueue final
{
  public:
    UploadQueue(std::size_t count, std::size_t bytes) : pending_(count), limit_(bytes)
    {
    }
    static lux::render::UploadSubmitNoReplyResult submit(
        void *owner, std::shared_ptr<lux::render::detail::PreparedUpload> packet) noexcept
    {
        auto &queue = *static_cast<UploadQueue *>(owner);
        std::lock_guard lock{queue.mutex_};
        if (!queue.accepting_)
        {
            return lux::cxx::unexpected(lux::render::ERenderUploadSubmitError::STOPPING);
        }
        if (queue.size_ == queue.pending_.size())
        {
            return lux::cxx::unexpected(lux::render::ERenderUploadSubmitError::QUEUE_FULL);
        }
        const auto bytes = packet->packet.accountedBytes();
        if (bytes > queue.limit_ - queue.bytes_)
        {
            return lux::cxx::unexpected(lux::render::ERenderUploadSubmitError::BYTE_BUDGET_EXHAUSTED);
        }
        queue.bytes_ += bytes;
        queue.pending_[(queue.head_ + queue.size_++) % queue.pending_.size()] = std::move(packet);
        return {};
    }
    void stop() noexcept
    {
        std::lock_guard lock{mutex_};
        accepting_ = false;
    }
    bool empty() const noexcept
    {
        std::lock_guard lock{mutex_};
        return size_ == 0;
    }
    std::size_t poll(lux::render::RenderUploadSession &uploads, bool stopped, std::size_t budget)
    {
        std::size_t processed{};
        while (processed < budget)
        {
            std::shared_ptr<lux::render::detail::PreparedUpload> packet;
            {
                std::lock_guard lock{mutex_};
                if (!size_)
                {
                    break;
                }
                packet = pending_[head_];
            }
            const auto accounted_bytes = packet->packet.accountedBytes();
            if (stopped)
            {
                static_cast<void>(packet->callback.settleFailure(
                    lux::render::renderError<lux::render::err::comm::ChannelStopping>()));
            }
            else
            {
                lux::render::ReplyDispatchCallback callback{
                    [packet](auto reply, const auto &record) { packet->callback(reply, record); },
                    [packet](auto error) { static_cast<void>(packet->callback.settleFailure(error)); }};
                const auto submitted =
                    packet->expected_reply_type == lux::render::kInvalidTypeId
                        ? uploads.trySubmitPreparedNoReply(packet->packet)
                        : uploads.trySubmitPrepared(packet->packet, packet->expected_reply_type, std::move(callback));
                if (!submitted)
                {
                    break;
                }
            }
            std::lock_guard lock{mutex_};
            bytes_ -= accounted_bytes;
            pending_[head_].reset();
            head_ = (head_ + 1) % pending_.size();
            --size_;
            ++processed;
        }
        return processed;
    }

  private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<lux::render::detail::PreparedUpload>> pending_;
    std::size_t limit_{}, head_{}, size_{}, bytes_{};
    bool accepting_{true};
};
} // namespace lux::render::detail
