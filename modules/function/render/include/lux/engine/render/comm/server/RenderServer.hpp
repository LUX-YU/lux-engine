#pragma once

#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <iostream>
#include <unordered_map>
#include <vector>

#include <lux/engine/function/visibility.h>
#include <lux/engine/render/comm/FrameProgram.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/core/Errors.hpp>
#include <lux/engine/render/core/FeatureHandle.hpp>
#include <lux/engine/render/core/RenderSceneId.hpp>
#include <lux/engine/render/core/RenderTypes.hpp>

namespace lux::window { class LuxWindow; }
// rdesc::Mesh is a struct (see description/Mesh.hpp) — keyword must match the
// definition for MSVC name mangling (class-vs-struct => V/U => LNK2019).
namespace lux::rdesc { struct Mesh; class Material; class Texture; struct ShaderInfo; }

namespace lux::render
{
    // Forward declarations for types defined in sinclude/
    struct RenderTargetLayout;
    struct GraphMaterialData;  // resources/GraphMaterialData.hpp (POD blob)
    template <std::size_t PayloadAlignment = 64>
    class FrameReplyBuilder
    {
    public:
        explicit FrameReplyBuilder(FrameReplies<PayloadAlignment>& dst) noexcept
            : dst_(&dst)
        {
        }

        void begin(const FrameMemoryHints& hints = {})
        {
            dst_->clear_keep_capacity();
            dst_->reserve(hints);
        }

        template <FrameBlobPayload T>
        bool push(TypeId type_id,
                  const T& payload,
                  std::uint16_t flags = 0,
                  RequestId request_id = kInvalidRequestId)
        {
            static_assert(alignof(T) <= PayloadAlignment,
                "Reply type alignment exceeds reply blob alignment. Raise PayloadAlignment.");

            const std::uint32_t offset = appendBytes(&payload, sizeof(T), alignof(T));
            dst_->replies.push_back(ReplyRecord{
                .type_id = type_id,
                .flags = flags,
                .reserved0 = 0,
                .payload_offset = offset,
                .payload_size = narrowU32(sizeof(T)),
                .request_id = request_id,
            });
            return true;
        }

        template <FrameBlobPayload CommandPayload, FrameBlobPayload ReplyPayload>
        bool pushFor(const CmdRecord& cmd, const ReplyPayload& payload, std::uint16_t flags = 0)
        {
            static_assert(command_has_reply_v<CommandPayload>,
                "CommandPayload does not declare a reply type.");
            static_assert(std::is_same_v<typename CommandTraits<CommandPayload>::Reply, ReplyPayload>,
                "Reply payload type does not match CommandTraits<CommandPayload>::Reply.");

            return push(CommandTraits<CommandPayload>::reply_type_id,
                payload,
                flags,
                cmd.request_id
            );
        }

    private:
        template <typename T>
        std::uint32_t appendBytes(const T* src, std::size_t byte_count, std::size_t alignment)
        {
            const std::size_t old_size = dst_->payload.size();
            const std::size_t offset = alignUp(old_size, alignment);
            const std::size_t new_size = offset + byte_count;

            if (new_size > dst_->payload.size())
            {
                dst_->payload.resize(new_size);
            }

            if (byte_count != 0)
            {
                std::memcpy(dst_->payload.data() + offset, src, byte_count);
            }
            return narrowU32(offset);
        }

        static std::uint32_t narrowU32(std::size_t value)
        {
            assert(value <= std::numeric_limits<std::uint32_t>::max());
            return static_cast<std::uint32_t>(value);
        }

    private:
        FrameReplies<PayloadAlignment>* dst_{nullptr};
    };

    // -----------------------------------------------------------------------------
    //  Typed payload readers / views
    // -----------------------------------------------------------------------------

    template <FrameBlobPayload T, std::size_t PayloadAlignment>
    T readUnaryPayload(const FrameProgram<PayloadAlignment>& pack, const CmdRecord& cmd)
    {
        assert(cmd.payload_size == sizeof(T));
        T value{};
        std::memcpy(&value, pack.payload.data() + cmd.payload_offset, sizeof(T));
        return value;
    }

    template <FrameBlobPayload T, std::size_t PayloadAlignment>
    std::span<const T> readBulkPayload(const FrameProgram<PayloadAlignment>& pack, const CmdRecord& cmd)
    {
        assert((cmd.payload_size % sizeof(T)) == 0);
        const auto* ptr = reinterpret_cast<const T*>(pack.payload.data() + cmd.payload_offset);
        assert((reinterpret_cast<std::uintptr_t>(ptr) % alignof(T)) == 0);
        return {ptr, cmd.payload_size / sizeof(T)};
    }

