#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>


#include <lux/cxx/container/SparseSet.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/function/render/client/FrameProgram.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>

namespace lux::render
{
    // RequestId combines lane, generation and recycled slot index.
    template <std::size_t ReplyAlignment = 64>
    class ResponseCallbackStore
    {
    public:
        using Packet    = ReplyPacket<ReplyAlignment>;
        using Callback  = ReplyDispatchCallback;

        explicit ResponseCallbackStore(
            ERequestLane lane = ERequestLane::FRAME) noexcept
            : lane_(lane)
        {
        }

        RequestId registerCallback(Callback callback, TypeId expected_reply_type)
        {
            const std::uint32_t slot =
                entries_.emplace(Entry{std::move(callback), expected_reply_type});
            if (slot > kIndexMask)
                renderFatal("response callback store exceeds the RequestId slot limit");
            if (slot >= generations_.size())
            {
                generations_.resize(slot + 1, 0);
            }
            return packRequestId(lane_, generations_[slot], slot);
        }

        void cancel(RequestId request_id) noexcept
        {
            if (request_id == kInvalidRequestId ||
                unpackLane(request_id) != lane_)
                return;
            const auto slot = unpackIndex(request_id);
            if (entries_.contains(slot))
                releaseSlot(slot);
        }

        void setUnsolicitedHandler(TypeId reply_type, Callback handler)
        {
            unsolicited_[reply_type] = std::move(handler);
        }

        [[nodiscard]] std::uint64_t unroutedUnsolicited() const noexcept
        {
            return unrouted_unsolicited_;
        }

        [[nodiscard]] bool dispatch(const Packet& packet, const ReplyRecord& record)
        {
            if (record.request_id == kInvalidRequestId)
            {
                const auto handler = unsolicited_.find(record.type_id);
                if (handler == unsolicited_.end() || !handler->second)
                {
                    ++unrouted_unsolicited_;
                    return false;
                }
                handler->second(replyPacketView(packet), record);
                return true;
            }

            const std::uint32_t slot = unpackIndex(record.request_id);
            const std::uint8_t  gen  = unpackGeneration(record.request_id);

            if (unpackLane(record.request_id) != lane_)
            {
                ++unmatched_replies_;
                return false;
            }

            if (!entries_.contains(slot))
            {
                ++unmatched_replies_;
                return false;
            }

            if (slot >= generations_.size() || generations_[slot] != gen)
            {
                ++unmatched_replies_;
                return false;
            }

            Entry& entry = entries_.at(slot);
            const bool is_failure = (record.type_id == kReplyCommandFailedTypeId);
            if (!is_failure && entry.expected_reply_type != record.type_id)
            {
                Callback callback = std::move(entry.callback);
                const TypeId expected = entry.expected_reply_type;
                releaseSlot(slot);
                ++malformed_replies_;
                return callback.settleFailure(
                    renderError<err::comm::ReplyTypeMismatch>(
                        expected,
                        record.type_id
                    )
                );
            }

            // Release before invocation because continuations may re-enter.
            Callback cb = std::move(entry.callback);
            releaseSlot(slot);
            if (cb)
            {
                cb(replyPacketView(packet), record);
            }
            return true;
        }

        std::size_t dispatchAll(const Packet& packet)
        {
            std::size_t handled = 0;
            for (const ReplyRecord& record : packet.replies)
            {
                handled += dispatch(packet, record) ? 1u : 0u;
            }
            return handled;
        }

        [[nodiscard]] std::uint64_t malformedReplies() const noexcept
        {
            return malformed_replies_;
        }

        [[nodiscard]] std::uint64_t unmatchedReplies() const noexcept
        {
            return unmatched_replies_;
        }

        [[nodiscard]] std::size_t pendingCallbacks() const noexcept
        {
            return entries_.size();
        }

    private:
        struct Entry
        {
            Callback callback{};
            TypeId   expected_reply_type{kInvalidTypeId};
        };

        static constexpr RequestId kIndexMask      = 0x00FFFFFFu;
        static constexpr RequestId kGenerationMask = 0x3Fu;

