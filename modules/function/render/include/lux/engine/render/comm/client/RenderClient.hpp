#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <chrono>
#include <mutex>
#include <condition_variable>

#include <lux/cxx/container/SparseSet.hpp>
#include <lux/engine/render/comm/FrameProgram.hpp>

namespace lux::render
{
    // Stores reply callbacks keyed by a packed RequestId. The wire RequestId
    // packs an 8-bit generation (high bits) with a 24-bit slot index (low bits):
    //   - the slot index is the OffsetAutoSparseSet key (recycled on release);
    //   - the generation is bumped each time a slot is released, so a stale or
    //     duplicate reply addressed to a slot since reused for a DIFFERENT
    //     request is detected and dropped (H6).
    // Each entry also records its expected reply type_id so dispatch() can reject
    // a mis-routed reply instead of memcpy-ing a wrong-shaped payload (H6).
    // A per-frame registration batch lets cancelFrame() roll back callbacks that
    // were recorded but never sent (H5).
    template <std::size_t ReplyAlignment = 64>
    class ResponseCallbackStore
    {
    public:
        using Packet    = FrameReplies<ReplyAlignment>;
        using Callback  = std::function<void(const Packet&, const ReplyRecord&)>;

        RequestId registerCallback(Callback callback, TypeId expected_reply_type)
        {
            const std::uint32_t slot =
                entries_.emplace(Entry{std::move(callback), expected_reply_type});
            assert(slot <= kIndexMask && "ResponseCallbackStore: slot index overflow (>24 bits)");
            if (slot >= generations_.size())
            {
                generations_.resize(slot + 1, 0);
            }
            batch_slots_.push_back(slot);
            return packRequestId(generations_[slot], slot);
        }