    template <FrameBlobPayload T, std::size_t PayloadAlignment>
    T readReplyPayload(const FrameReplies<PayloadAlignment>& pack, const ReplyRecord& rec)
    {
        assert(rec.payload_size == sizeof(T));
        T value{};
        std::memcpy(&value, pack.payload.data() + rec.payload_offset, sizeof(T));
        return value;
    }

    template <std::size_t PayloadAlignment>
    std::span<const std::byte> resolveBlob(const FrameProgram<PayloadAlignment>& pack, BlobRef ref)
    {
        assert(static_cast<std::size_t>(ref.offset) + ref.size <= pack.payload.size());
        return {
            pack.payload.data() + ref.offset,
            ref.size,
        };
    }

    template <std::size_t PayloadAlignment>
    std::string_view resolveString(const FrameProgram<PayloadAlignment>& pack, StringRef ref)
    {
        const auto bytes = resolveBlob(pack, ref);
        return {
            reinterpret_cast<const char*>(bytes.data()),
            bytes.size(),
        };
    }

    template <typename T, std::size_t PayloadAlignment>
    const T& resolveAttachment(const FrameProgram<PayloadAlignment>& pack, std::uint32_t attachment_index)
    {
        assert(attachment_index < pack.attachments.size());
        assert(pack.attachments[attachment_index].object != nullptr);
        return *static_cast<const T*>(pack.attachments[attachment_index].object);
    }

    template <std::size_t PayloadAlignment>
    struct ExternalDataView
    {
        std::span<const std::byte> bytes{};
        std::shared_ptr<const void> owner{};
    };

    template <std::size_t PayloadAlignment>
    ExternalDataView<PayloadAlignment> resolveExternalDataView(
        const FrameProgram<PayloadAlignment>& pack,
        ExternalDataRef ref,
        TypeId expected_attachment_type = kInvalidTypeId)
    {
        assert(ref.attachment_index < pack.attachments.size());
        const AttachmentRecord& record = pack.attachments[ref.attachment_index];
        if (expected_attachment_type != kInvalidTypeId)
            assert(record.type_id == expected_attachment_type);

        const std::byte* data = nullptr;
        std::uint32_t size = 0;
        std::shared_ptr<const void> owner;

        if (record.type_id == attachment_types::BorrowedBytes)
        {
            const auto& attachment = *static_cast<const BorrowedBytesAttachment*>(record.object);
            data = attachment.data;
            size = attachment.size;
        }
        else if (record.type_id == attachment_types::OwnedBytes)
        {
            const auto& attachment = *static_cast<const OwnedBytesAttachment*>(record.object);
            data = attachment.data;
            size = attachment.size;
            owner = attachment.owner;
        }
        else
        {
            assert(false && "resolveExternalDataView: unsupported attachment type");
        }

        assert(data != nullptr || ref.size == 0);
        assert(static_cast<std::size_t>(ref.offset) + ref.size <= size);

        return ExternalDataView<PayloadAlignment>{
            .bytes = { data + ref.offset, ref.size },
            .owner = std::move(owner),
        };
    }

    template <std::size_t PayloadAlignment>
    std::span<const std::byte> resolveExternalData(
        const FrameProgram<PayloadAlignment>& pack,
        ExternalDataRef ref,
        TypeId expected_attachment_type = kInvalidTypeId)
    {
        return resolveExternalDataView(pack, ref, expected_attachment_type).bytes;
    }

    // -----------------------------------------------------------------------------
    //  Execute context and dispatch tables
    // -----------------------------------------------------------------------------

    template <std::size_t RequestAlignment = 64, std::size_t ReplyAlignment = 64>
    struct ExecuteContext
    {
        const FrameProgram<RequestAlignment>& program;
        FrameReplyBuilder<ReplyAlignment>&    replies;
        void*                                 user_state{nullptr};
        const CmdRecord*                      current_cmd{nullptr};
        bool                                  dispatch_error{false};
        const char*                           dispatch_error_msg{nullptr};

        [[nodiscard]] RequestId currentRequestId() const noexcept
        {
            return current_cmd ? current_cmd->request_id : kInvalidRequestId;
        }

        [[nodiscard]] bool currentExpectsReply() const noexcept
        {
            return current_cmd && hasFlag(current_cmd->flags, CmdFlags::ExpectsReply);
        }

        void markDispatchError(const char* msg) noexcept
        {
            dispatch_error = true;
            dispatch_error_msg = msg;
        }

