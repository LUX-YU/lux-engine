#pragma once
#include <mutex>
#include <condition_variable>
#include "RenderCommTypes.hpp"
#include "BoundedSpscFrameRing.hpp"

namespace lux::render
{
    // -----------------------------------------------------------------------------
    //  Request / response packets stored in the triple buffer slots
    // -----------------------------------------------------------------------------
    template <std::size_t PayloadAlignment = 64>
    struct FrameProgram
    {
        using PayloadVector = std::vector<std::byte, AlignedAllocator<std::byte, PayloadAlignment>>;

        std::vector<CmdRecord>        commands;
        PayloadVector                 payload;
        std::vector<AttachmentRecord> attachments;

        FrameProgram() = default;
        FrameProgram(const FrameProgram&) = delete;
        FrameProgram& operator=(const FrameProgram&) = delete;
        FrameProgram(FrameProgram&&) = delete;
        FrameProgram& operator=(FrameProgram&&) = delete;

        ~FrameProgram()
        {
            clear_keep_capacity();
        }

        void reserve(const FrameMemoryHints& hints)
        {
            commands.reserve(hints.command_capacity);
            payload.reserve(hints.payload_capacity);
            attachments.reserve(hints.attachment_capacity);
        }

        void clear_keep_capacity()
        {
            for (auto& a : attachments)
            {
                a.reset();
            }
            attachments.clear();
            commands.clear();
            payload.clear();
        }
    };

    template <std::size_t PayloadAlignment = 64>
    struct FrameReplies
    {
        using PayloadVector = std::vector<std::byte, AlignedAllocator<std::byte, PayloadAlignment>>;

        std::vector<ReplyRecord> replies;
        PayloadVector            payload;

        FrameReplies() = default;
        FrameReplies(const FrameReplies&) = delete;
        FrameReplies& operator=(const FrameReplies&) = delete;
        FrameReplies(FrameReplies&&) = delete;
        FrameReplies& operator=(FrameReplies&&) = delete;

        void reserve(const FrameMemoryHints& hints)
        {
            replies.reserve(hints.reply_capacity);
            payload.reserve(hints.reply_payload_capacity);
        }

        void clear_keep_capacity() noexcept
        {
            replies.clear();
            payload.clear();
        }
    };

    struct RenderChannelSync
    {
        std::mutex              request_mutex{};
        std::condition_variable request_cv{};
        std::uint64_t           request_epoch{0};

        std::mutex              reply_mutex{};
        std::condition_variable reply_cv{};
        std::uint64_t           reply_epoch{0};

        bool                    stopping{false};

        void notifyRequestStateChanged()
        {
            {
                std::lock_guard lock(request_mutex);
                ++request_epoch;
            }
            request_cv.notify_all();
        }

        void notifyReplyStateChanged()
        {
            {
                std::lock_guard lock(reply_mutex);
                ++reply_epoch;
            }
            reply_cv.notify_all();
        }

        void requestStop()
        {
            {
                std::lock_guard lock_req(request_mutex);
                std::lock_guard lock_rep(reply_mutex);
                stopping = true;
                ++request_epoch;
                ++reply_epoch;
            }

            request_cv.notify_all();
            reply_cv.notify_all();
        }
    };

    template <std::size_t RequestAlignment = 64, std::size_t ReplyAlignment = 64>
    struct RenderProgramChannel
    {
        lux::cxx::BoundedSpscFrameRing<FrameProgram<RequestAlignment>, 4> requests{2};
        lux::cxx::BoundedSpscFrameRing<FrameReplies<ReplyAlignment>, 4>   responses{2};

        [[nodiscard]] static std::shared_ptr<RenderProgramChannel> create()
        {
            return std::make_shared<RenderProgramChannel>();
        }
    };
} // namespace lux::render