        static constexpr RequestId packRequestId(
            ERequestLane lane, std::uint8_t gen, std::uint32_t slot) noexcept
        {
            return (static_cast<RequestId>(lane) << 30)
                | ((static_cast<RequestId>(gen) & kGenerationMask) << 24)
                | (slot & kIndexMask);
        }
        static constexpr std::uint32_t unpackIndex(RequestId id) noexcept
        {
            return id & kIndexMask;
        }
        static constexpr std::uint8_t unpackGeneration(RequestId id) noexcept
        {
            return static_cast<std::uint8_t>((id >> 24) & kGenerationMask);
        }
        static constexpr ERequestLane unpackLane(RequestId id) noexcept
        {
            return static_cast<ERequestLane>((id >> 30) & 0x3u);
        }

        void releaseSlot(std::uint32_t slot)
        {
            entries_.erase(slot);
            if (slot < generations_.size())
            {
                generations_[slot] = static_cast<std::uint8_t>(
                    (generations_[slot] + 1u) & kGenerationMask);
            }
        }

        lux::cxx::OffsetAutoSparseSet<RequestId, Entry> entries_{};
        std::vector<std::uint8_t>                       generations_{};
        std::unordered_map<TypeId, Callback>            unsolicited_{};
        std::uint64_t                                   unrouted_unsolicited_{0};
        std::uint64_t                                   malformed_replies_{0};
        std::uint64_t                                   unmatched_replies_{0};
        ERequestLane                                    lane_{ERequestLane::FRAME};
    };

    template <
        class Packet,
        bool AllowBorrowed,
        std::size_t PayloadAlignment = 64,
        std::size_t ReplyAlignment = 64>
    class CommandPacketBuilder
    {
    public:
        template<std::size_t RequestAlignment, std::size_t OtherReplyAlignment>
        friend class RenderClient;

        explicit CommandPacketBuilder(Packet& dst) noexcept
            : dst_(dst)
        {
        }

        CommandPacketBuilder(
            Packet& dst,
            ResponseCallbackStore<ReplyAlignment>& callback_store
        ) noexcept
            : dst_(dst), callback_store_(&callback_store)
        {
        }

        using MemoryHints = typename Packet::MemoryHints;

        void begin(const MemoryHints& hints = {})
        {
            dst_.clear_keep_capacity();
            dst_.reserve(hints);
            valid_ = true;
            prepared_reply_type_ = kInvalidTypeId;
        }

        [[nodiscard]] bool valid() const noexcept { return valid_; }

        [[nodiscard]] std::size_t commandCount() const noexcept
        {
            if constexpr (requires { dst_.commands.size(); })
                return dst_.commands.size();
            else
                return dst_.has_command ? 1u : 0u;
        }

        template <FrameBlobPayload T>
        void push(OpCode opcode, TypeId type_id, const T& payload, std::uint16_t flags = 0)
        {
            static_assert(!command_has_reply_v<T>, "This command type declares a reply. Use pushWithReply().");
            pushUnary(opcode, type_id, payload, flags, kInvalidRequestId);
        }

        template <FrameBlobPayload T, typename Func>
        RequestId pushWithReply(OpCode opcode, TypeId type_id, const T& payload, Func&& callback, std::uint16_t flags = 0)
        {
            static_assert(command_has_reply_v<T>, "This command type does not declare a reply. Use push().");
            if (callback_store_ == nullptr)
            {
                valid_ = false;
                return kInvalidRequestId;
            }
            auto request_id = callback_store_->registerCallback(
                std::move(callback),
                CommandTraits<T>::reply_type_id);

            pushUnary(
                opcode, 
                type_id, 
                payload, 
                flags | static_cast<std::uint16_t>(CmdFlags::ExpectsReply), request_id
            );

            return request_id;
        }

        template <FrameBlobPayload T, typename Func>
        RequestId pushResource(TypeId type_id, const T& payload, Func&& callback, std::uint16_t flags = 0)
        {
            static_assert(command_has_reply_v<T>,
                "This resource command type does not declare a reply. Use pushResource().");
            if (callback_store_ == nullptr)
            {
                valid_ = false;
                return kInvalidRequestId;
            }
            auto request_id = callback_store_->registerCallback(
                std::move(callback),
                CommandTraits<T>::reply_type_id);

            pushUnary(
                opcodes::ResourceOp, 
                type_id, 
                payload, 
                flags | static_cast<std::uint16_t>(CmdFlags::ExpectsReply), request_id
            );

            return request_id;
        }

        template <FrameBlobPayload T>
        void pushBulk(TypeId type_id, std::span<const T> items, 
            std::uint16_t flags = 0, RequestId request_id = kInvalidRequestId)
        {
            static_assert(alignof(T) <= PayloadAlignment,
                "Payload type alignment exceeds payload blob alignment. Raise PayloadAlignment.");

            if (items.empty())
            {
                return;
            }

            const std::uint32_t offset = appendBytes(items.data(), items.size_bytes(), alignof(T));
            appendRecord(opcodes::BulkData, type_id, offset, narrowU32(items.size_bytes()), flags, request_id);
        }