        void clearDispatchError() noexcept
        {
            dispatch_error = false;
            dispatch_error_msg = nullptr;
        }
    };

    template <std::size_t RequestAlignment, std::size_t ReplyAlignment>
    using ErasedHandlerFn = void (*)(
        ExecuteContext<RequestAlignment, ReplyAlignment>&,
        const CmdRecord&,
        std::span<const std::byte>
    );

    template <std::size_t RequestAlignment, std::size_t ReplyAlignment>
    struct HandlerEntry
    {
        ErasedHandlerFn<RequestAlignment, ReplyAlignment> fn{nullptr};
        std::uint32_t stride{0};
        const char* debug_name{""};
    };

    template <FrameBlobPayload CommandPayload, std::size_t RequestAlignment, std::size_t ReplyAlignment>
    bool replyToCurrent(ExecuteContext<RequestAlignment, ReplyAlignment>& ctx,
        const typename CommandTraits<CommandPayload>::Reply& payload,
        std::uint16_t flags = 0)
    {
        static_assert(command_has_reply_v<CommandPayload>,
            "CommandPayload does not declare a reply type.");

        assert(ctx.current_cmd != nullptr);
        return ctx.replies.template pushFor<CommandPayload>(*ctx.current_cmd, payload, flags);
    }

    template <std::size_t MaxOpcodes = 8, 
        std::size_t RequestAlignment = 64, std::size_t ReplyAlignment = 64>
    class FrameDispatcher
    {
    public:
        using Ctx     = ExecuteContext<RequestAlignment, ReplyAlignment>;
        using Entry   = HandlerEntry<RequestAlignment, ReplyAlignment>;

        // ── Name-based TypeId lookup (all opcode domains) ─────────────

        /// Result of a name-based TypeId lookup.
        struct NamedTypeEntry {
            OpCode opcode{0xff};
            TypeId type_id{kInvalidTypeId};
        };

        /// Look up a TypeId by its registered debug name (any opcode domain).
        /// Returns {0xff, kInvalidTypeId} if not found.
        [[nodiscard]] NamedTypeEntry findTypeId(std::string_view name) const noexcept
        {
            auto it = name_index_.find(name);
            return (it != name_index_.end()) ? it->second : NamedTypeEntry{};
        }

        // ── Static registration (generation 0 — never recycled) ──────────
        template <FrameBlobPayload T, void (*Fn)(Ctx&, const T&)>
        void registerUnary(OpCode opcode, TypeId type_id, const char* name = "")
        {
            assert(opcode < MaxOpcodes);
            ensureSlot(opcode, typeIdIndex(type_id));
            auto& dom = domains_[opcode];
            const auto idx = typeIdIndex(type_id);
            dom.handlers[idx] = Entry{
                .fn = &unaryThunk<T, Fn>,
                .stride = static_cast<std::uint32_t>(sizeof(T)),
                .debug_name = name,
            };
            dom.generations[idx] = typeIdGen(type_id);
            if (name && name[0] != '\0')
                name_index_[std::string_view{name}] = NamedTypeEntry{opcode, type_id};
        }

        template <FrameBlobPayload T, void (*Fn)(Ctx&, std::span<const T>)>
        void registerBulk(OpCode opcode, TypeId type_id, const char* name = "")
        {
            assert(opcode < MaxOpcodes);
            ensureSlot(opcode, typeIdIndex(type_id));
            auto& dom = domains_[opcode];
            const auto idx = typeIdIndex(type_id);
            dom.handlers[idx] = Entry{
                .fn = &bulkThunk<T, Fn>,
                .stride = static_cast<std::uint32_t>(sizeof(T)),
                .debug_name = name,
            };
            dom.generations[idx] = typeIdGen(type_id);
            if (name && name[0] != '\0')
                name_index_[std::string_view{name}] = NamedTypeEntry{opcode, type_id};
        }

        template <void (*Fn)(Ctx&, const CmdRecord&, std::span<const std::byte>)>
        void registerRaw(OpCode opcode, TypeId type_id, const char* name = "")
        {
            assert(opcode < MaxOpcodes);
            ensureSlot(opcode, typeIdIndex(type_id));
            auto& dom = domains_[opcode];
            const auto idx = typeIdIndex(type_id);
            dom.handlers[idx] = Entry{
                .fn = &rawThunk<Fn>,
                .stride = 0,
                .debug_name = name,
            };
            dom.generations[idx] = typeIdGen(type_id);
        }

        template <FrameBlobPayload T, void (*Fn)(Ctx&, const T&)>
        void registerResource(TypeId type_id, const char* name = "")
        {
            registerUnary<T, Fn>(opcodes::ResourceOp, type_id, name);
        }