        [[nodiscard]] bool dispatch(const Packet& packet, const ReplyRecord& record)
        {
            if (record.request_id == kInvalidRequestId)
            {
                return false;
            }

            const std::uint32_t slot = unpackIndex(record.request_id);
            const std::uint8_t  gen  = unpackGeneration(record.request_id);

            if (!entries_.contains(slot))
            {
                return false;
            }

            if (slot >= generations_.size() || generations_[slot] != gen)
            {
                // Stale/duplicate reply for a slot that was recycled for a
                // different request — drop it rather than mis-deliver.
                return false;
            }

            Entry& entry = entries_.at(slot);
            if (entry.expected_reply_type != record.type_id)
            {
                // Mis-routed reply: the type does not match what this request
                // expects. Drop + log instead of memcpy-ing a wrong-shaped
                // payload into the caller's State. Keep the entry pending in
                // case the correct reply still arrives.
                std::fprintf(stderr,
                    "[ResponseCallbackStore] reply type mismatch on request slot %u: "
                    "expected type_id %u, got %u (reply dropped)\n",
                    slot,
                    static_cast<unsigned>(entry.expected_reply_type),
                    static_cast<unsigned>(record.type_id));
                return false;
            }

            // Move the callback out and release the slot BEFORE invoking it. The
            // continuation may re-enter registerCallback(), whose entries_.emplace
            // can reallocate the sparse-set's dense storage and destroy `entry`
            // (and the std::function's captures) mid-call — a use-after-free on the
            // executing functor. A local copy + early release makes the invocation
            // independent of the store's state. (review P1#3)
            Callback cb = std::move(entry.callback);
            releaseSlot(slot);
            if (cb)
            {
                cb(packet, record);
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

        [[nodiscard]] bool contains(RequestId request_id) const
        {
            return entries_.contains(unpackIndex(request_id));
        }

        void clear()
        {
            // Bump every live slot's generation so an in-flight reply to a
            // cleared request can no longer match once slots are reused.
            for (std::uint32_t slot = 0; slot < generations_.size(); ++slot)
            {
                if (entries_.contains(slot))
                {
                    ++generations_[slot];
                }
            }
            entries_.clear();
            batch_slots_.clear();
        }

        // ── H5: per-frame registration batch ───────────────────────────────
        // beginBatch() at beginFrame(); commitBatch() once the frame is staged
        // for submission (callbacks are now genuinely in flight); rollbackBatch()
        // on cancelFrame() to release callbacks that were recorded but never sent.
        void beginBatch() noexcept   { batch_slots_.clear(); }
        void commitBatch() noexcept  { batch_slots_.clear(); }
        void rollbackBatch()
        {
            for (std::uint32_t slot : batch_slots_)
            {
                if (entries_.contains(slot))
                {
                    releaseSlot(slot);
                }
            }
            batch_slots_.clear();
        }

    private:
        struct Entry
        {
            Callback callback{};
            TypeId   expected_reply_type{kInvalidTypeId};
        };

        static constexpr RequestId kIndexMask = 0x00FFFFFFu;

        static constexpr RequestId packRequestId(std::uint8_t gen, std::uint32_t slot) noexcept
        {
            return (static_cast<RequestId>(gen) << 24) | (slot & kIndexMask);
        }
        static constexpr std::uint32_t unpackIndex(RequestId id) noexcept
        {
            return id & kIndexMask;
        }
        static constexpr std::uint8_t unpackGeneration(RequestId id) noexcept
        {
            return static_cast<std::uint8_t>(id >> 24);
        }

        void releaseSlot(std::uint32_t slot)
        {
            entries_.erase(slot);
            if (slot < generations_.size())
            {
                ++generations_[slot]; // next reuse of this slot gets a new gen
            }
        }

        lux::cxx::OffsetAutoSparseSet<RequestId, Entry> entries_{};
        std::vector<std::uint8_t>                       generations_{};
        std::vector<std::uint32_t>                      batch_slots_{};
    };

    template <std::size_t PayloadAlignment = 64, std::size_t ReplyAlignment = 64>
    class FrameProgramBuilder
    {
    public:
        template<std::size_t RequestAlignment, std::size_t OtherReplyAlignment>
        friend class RenderClient;

        FrameProgramBuilder(FrameProgram<PayloadAlignment>& dst, ResponseCallbackStore<ReplyAlignment>& callback_store) noexcept
            : dst_(dst), callback_store_(callback_store)
        {
        }

        void begin(const FrameMemoryHints& hints = {})
        {
            dst_.clear_keep_capacity();
            dst_.reserve(hints);
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
            auto request_id = callback_store_.registerCallback(std::move(callback), CommandTraits<T>::reply_type_id);

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

            auto request_id = callback_store_.registerCallback(std::move(callback), CommandTraits<T>::reply_type_id);

            pushUnary(
                opcodes::ResourceOp, 
                type_id, 
                payload, 
                flags | static_cast<std::uint16_t>(CmdFlags::ExpectsReply), request_id
            );

            return request_id;
        }

        template <FrameBlobPayload T, typename Func>
        RequestId pushFeature(TypeId type_id, const T& payload, Func&& callback, std::uint16_t flags = 0)
        {
            static_assert(command_has_reply_v<T>,
                "This feature command type does not declare a reply. Use pushFeature().");
            
            auto request_id = callback_store_.registerCallback(std::move(callback), CommandTraits<T>::reply_type_id);
            pushUnary(
                opcodes::CommandOp, 
                type_id, 
                payload, 
                flags | static_cast<std::uint16_t>(CmdFlags::ExpectsReply), request_id
            );

            return request_id;
        }

        template <FrameBlobPayload T>
        void pushDebug(TypeId type_id, const T& payload, std::uint16_t flags = 0)
        {
            static_assert(!command_has_reply_v<T>,
                "Debug commands should not use pushDebug() with reply semantics. Use pushWithReply() if needed.");

            pushUnary(opcodes::Debug, type_id, payload, flags, kInvalidRequestId);
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

        [[nodiscard]] StringRef pushString(std::string_view s)
        {
            const auto* bytes = reinterpret_cast<const std::byte*>(s.data());
            return pushBlob(std::span<const std::byte>{bytes, s.size()}, alignof(char));
        }

        template <typename T, typename... Args>
        std::uint32_t emplaceAttachment(TypeId type_id, Args&&... args)
        {
            auto destroy = [](void* p) noexcept
            {
                delete static_cast<T*>(p);
            };

            T* obj = new T(std::forward<Args>(args)...);
            dst_.attachments.push_back(AttachmentRecord{
                .type_id = type_id,
                .object = obj,
                .object_size = sizeof(T),
                .destroy = destroy,
            });
            return narrowU32(dst_.attachments.size() - 1);
        }

        template <typename T>
        std::uint32_t pushOwnedAttachment(TypeId type_id, std::unique_ptr<T> obj)
        {
            T* raw = obj.release();
            dst_.attachments.push_back(AttachmentRecord{
                .type_id = type_id,
                .object = raw,
                .object_size = sizeof(T),
                .destroy = [](void* p) noexcept { delete static_cast<T*>(p); },
            });
            return narrowU32(dst_.attachments.size() - 1);
        }

        /// Attach a borrowed (non-owning) byte range to the current frame.
        ///
        /// LIFETIME: The caller MUST keep the pointed-to memory valid and
        /// unmodified from this call until submitFrame() returns.  The
        /// render server may read the data asynchronously on another thread
        /// during frame processing.
        ///
        /// THREAD SAFETY: The caller is responsible for ensuring no concurrent
        /// writes to the borrowed region while the frame is in flight.
        [[nodiscard]] std::uint32_t pushBorrowedBytesAttachment(const std::byte* data, std::uint32_t size, 
            TypeId attachment_type = attachment_types::BorrowedBytes)
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
        /// LIFETIME: The caller MUST keep the pointed-to object alive and
        /// unmodified from this call until submitFrame() returns.
        template <typename T>
        [[nodiscard]] std::uint32_t pushBorrowedObject(TypeId attachment_type, const T* obj)
        {
            dst_.attachments.push_back(AttachmentRecord{
                .type_id      = attachment_type,
                .object       = const_cast<void*>(static_cast<const void*>(obj)),
                .object_size  = sizeof(T),
                .destroy      = nullptr,   // non-owning — no destroy
            });
            return narrowU32(dst_.attachments.size() - 1);
        }

        [[nodiscard]] ExternalDataRef makeExternalDataRef(std::uint32_t attachment_index, 
            std::uint32_t offset, std::uint32_t size) const
        {
            return ExternalDataRef{
                .attachment_index = attachment_index,
                .offset = offset,
                .size = size,
            };
        }

        [[nodiscard]] ExternalDataRef pushBorrowedBytes(const std::byte* data, std::uint32_t size, 
            TypeId attachment_type = attachment_types::BorrowedBytes)
        {
            const std::uint32_t attachment_index =
                pushBorrowedBytesAttachment(data, size, attachment_type);

            return ExternalDataRef{
                .attachment_index = attachment_index,
                .offset = 0,
                .size = size,
            };
        }

        /// Attach a shared-lifetime byte range to the current frame.
        ///
        /// The backing memory can outlive the frame packet through @p owner,
        /// making this safe for async worker pipelines.
        [[nodiscard]] std::uint32_t pushSharedBytesAttachment(
            std::shared_ptr<const void> owner,
            const std::byte* data,
            std::uint32_t size,
            TypeId attachment_type = attachment_types::OwnedBytes)
        {
            return emplaceAttachment<OwnedBytesAttachment>(
                attachment_type,
                OwnedBytesAttachment{
                    .owner = std::move(owner),
                    .data = data,
                    .size = size,
                });
        }

        [[nodiscard]] ExternalDataRef pushSharedBytes(
            std::shared_ptr<const void> owner,
            const std::byte* data,
            std::uint32_t size,
            TypeId attachment_type = attachment_types::OwnedBytes)
        {
            const std::uint32_t attachment_index =
                pushSharedBytesAttachment(std::move(owner), data, size, attachment_type);

            return ExternalDataRef{
                .attachment_index = attachment_index,
                .offset = 0,
                .size = size,
            };
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
            assert(alignment > 0);

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
            dst_.commands.push_back(CmdRecord{
                .opcode = opcode,
                .reserved0 = 0,
                .flags = flags,
                .type_id = type_id,
                .payload_offset = payload_offset,
                .payload_size = payload_size,
                .request_id = request_id,
            });
        }

        static std::uint32_t narrowU32(std::size_t value)
        {
            assert(value <= std::numeric_limits<std::uint32_t>::max());
            return static_cast<std::uint32_t>(value);
        }

    private:
        FrameProgram<PayloadAlignment>&         dst_;
        ResponseCallbackStore<ReplyAlignment>&  callback_store_;
    };

    template <std::size_t RequestAlignment = 64, std::size_t ReplyAlignment = 64>
    class RenderClient
    {
    public:
        using Channel       = RenderProgramChannel<RequestAlignment, ReplyAlignment>;
        using CallbackStore = ResponseCallbackStore<ReplyAlignment>;
        using Builder       = FrameProgramBuilder<RequestAlignment, ReplyAlignment>;
        using StageProgram  = FrameProgram<RequestAlignment>;

        explicit RenderClient(std::shared_ptr<Channel> channel,
                              std::shared_ptr<RenderChannelSync> sync)
            : channel_(std::move(channel))
            , sync_(std::move(sync))
            , staging_builder_(staging_program_, callbacks_)
        {
        }

        // -----------------------------------------------------------------
        //  Reply phase
        // -----------------------------------------------------------------

        void pumpReplies()
        {
            while (channel_->responses.tryAcquireRead())
            {
                callbacks_.dispatchAll(channel_->responses.currentRead());
                sync_->notifyReplyStateChanged();
            }
        }

        bool waitAndPumpReplies()
        {
            if (channel_->responses.tryAcquireRead())
            {
                callbacks_.dispatchAll(channel_->responses.currentRead());
                sync_->notifyReplyStateChanged();
                // Drain any additional responses (e.g. deferred-reply frames
                // published by flushDeferredRepliesOnly on the server).
                pumpReplies();
                return true;
            }

            std::unique_lock lock(sync_->reply_mutex);
            const std::uint64_t observed = sync_->reply_epoch;

            if (channel_->responses.tryAcquireRead())
            {
                lock.unlock();
                callbacks_.dispatchAll(channel_->responses.currentRead());
                sync_->notifyReplyStateChanged();
                pumpReplies();
                return true;
            }

            sync_->reply_cv.wait(lock, [&] {
                return sync_->stopping || sync_->reply_epoch != observed;
            });

            if (sync_->stopping)
            {
                return false;
            }

            lock.unlock();
            pumpReplies();
            return true;
        }

        // -----------------------------------------------------------------
        //  Recording phase
        // -----------------------------------------------------------------

        bool beginFrame(const FrameMemoryHints& hints = {})
        {
            if (recording_ || staged_ready_ || pending_publish_)
            {
                return false;
            }

            staging_builder_.begin(hints);
            callbacks_.beginBatch();
            recording_ = true;
            return true;
        }

        [[nodiscard]] bool isRecording() const noexcept
        {
            return recording_;
        }

        [[nodiscard]] bool hasPendingSubmit() const noexcept
        {
            return staged_ready_ || pending_publish_;
        }

        Builder& builder() noexcept
        {
            assert(recording_ && "RenderClient::builder() requires beginFrame() first.");
            return staging_builder_;
        }

        const Builder& builder() const noexcept
        {
            assert(recording_ && "RenderClient::builder() requires beginFrame() first.");
            return staging_builder_;
        }

        void cancelFrame()
        {
            if (!recording_)
            {
                return;
            }

            staging_program_.clear_keep_capacity();
            callbacks_.rollbackBatch();
            recording_ = false;
            staged_ready_ = false;
        }

        // -----------------------------------------------------------------
        //  Submission phase
        // -----------------------------------------------------------------

        bool submitFrame(bool blocking = false)
        {
            // Finalize recording into staged state.
            if (recording_)
            {
                recording_ = false;
                staged_ready_ = true;
                callbacks_.commitBatch();
            }

            return retryPendingSubmit(blocking);
        }

        bool retryPendingSubmit(bool blocking = false)
        {
            // Case 1: frame already lives in a ring write slot, only publish is pending.
            if (pending_publish_)
            {
                if (!publishRequest(blocking))
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
            FrameProgram<RequestAlignment>* slot = beginRequestWrite(blocking);
            if (!slot)
            {
                return false;
            }

            swapFramePrograms(*slot, staging_program_);
            staged_ready_ = false;
            pending_publish_ = true;

            if (!publishRequest(blocking))
            {
                return false;
            }

            pending_publish_ = false;
            sync_->notifyRequestStateChanged();
            return true;
        }

        // -----------------------------------------------------------------
        //  Convenience wrapper
        // -----------------------------------------------------------------

        template <typename FillFn>
        bool tick(FillFn&& fill_fn, const FrameMemoryHints& hints = {})
        {
            pumpReplies();

            if (!retryPendingSubmit(false))
            {
                return false;
            }

            if (!beginFrame(hints))
            {
                return false;
            }

            fill_fn(staging_builder_);
            return submitFrame(false);
        }

        template <typename FillFn>
        bool tickBlocking(FillFn&& fill_fn, const FrameMemoryHints& hints = {})
        {
            pumpReplies();

            if (!retryPendingSubmit(true))
            {
                return false;
            }

            if (!beginFrame(hints))
            {
                return false;
            }

            fill_fn(staging_builder_);
            return submitFrame(true);
        }

        // -----------------------------------------------------------------
        //  Misc
        // -----------------------------------------------------------------

        void requestStop()
        {
            sync_->requestStop();
        }

        CallbackStore& callbacks() noexcept
        {
            return callbacks_;
        }

        const CallbackStore& callbacks() const noexcept
        {
            return callbacks_;
        }

        StageProgram& stagingProgram() noexcept
        {
            return staging_program_;
        }

        const StageProgram& stagingProgram() const noexcept
        {
            return staging_program_;
        }

    private:
        // While a blocking submit waits for a request slot, it re-pumps the
        // response ring at least this often. This is a self-healing safety net:
        // the request_cv notify path already wakes us, but a bounded wait
        // guarantees forward progress even under an unforeseen cross-CV
        // interleaving. Only ever spent while genuinely blocked, so the cost is
        // negligible. See beginRequestWrite() for the deadlock it defuses.
        static constexpr std::chrono::milliseconds kBlockedPumpInterval{1};

        FrameProgram<RequestAlignment>* beginRequestWrite(bool blocking)
        {
            if (!blocking)
            {
                return channel_->requests.tryBeginWrite();
            }

            for (;;)
            {
                if (auto* slot = channel_->requests.tryBeginWrite())
                {
                    return slot;
                }

                // Keep draining the response ring while we wait. Both rings are
                // depth-2; if we blocked here WITHOUT pumping, the server could
                // fill the response ring, then block in its OWN reply publish,
                // and so never drain our request ring — a two-ring cross-deadlock
                // (we wait on request_cv, the server waits on reply_cv, neither
                // advances). Pumping frees response slots and wakes the server's
                // reply_cv, breaking the cycle. Reply callbacks already run with
                // recording_==false (pumpReplies runs at the top of every tick),
                // so pumping here introduces no new re-entrancy context.
                pumpReplies();

                std::unique_lock lock(sync_->request_mutex);
                const std::uint64_t observed = sync_->request_epoch;

                if (auto* slot = channel_->requests.tryBeginWrite())
                {
                    return slot;
                }

                sync_->request_cv.wait_for(lock, kBlockedPumpInterval, [&] {
                    return sync_->stopping || sync_->request_epoch != observed;
                });

                if (sync_->stopping)
                {
                    return nullptr;
                }
            }
        }

        bool publishRequest(bool blocking)
        {
            if (!blocking)
            {
                return channel_->requests.publishWrite();
            }

            for (;;)
            {
                if (channel_->requests.publishWrite())
                {
                    return true;
                }

                // Drain responses while waiting — same two-ring deadlock guard
                // as beginRequestWrite(): the server only frees a request slot
                // (lets publishWrite succeed) once it can publish its replies.
                pumpReplies();

                std::unique_lock lock(sync_->request_mutex);
                const std::uint64_t observed = sync_->request_epoch;

                if (channel_->requests.publishWrite())
                {
                    return true;
                }

                sync_->request_cv.wait_for(lock, kBlockedPumpInterval, [&] {
                    return sync_->stopping || sync_->request_epoch != observed;
                });

                if (sync_->stopping)
                {
                    return false;
                }
            }
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