        [[nodiscard]] BlobRef pushBlob(std::span<const std::byte> bytes,
                                       std::size_t alignment = alignof(std::max_align_t))
        {
            const std::uint32_t offset = appendBytes(bytes.data(), bytes.size(), alignment);
            return BlobRef{
                .offset = offset,
                .size = narrowU32(bytes.size()),
            };
        }

        template <typename T, typename... Args>
        std::uint32_t emplaceAttachment(TypeId type_id, Args&&... args)
        {
            auto destroy = [](void* p) noexcept
            {
                delete static_cast<T*>(p);
            };

            T* obj = new T(std::forward<Args>(args)...);
            dst_.attachments.emplace_back(
                type_id,
                obj,
                sizeof(T),
                sizeof(T),
                destroy
            );
            return narrowU32(dst_.attachments.size() - 1);
        }

        /// Build an owning upload command before it reaches the sole upload
        /// endpoint owner.  The coordinator assigns the RequestId and installs
        /// the reply callback immediately before publishing the packet to the
        /// SPSC channel.  Consequently producer threads never touch the
        /// endpoint's callback table.
        template <FrameBlobPayload T>
        void pushPreparedResource(
            TypeId type_id,
            const T& payload,
            std::uint16_t flags = 0)
        {
            static_assert(
                command_has_reply_v<T>,
                "Prepared upload commands must declare a reply");
            if (prepared_reply_type_ != kInvalidTypeId)
            {
                valid_ = false;
                return;
            }
            prepared_reply_type_ = CommandTraits<T>::reply_type_id;
            pushUnary(
                opcodes::ResourceOp,
                type_id,
                payload,
                flags | static_cast<std::uint16_t>(CmdFlags::ExpectsReply),
                kInvalidRequestId);
        }

        [[nodiscard]] TypeId preparedReplyType() const noexcept
        {
            return prepared_reply_type_;
        }

        /// Attach a borrowed (non-owning) byte range to the current frame.
        ///
        /// LIFETIME(契约归真,2026-08-04):borrowed 内存必须存活到**渲染服务端
        /// 消费完这一帧**(命令被 dispatch 执行完)。⚠️ 不是 submitFrame() 返回
        /// —— submit 只是把帧发布进 SPSC 环,服务端在另一线程随后才读;老措辞
        /// 「until submitFrame」按字面执行就是 UAF(旧上传池的
        /// 修复注释、SkinningOperation.hpp 的 blob 化都是这条的伤疤)。
        /// 「消费完」对调用方**不可观测**,唯一可靠的代理是该命令的回复到达
        /// (回复必然晚于消费)—— 没有回复的命令没有安全的借用判据,别用借用。
        /// 新代码一律选所有权路径:pushOwnedBytesCopy / pushSharedBytes。
        ///
        /// THREAD SAFETY: The caller is responsible for ensuring no concurrent
        /// writes to the borrowed region while the frame is in flight.
        [[nodiscard]] std::uint32_t pushBorrowedBytesAttachment(const std::byte* data, std::uint32_t size,
            TypeId attachment_type = attachment_types::BorrowedBytes)
            requires AllowBorrowed
        {
            return emplaceAttachment<BorrowedBytesAttachment>(
                attachment_type,
                BorrowedBytesAttachment{
                    .data = data,
                    .size = size,
                });
        }

        /// Attach a borrowed (non-owning) typed object pointer to the current frame.
        ///
        /// LIFETIME:存活到服务端消费完这一帧(见 pushBorrowedBytesAttachment
        /// 的契约归真注释 —— 不是 submitFrame)。现存唯一消费者是 ImGui 快照
        /// (UIRenderFrameSession 的 4 帧快照环按深度兜住借用期)。
        template <typename T>
        [[nodiscard]] std::uint32_t pushBorrowedObject(TypeId attachment_type, const T* obj)
            requires AllowBorrowed
        {
            dst_.attachments.emplace_back(
                attachment_type,
                const_cast<void*>(static_cast<const void*>(obj)),
                sizeof(T),
                0,
                nullptr
            );
            return narrowU32(dst_.attachments.size() - 1);
        }