        template <FrameBlobPayload T, void (*Fn)(Ctx&, const T&)>
        void registerFeature(TypeId type_id, const char* name = "")
        {
            registerUnary<T, Fn>(opcodes::CommandOp, type_id, name);
        }

        template <FrameBlobPayload T, void (*Fn)(Ctx&, const T&)>
        void registerDebug(TypeId type_id, const char* name = "")
        {
            registerUnary<T, Fn>(opcodes::Debug, type_id, name);
        }

        template <FrameBlobPayload T, void (*Fn)(Ctx&, std::span<const T>)>
        void registerBulk(TypeId type_id, const char* name = "")
        {
            registerBulk<T, Fn>(opcodes::BulkData, type_id, name);
        }

        // ── Dynamic slot management ─────────────────────────────────────
        /// Allocate a new slot in the given opcode domain. Returns a TypeId
        /// (index + generation). The caller must register a handler before
        /// any frame uses this TypeId.
        [[nodiscard]] TypeId allocateSlot(OpCode opcode)
        {
            assert(opcode < MaxOpcodes);
            auto& dom = domains_[opcode];

            std::uint16_t idx;
            if (!dom.free_list.empty())
            {
                idx = dom.free_list.back();
                dom.free_list.pop_back();
            }
            else
            {
                idx = static_cast<std::uint16_t>(dom.handlers.size());
                dom.handlers.emplace_back();
                dom.generations.push_back(0);
            }

            // Bump generation (skip 0 — reserved for static slots).
            auto& gen = dom.generations[idx];
            if (++gen == 0) gen = 1;

            return makeTypeId(idx, gen);
        }

        /// Free a previously allocated dynamic slot. The handler is cleared
        /// and the index returned to the free list.
        void freeSlot(OpCode opcode, TypeId id)
        {
            assert(opcode < MaxOpcodes);
            auto& dom = domains_[opcode];
            const auto idx = typeIdIndex(id);
            assert(idx < dom.handlers.size());
            assert(dom.generations[idx] == typeIdGen(id));
            {
                const char* n = dom.handlers[idx].debug_name;
                if (n && n[0] != '\0')
                    name_index_.erase(std::string_view{n});
            }
            dom.handlers[idx] = Entry{};
            dom.free_list.push_back(idx);
        }

        /// Convenience: allocate a slot and immediately register a unary handler.
        template <FrameBlobPayload T, void (*Fn)(Ctx&, const T&)>
        [[nodiscard]] TypeId allocateAndRegisterUnary(OpCode opcode, const char* name = "")
        {
            const TypeId id = allocateSlot(opcode);
            auto& dom = domains_[opcode];
            dom.handlers[typeIdIndex(id)] = Entry{
                .fn = &unaryThunk<T, Fn>,
                .stride = static_cast<std::uint32_t>(sizeof(T)),
                .debug_name = name,
            };
            if (name && name[0] != '\0')
                name_index_[std::string_view{name}] = NamedTypeEntry{opcode, id};
            return id;
        }

        /// Convenience: allocate a slot and immediately register a BULK handler
        /// (one call per batch of N payloads). The dynamic-op counterpart of
        /// registerBulk — lets a feature's register_ops_fn own a BulkData op the
        /// same way allocateAndRegisterUnary owns a unary one (e.g. a batched
        /// light update covering every light in one command).
        template <FrameBlobPayload T, void (*Fn)(Ctx&, std::span<const T>)>
        [[nodiscard]] TypeId allocateAndRegisterBulk(OpCode opcode, const char* name = "")
        {
            const TypeId id = allocateSlot(opcode);
            auto& dom = domains_[opcode];
            dom.handlers[typeIdIndex(id)] = Entry{
                .fn = &bulkThunk<T, Fn>,
                .stride = static_cast<std::uint32_t>(sizeof(T)),
                .debug_name = name,
            };
            if (name && name[0] != '\0')
                name_index_[std::string_view{name}] = NamedTypeEntry{opcode, id};
            return id;
        }

        // ── Register handler on an existing (dynamic) TypeId ─────────────

        void registerEntry(OpCode opcode, TypeId id, Entry entry)
        {
            assert(opcode < MaxOpcodes);
            auto& dom = domains_[opcode];
            const auto idx = typeIdIndex(id);
            assert(idx < dom.handlers.size());
            assert(dom.generations[idx] == typeIdGen(id));
            dom.handlers[idx] = entry;
        }