        /// pushBorrowedObject 的无类型变体:object 直接指向 @p size 字节的裸块
        /// (服务端 handler 直读 attachment.object/object_size —— 与
        /// emplaceAttachment<Config> 的读法同形,只是长度来自运行期)。
        /// LIFETIME 同上:到服务端消费完(不是 submitFrame)。现存唯一消费者
        /// addFeatureRaw 借的是进程常驻的 attach 计划,天然满足。
        [[nodiscard]] std::uint32_t pushBorrowedRaw(TypeId attachment_type,
                                                    const void* data, std::uint32_t size)
            requires AllowBorrowed
        {
            dst_.attachments.emplace_back(
                attachment_type,
                const_cast<void*>(data),
                size,
                0,
                nullptr
            );
            return narrowU32(dst_.attachments.size() - 1);
        }

        /// Attach a shared-lifetime byte range to the current frame.
        ///
        /// The backing memory can outlive the frame packet through @p owner,
        /// making this safe for async worker pipelines.
        [[nodiscard]] std::uint32_t pushSharedBytesAttachment(
            std::shared_ptr<const void> owner,
            const std::byte* data,
            std::uint32_t size,
            TypeId attachment_type = attachment_types::OwnedBytes,
            std::size_t accounted_size =
                std::numeric_limits<std::size_t>::max())
        {
            const auto index = emplaceAttachment<OwnedBytesAttachment>(
                attachment_type,
                OwnedBytesAttachment{
                    .owner = std::move(owner),
                    .data = data,
                    .size = size,
                });
            dst_.attachments[index].accounted_size =
                accounted_size == std::numeric_limits<std::size_t>::max()
                    ? size
                    : accounted_size;
            return index;
        }

        [[nodiscard]] ExternalDataRef pushSharedBytes(
            std::shared_ptr<const void> owner,
            const std::byte* data,
            std::uint32_t size,
            TypeId attachment_type = attachment_types::OwnedBytes,
            std::size_t accounted_size =
                std::numeric_limits<std::size_t>::max())
        {
            const std::uint32_t attachment_index =
                pushSharedBytesAttachment(
                    std::move(owner),
                    data,
                    size,
                    attachment_type,
                    accounted_size
                );

            return ExternalDataRef{
                .attachment_index = attachment_index,
                .offset = 0,
                .size = size,
            };
        }

        [[nodiscard]] ExternalDataRef pushSharedBytes(
            const lux::cxx::SharedBytes<>& bytes,
            TypeId attachment_type = attachment_types::OwnedBytes)
        {
            if (bytes.size() > UINT32_MAX)
            {
                valid_ = false;
                return {};
            }
            if (bytes.empty())
                return pushSharedBytes(
                    {},
                    nullptr,
                    0u,
                    attachment_type);

            auto owner =
                std::make_shared<const lux::cxx::SharedBytes<>>(bytes);
            return pushSharedBytes(
                std::shared_ptr<const void>{owner, owner->data()},
                owner->data(),
                static_cast<std::uint32_t>(bytes.size()),
                attachment_type);
        }

        /// Copy bytes into shared-owned storage and attach that storage.
        [[nodiscard]] ExternalDataRef pushOwnedBytesCopy(
            const std::byte* data,
            std::uint32_t size,
            TypeId attachment_type = attachment_types::OwnedBytes)
        {
            auto storage = std::make_shared<std::vector<std::byte>>(size);
            if (size > 0)
            {
                std::memcpy(storage->data(), data, size);
            }

            return pushSharedBytes(
                std::static_pointer_cast<const void>(storage),
                storage->data(),
                size,
                attachment_type);
        }

    private:
        template <FrameBlobPayload T>
        void pushUnary(OpCode opcode, TypeId type_id, const T& payload, std::uint16_t flags, RequestId request_id)
        {
            static_assert(alignof(T) <= PayloadAlignment,
                "Payload type alignment exceeds payload blob alignment. Raise PayloadAlignment.");

            const std::uint32_t offset = appendBytes(&payload, sizeof(T), alignof(T));
            appendRecord(opcode, type_id, offset, narrowU32(sizeof(T)), flags, request_id);
        }

        template <typename T>
        std::uint32_t appendBytes(const T* src, std::size_t byte_count, std::size_t alignment)
        {
            if (alignment == 0 || (alignment & (alignment - 1)) != 0)
            {
                valid_ = false;
                return 0;
            }

            const std::size_t old_size = dst_.payload.size();
            const std::size_t offset   = alignUp(old_size, alignment);
            const std::size_t new_size = offset + byte_count;

            if (new_size > dst_.payload.size())
            {
                dst_.payload.resize(new_size);
            }

            if (byte_count != 0)
            {
                std::memcpy(dst_.payload.data() + offset, src, byte_count);
            }

            return narrowU32(offset);
        }

        void appendRecord(OpCode opcode, TypeId type_id, std::uint32_t payload_offset,
            std::uint32_t payload_size, std::uint16_t flags, RequestId request_id)
        {
            const CmdRecord record{
                .opcode = opcode,
                .reserved0 = 0,
                .flags = flags,
                .type_id = type_id,
                .payload_offset = payload_offset,
                .payload_size = payload_size,
                .request_id = request_id,
            };
            if constexpr (requires { dst_.commands.push_back(record); })
                dst_.commands.push_back(record);
            else if (!dst_.setCommand(record))
                valid_ = false;
        }

        static std::uint32_t narrowU32(std::size_t value)
        {
            if (value > std::numeric_limits<std::uint32_t>::max())
                renderFatal("command packet exceeds the 32-bit wire limit");
            return static_cast<std::uint32_t>(value);
        }

    private:
        Packet&                                dst_;
        ResponseCallbackStore<ReplyAlignment>* callback_store_{nullptr};
        bool                                   valid_{true};
        TypeId                                 prepared_reply_type_{
            kInvalidTypeId};
    };

    template <std::size_t PayloadAlignment = 64, std::size_t ReplyAlignment = 64>
    using FrameProgramBuilder = CommandPacketBuilder<
        FrameProgram<PayloadAlignment>,
        true,
        PayloadAlignment,
        ReplyAlignment>;

    template <std::size_t PayloadAlignment = 64, std::size_t ReplyAlignment = 64>
    using SingleOperationBuilder = CommandPacketBuilder<
        OperationPacket<PayloadAlignment>,
        false,
        PayloadAlignment,
        ReplyAlignment>;

    template <std::size_t RequestAlignment = 64, std::size_t ReplyAlignment = 64>
    class RenderClient
    {
    public:
        using Channel       = RenderFrameChannel<RequestAlignment, ReplyAlignment>;
        using CallbackStore = ResponseCallbackStore<ReplyAlignment>;
        using Builder       = FrameProgramBuilder<RequestAlignment, ReplyAlignment>;
        using StageProgram  = FrameProgram<RequestAlignment>;

        explicit RenderClient(std::shared_ptr<Channel> channel,
                              std::shared_ptr<RenderChannelSync> sync,
                              ERequestLane lane = ERequestLane::FRAME)
            : channel_(std::move(channel))
            , sync_(std::move(sync))
            , callbacks_(lane)
            , staging_builder_(staging_program_, callbacks_)
        {
        }

        /// 登记一个无请求回复的处理器(按回复 type_id 路由)。渲染线程自发推送的
        /// 回复没有等待它的请求可以认领,此前一律被丢弃 —— 通道早就在跑,缺的只是
        /// 客户端这一侧的路由。表里没登记的 type_id 仍然丢弃,不设处理器行为不变。
        ///
        /// 语义化的封装见 RenderFrameSession::setErrorEventHandler。
        void setUnsolicitedHandler(TypeId reply_type, typename CallbackStore::Callback handler)
        {
            callbacks_.setUnsolicitedHandler(reply_type, std::move(handler));
        }

        /// 见 CallbackStore::unroutedUnsolicited —— 装配期漏登记处理器的自证。
        [[nodiscard]] std::uint64_t unroutedUnsolicited() const noexcept
        {
            return callbacks_.unroutedUnsolicited();
        }

        // -----------------------------------------------------------------
        //  Reply phase
        // -----------------------------------------------------------------

        std::size_t pumpReplies()
        {
            std::size_t acquired = 0u;
            while (channel_->responses.tryAcquireRead())
            {
                callbacks_.dispatchAll(channel_->responses.currentRead());
                sync_->notifyReplyConsumed();
                ++acquired;
            }
            return acquired;
        }