        // ── Dispatch ─────────────────────────────────────────────────────

        [[nodiscard]] bool execute(const FrameProgram<RequestAlignment>& program, Ctx& ctx) const
        {
            const auto* base = program.payload.data();

            for (const CmdRecord& cmd : program.commands)
            {
                if (cmd.opcode >= MaxOpcodes)
                {
                    std::cerr << "[FrameDispatcher] Invalid opcode=" << static_cast<uint32_t>(cmd.opcode) << "\n";
                    ctx.current_cmd = nullptr;
                    return false;
                }

                const auto payload_end = static_cast<std::size_t>(cmd.payload_offset) + cmd.payload_size;
                if (payload_end > program.payload.size())
                {
                    std::cerr << "[FrameDispatcher] Payload out of bounds: offset="
                              << cmd.payload_offset << ", size=" << cmd.payload_size << "\n";
                    ctx.current_cmd = nullptr;
                    return false;
                }

                assert(cmd.opcode < MaxOpcodes);
                assert(payload_end <= program.payload.size());

                const auto& dom = domains_[cmd.opcode];
                const auto idx  = typeIdIndex(cmd.type_id);
                if (idx >= dom.handlers.size())
                {
                    std::cerr << "[FrameDispatcher] Unknown TypeId index=" << idx << "\n";
                    ctx.current_cmd = nullptr;
                    return false;
                }

                if (dom.generations[idx] != typeIdGen(cmd.type_id))
                {
                    std::cerr << "[FrameDispatcher] TypeId generation mismatch for index=" << idx << "\n";
                    ctx.current_cmd = nullptr;
                    return false;
                }

                assert(idx < dom.handlers.size());
                assert(dom.generations[idx] == typeIdGen(cmd.type_id) &&
                       "TypeId generation mismatch — slot was recycled.");

                const auto bytes = std::span<const std::byte>{
                    base + cmd.payload_offset,
                    cmd.payload_size,
                };

                ctx.current_cmd = &cmd;
                ctx.clearDispatchError();
                if (!dispatchOne(dom.handlers[idx], ctx, cmd, bytes))
                {
                    if (ctx.dispatch_error_msg)
                        std::cerr << "[FrameDispatcher] Dispatch failed: " << ctx.dispatch_error_msg << "\n";
                    ctx.current_cmd = nullptr;
                    return false;
                }
                if (ctx.dispatch_error)
                {
                    std::cerr << "[FrameDispatcher] Handler payload validation failed"
                              << (ctx.dispatch_error_msg ? std::string(": ") + ctx.dispatch_error_msg : std::string())
                              << "\n";
                    ctx.current_cmd = nullptr;
                    return false;
                }
            }

            ctx.current_cmd = nullptr;
            return true;
        }

    private:
        void ensureSlot(OpCode opcode, std::uint16_t idx)
        {
            auto& dom = domains_[opcode];
            if (idx >= dom.handlers.size())
            {
                dom.handlers.resize(idx + 1);
                dom.generations.resize(idx + 1, 0);
            }
        }

        static bool dispatchOne(const Entry& entry, Ctx& ctx, const CmdRecord& cmd, std::span<const std::byte> bytes)
        {
            assert(entry.fn && "Missing handler registration for opcode/type_id pair.");
            if (!entry.fn)
                return false;
            entry.fn(ctx, cmd, bytes);
            return true;
        }

        template <FrameBlobPayload T, void (*Fn)(Ctx&, const T&)>
        static void unaryThunk(Ctx& ctx, const CmdRecord& cmd, std::span<const std::byte> bytes)
        {
            assert(bytes.size() == sizeof(T));
            if (bytes.size() != sizeof(T))
            {
                ctx.markDispatchError("unary payload size mismatch");
                return;
            }
            T value{};
            std::memcpy(&value, bytes.data(), sizeof(T));
            Fn(ctx, value);
        }

        template <FrameBlobPayload T, void (*Fn)(Ctx&, std::span<const T>)>
        static void bulkThunk(Ctx& ctx, const CmdRecord& cmd, std::span<const std::byte> bytes)
        {
            assert((bytes.size() % sizeof(T)) == 0);
            if ((bytes.size() % sizeof(T)) != 0)
            {
                ctx.markDispatchError("bulk payload size mismatch");
                return;
            }
            const auto* ptr = reinterpret_cast<const T*>(bytes.data());
            assert((reinterpret_cast<std::uintptr_t>(ptr) % alignof(T)) == 0);
            if ((reinterpret_cast<std::uintptr_t>(ptr) % alignof(T)) != 0)
            {
                ctx.markDispatchError("bulk payload alignment mismatch");
                return;
            }
            Fn(ctx, std::span<const T>{ptr, bytes.size() / sizeof(T)});
        }

        template <void (*Fn)(Ctx&, const CmdRecord&, std::span<const std::byte>)>
        static void rawThunk(Ctx& ctx, const CmdRecord& cmd, std::span<const std::byte> bytes)
        {
            Fn(ctx, cmd, bytes);
        }

        struct OpDomain
        {
            std::vector<Entry>          handlers{};
            std::vector<std::uint16_t>  generations{};
            std::vector<std::uint16_t>  free_list{};
        };

        std::array<OpDomain, MaxOpcodes> domains_{};

        /// Name → {OpCode, TypeId} index across all opcode domains.
        /// Keys are string_view into Entry::debug_name (stable const char*).
        std::unordered_map<std::string_view, NamedTypeEntry> name_index_;
    };

    template <std::size_t RequestAlignment = 64, std::size_t ReplyAlignment = 64, 
        std::size_t MaxOpcodes = 8>
    class RenderServer
    {
    public:
        using Channel    = RenderProgramChannel<RequestAlignment, ReplyAlignment>;
        using Dispatcher = FrameDispatcher<MaxOpcodes, RequestAlignment, ReplyAlignment>;

        explicit RenderServer(
            std::shared_ptr<Channel> channel,
            std::shared_ptr<RenderChannelSync> sync,
            Dispatcher& dispatcher)
            : channel_(std::move(channel)), sync_(std::move(sync)), dispatcher_(dispatcher)
        {
        }

        /// Non-blocking: acquire one request frame, dispatch handlers, publish replies.
        bool drainAndDispatch(void* user_state = nullptr)
        {
            if (!acquireAndExecute(/*blocking=*/false, user_state)) return false;
            return finalizeReplies(/*blocking=*/false);
        }

        /// Blocking: sleeps until there is work, reply space, or stop is requested.
        bool drainAndDispatchBlocking(void* user_state = nullptr)
        {
            if (!acquireAndExecute(/*blocking=*/true, user_state)) return false;
            return finalizeReplies(/*blocking=*/true);
        }

        void requestStop()
        {
            sync_->requestStop();
        }

    protected:
        /// Access the underlying channel (for completion-only reply frames).
        [[nodiscard]] Channel& channel() noexcept { return *channel_; }

        /// Access the channel sync object (for blocking waits / notifications).
        [[nodiscard]] RenderChannelSync& channelSync() noexcept { return *sync_; }

        /// Acquire one request frame, dispatch all handlers.
        /// On success the reply builder is open — caller MUST call finalizeReplies().
        /// Between these two calls, replyBuilder() is valid and the caller may
        /// push additional replies (e.g. deferred upload completions).
        bool acquireAndExecute(bool blocking, void* user_state)
        {
            // Retry any previously unpublished reply before starting new work.
            if (pending_reply_publish_)
            {
                if (!publishReplies(blocking))
                    return false;
                sync_->notifyReplyStateChanged();
                pending_reply_publish_ = false;
            }

            // Non-blocking: ensure a reply write slot is available BEFORE
            // consuming the request.  tryBeginWrite() is idempotent — if a
            // slot is already held it simply returns it.  This prevents a
            // consumed request from being lost when no reply slot is available.
            if (!blocking && !channel_->responses.tryBeginWrite())
                return false;

            if (!acquireRequest(blocking))
                return false;

            const auto& request = channel_->requests.currentRead();
            sync_->notifyRequestStateChanged();

            auto* reply_slot = beginReplyWrite(blocking);
            if (!reply_slot)
                return false;

            active_reply_.emplace(*reply_slot);
            active_reply_->begin();

            ExecuteContext<> ctx{
                .program     = request,
                .replies     = *active_reply_,
                .user_state  = user_state,
                .current_cmd = nullptr,
            };

            (void)dispatcher_.execute(request, ctx);
            return true;
        }

        /// Publish the reply frame opened by acquireAndExecute().
        bool finalizeReplies(bool blocking)
        {
            active_reply_.reset();

            if (!publishReplies(blocking))
            {
                // The reply data is still in the write slot (hasWriteSlot
                // remains true); mark it for retry so the next
                // acquireAndExecute() call publishes before consuming.
                pending_reply_publish_ = true;
                return false;
            }

            sync_->notifyReplyStateChanged();
            return true;
        }

        /// Access the reply builder between acquireAndExecute() and finalizeReplies().
        [[nodiscard]] FrameReplyBuilder<ReplyAlignment>& replyBuilder() noexcept
        {
            return *active_reply_;
        }