        bool waitAndPumpReplies()
        {
            if (channel_->responses.tryAcquireRead())
            {
                callbacks_.dispatchAll(channel_->responses.currentRead());
                sync_->notifyReplyConsumed();
                // Drain any additional responses (e.g. deferred-reply frames
                // published by flushDeferredRepliesOnly on the server).
                (void)pumpReplies();
                return true;
            }

            const std::uint64_t observed =
                sync_->reply_epoch.load(std::memory_order_acquire);

            if (channel_->responses.tryAcquireRead())
            {
                callbacks_.dispatchAll(channel_->responses.currentRead());
                sync_->notifyReplyConsumed();
                (void)pumpReplies();
                return true;
            }

            if (sync_->isStopping())
            {
                return false;
            }
            sync_->reply_epoch.wait(observed, std::memory_order_acquire);
            if (sync_->isStopping())
                return false;
            (void)pumpReplies();
            return true;
        }

        // -----------------------------------------------------------------
        //  Recording phase
        // -----------------------------------------------------------------

        bool beginFrame(const FrameMemoryHints& hints = {})
        {
            // requestStop closes admission for every lane.  Already staged
            // packets may still be drained by retryPendingSubmit(), but a
            // producer must never create a new packet after the endpoint has
            // entered its stopping state.
            if (sync_->isStopping() || recording_ || staged_ready_ ||
                pending_publish_)
            {
                return false;
            }

            staging_builder_.begin(hints);
            recording_ = true;
            return true;
        }

        [[nodiscard]] bool isRecording() const noexcept
        {
            return recording_;
        }

        Builder& builder() noexcept
        {
            if (!recording_)
                renderFatal("RenderClient::builder requires an open frame");
            return staging_builder_;
        }

        const Builder& builder() const noexcept
        {
            if (!recording_)
                renderFatal("RenderClient::builder requires an open frame");
            return staging_builder_;
        }

        // -----------------------------------------------------------------
        //  Submission phase
        // -----------------------------------------------------------------

        bool trySubmitFrame()
        {
            // Finalize recording into staged state.
            if (recording_)
            {
                recording_ = false;
                staged_ready_ = true;
            }

            return retryPendingSubmit();
        }

        bool retryPendingSubmit()
        {
            // Case 1: frame already lives in a ring write slot, only publish is pending.
            if (pending_publish_)
            {
                if (!publishRequest())
                {
                    return false;
                }

                pending_publish_ = false;
                sync_->notifyRequestStateChanged();
                return true;
            }

            // Case 2: nothing staged.
            if (!staged_ready_)
            {
                return true;
            }

            // Case 3: acquire a real request slot and move staged packet into it.
            FrameProgram<RequestAlignment>* slot = beginRequestWrite();
            if (!slot)
            {
                return false;
            }

            swapFramePrograms(*slot, staging_program_);
            staged_ready_ = false;
            pending_publish_ = true;

            if (!publishRequest())
            {
                return false;
            }

            pending_publish_ = false;
            sync_->notifyRequestStateChanged();
            return true;
        }

        using FrameProgressToken = std::uint64_t;

        [[nodiscard]] FrameProgressToken observeProgress() const noexcept
        {
            return sync_->work_epoch.load(std::memory_order_acquire);
        }

        void waitForProgress(FrameProgressToken observed) const noexcept
        {
            sync_->work_epoch.wait(observed, std::memory_order_acquire);
        }

        void notifyProgress() noexcept
        {
            sync_->notifyRequestStateChanged();
        }

        [[nodiscard]] bool isStopping() const noexcept
        {
            return sync_->isStopping();
        }

        [[nodiscard]] std::shared_ptr<RenderChannelSync>
        progressDomain() const noexcept
        {
            return sync_;
        }

        // -----------------------------------------------------------------
        //  Misc
        // -----------------------------------------------------------------

        void requestStop()
        {
            sync_->requestStop();
        }

    private:
        FrameProgram<RequestAlignment>* beginRequestWrite()
        {
            return channel_->requests.tryBeginWrite();
        }

        bool publishRequest()
        {
            return channel_->requests.publishWrite();
        }

        static void swapFramePrograms(FrameProgram<RequestAlignment>& a, FrameProgram<RequestAlignment>& b) noexcept
        {
            using std::swap;
            swap(a.commands,     b.commands);
            swap(a.payload,      b.payload);
            swap(a.attachments,  b.attachments);
        }

    private:
        std::shared_ptr<Channel>           channel_;
        std::shared_ptr<RenderChannelSync> sync_;
        CallbackStore                      callbacks_{};

        StageProgram                       staging_program_{};
        Builder                            staging_builder_;

        bool                               recording_{false};
        bool                               staged_ready_{false};
        bool                               pending_publish_{false};
    };

    using GeneralRenderClient = RenderClient<>;
} // namespace lux::render