        bool acquireRequest(bool blocking)
        {
            if (!blocking)
            {
                return channel_->requests.tryAcquireRead();
            }

            for (;;)
            {
                if (channel_->requests.tryAcquireRead())
                {
                    return true;
                }

                std::unique_lock lock(sync_->request_mutex);
                const std::uint64_t observed = sync_->request_epoch;

                // Re-check after taking the mutex to avoid missed wakeups.
                if (channel_->requests.tryAcquireRead())
                {
                    return true;
                }

                sync_->request_cv.wait(lock, [&] {
                    return sync_->stopping || sync_->request_epoch != observed;
                });

                if (sync_->stopping)
                {
                    return false;
                }
            }
        }

        FrameReplies<ReplyAlignment>* beginReplyWrite(bool blocking)
        {
            if (!blocking)
            {
                return channel_->responses.tryBeginWrite();
            }

            for (;;)
            {
                if (FrameReplies<ReplyAlignment>* slot = channel_->responses.tryBeginWrite())
                {
                    return slot;
                }

                std::unique_lock lock(sync_->reply_mutex);
                const std::uint64_t observed = sync_->reply_epoch;

                if (FrameReplies<ReplyAlignment>* slot = channel_->responses.tryBeginWrite())
                {
                    return slot;
                }

                sync_->reply_cv.wait(lock, [&] {
                    return sync_->stopping || sync_->reply_epoch != observed;
                });

                if (sync_->stopping)
                {
                    return nullptr;
                }
            }
        }

        bool publishReplies(bool blocking)
        {
            if (!blocking)
            {
                return channel_->responses.publishWrite();
            }

            for (;;)
            {
                if (channel_->responses.publishWrite())
                {
                    return true;
                }

                std::unique_lock lock(sync_->reply_mutex);
                const std::uint64_t observed = sync_->reply_epoch;

                if (channel_->responses.publishWrite())
                {
                    return true;
                }

                sync_->reply_cv.wait(lock, [&] {
                    return sync_->stopping || sync_->reply_epoch != observed;
                });

                if (sync_->stopping)
                {
                    return false;
                }
            }
        }

    private:
        std::shared_ptr<Channel>                channel_;
        std::shared_ptr<RenderChannelSync>      sync_;
        Dispatcher&                             dispatcher_;
        std::optional<FrameReplyBuilder<ReplyAlignment>> active_reply_{};
        bool                                    pending_reply_publish_{false};
    };

    // ─────────────────────────────────────────────────────────────────────
    //  ServerConfig — plain data only (no Vulkan handles)
    // ─────────────────────────────────────────────────────────────────────

    struct ServerConfig
    {
        std::vector<const char*> instance_extensions{};
        uint32_t frames_in_flight{2};
        bool prefer_discrete_gpu{true};
        bool use_dynamic_rendering{true};
        bool enable_vsync{true};
        bool enable_validation{false};
    };

    // ─────────────────────────────────────────────────────────────────────
    //  GeneralRenderServer
    // ─────────────────────────────────────────────────────────────────────
    class LUX_FUNCTION_PUBLIC GeneralRenderServer : public RenderServer<>
    {
    public:
        using Channel    = RenderServer<>::Channel;
        using Dispatcher = RenderServer<>::Dispatcher;

        GeneralRenderServer(
            std::shared_ptr<Channel> channel,
            std::shared_ptr<RenderChannelSync> sync
        );

        virtual ~GeneralRenderServer();

        /// Initialize the Vulkan stack. Must be called before tick() or attachToWindow().
        [[nodiscard]] Expected<void> init(ServerConfig config = {});

        /// Drain one request (non-blocking). Returns false on stop / no work.
        bool drainRequest();

        /// Drain one request (blocking). Returns false on stop.
        bool drainRequestBlocking();

        /// Blocking full-cycle: drain request + render all enabled views.
        /// Returns false when stop is requested.
        virtual bool tick();

        /// Attach to a window: creates RenderSurface + SwapchainTarget.
        /// Must be called before any non-offscreen view is added.
        [[nodiscard]] Expected<void> attachToWindow(lux::window::LuxWindow& window);

        // ── Server-side direct initialization (same thread, before tick()) ──
        /// Register a FeatureFactory, returns full registration result including ops.
        [[nodiscard]] FeatureTypeRegisteredReply addFeatureFactory(const FeatureFactory& factory);

        /// Batch register, returns corresponding registration results.
        [[nodiscard]] std::vector<FeatureTypeRegisteredReply> addFeatureFactories(std::span<const FeatureFactory> factories);

        /// Parameter for creating a feature within createScene().
        struct FeatureInitParam {
            uint32_t    feature_type_id;
            const void* param{nullptr};
            size_t      param_size{0};
        };

        /// Result of createScene().
        struct CreateSceneResult {
            RenderSceneId              scene_id{};
            std::vector<FeatureHandle> features;
            std::vector<ViewHandle>    views;
        };

        /// Create a scene and optionally add features in one call.
        /// Large-world coarse cull is opted into by adding SpatialCullFeature to the
        /// `features` list (kSpatialCullFeatureFactory) — the core scene knows nothing
        /// about it.
        [[nodiscard]] CreateSceneResult createScene(
            std::string_view name,
            std::span<const FeatureInitParam> features = {},
            lux::common::ETextureFormat lit_color_format = lux::common::ETextureFormat::RGBA16_SFLOAT);

        /// Parameter for creating a view (always offscreen).
        struct ViewInitParam {
            RenderSceneId    scene_id;
            common::Size2D         extent;
            std::string_view name;
        };

        /// Create an offscreen view for the given scene.
        [[nodiscard]] Expected<ViewHandle> createView(const ViewInitParam& param);

        // ── Swapchain binding (independent of view creation) ────────────

        /// Bind a scene + view to the swapchain for direct rendering.
        /// Only one swapchain binding is allowed at a time.
        /// @param layout  The RenderTargetLayout to use (caller decides
        ///                final_layout, e.g. PRESENT_SRC_KHR or
        ///                COLOR_ATTACHMENT_OPTIMAL when an overlay follows).
        [[nodiscard]] Expected<void> bindSwapchain(
            RenderSceneId scene_id, ViewHandle view,
            const RenderTargetLayout& layout);

        /// Remove the current swapchain binding.
        void unbindSwapchain();

        /// Whether a swapchain binding is active.
        [[nodiscard]] bool hasSwapchainBinding() const noexcept;

        /// Return the default swapchain layout (from SwapchainProvider).
        /// Only valid after attachToWindow().
        [[nodiscard]] const RenderTargetLayout& swapchainLayout() const;

        // ── Server-side direct resource creation (same thread, init → tick) ──
        // These are synchronous, blocking calls for use between init() and
        // the first tick(). They bypass the protocol dispatcher entirely.

        // (uploadMesh — synchronous server-side mesh upload — removed: no callers after
        //  mesh data upload became a StandardMeshStack feature op. The async upload path
        //  is the exported serverUploadMesh shim.)

        // (uploadMaterial(rdesc::Material) retired in W5a; uploadGraphMaterial removed in
        //  Stage C — the graph-material upload assembly is a StandardMaterial feature
        //  concern now (serverUploadGraphMaterial, in the feature TU). The core server API
        //  no longer names material upload.)

        /// Create a 2D texture. Staging is deferred until flushPendingGpuTransfers().
        [[nodiscard]] Expected<RTextureHandle> createTexture2D(
            const lux::rdesc::Texture& texture,
            bool generate_mips = true
        );

        // (createLight removed — light creation is feature-scoped via LightFeature;
        //  see renderer/features/light/LightOperationHandlers.cpp.)

        /// Compile a shader from SPIR-V (fully synchronous — VkShaderModule).
        [[nodiscard]] ShaderHandle compileShader(
            std::span<const std::byte> spirv,
            const lux::rdesc::ShaderInfo* info = nullptr
        );

        // (MeshInstanceParam + addMeshInstance removed — the mesh-instance assembly is a
        //  StandardMeshStack feature concern now (serverAddMeshInstance, in the feature
        //  TU). The core server API no longer names mesh instances.)

        /// Submit all pending staging transfers (mesh + texture) to the GPU and wait.
        /// Must be called after all uploadMesh/createTexture2D calls, before tick().
        [[nodiscard]] Expected<void> flushPendingGpuTransfers();

        class Impl;

    protected:
        GeneralRenderServer(
            std::shared_ptr<Channel> channel,
            std::shared_ptr<RenderChannelSync> sync,
            std::unique_ptr<Impl> impl
        );

        /// Try to flush pending upload-completion replies without a request.
        /// Opens a standalone reply-only frame, publishes, and returns.
        void flushDeferredRepliesOnly();

        /// Advance + flush in-flight async readbacks (ReadbackViewAsync):
        /// settle, submit the GPU copy, poll the fence, and send the deferred
        /// reply (by request_id) on completion. Called once per tick().
        void pollPendingReadbacks();

        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::render
