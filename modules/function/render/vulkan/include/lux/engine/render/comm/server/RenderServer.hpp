#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <lux/engine/function/visibility.h>
#include <lux/engine/function/render/client/FrameProgram.hpp>
#include <lux/engine/function/render/client/UploadLifecycle.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/function/render/client/core/DeviceCaps.hpp>
#include <lux/engine/function/render/client/core/EFeatureLevel.hpp>
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/core/RenderTypes.hpp>
#include <lux/engine/resource/deployment/RuntimeCapacity.hpp>

namespace lux::window { class LuxWindow; }
// rdesc::Mesh is a struct (see description/Mesh.hpp) — keyword must match the
// definition for MSVC name mangling (class-vs-struct => V/U => LNK2019).
namespace lux::rdesc { struct Mesh; class Material; class Texture; struct ShaderInfo; }

namespace lux::render
{
    // Forward declarations for types defined in sinclude/
    struct RenderTargetLayout;
    struct GraphMaterialData;  // resources/GraphMaterialData.hpp (POD blob)

    // 派生服务器扩展面(protected)用到的树内类型。只按引用/指针出现在声明里,
    // 故安装面不必认识它们的定义 —— 派生类在树内 include 对应的 sinclude 头。
    struct FrameTickState;         // FrameOrchestrator.hpp
    struct FrameStamp;             // core/FrameStamp.hpp
    class  RenderTargetRegistry;   // renderer/RenderTargetRegistry.hpp
    class  Renderer;               // renderer/Renderer.hpp
    class  ResourceContext;        // resources/ResourceContext.hpp
    class  DeviceContext;          // gpu/DeviceContext.hpp
    class  SwapchainProvider;      // targets/SwapchainProvider.hpp
    class  PresentContext;         // targets/PresentContext.hpp

    /// 场景销毁前的回调 —— 让派生类清掉自己缓存的 per-scene 指针。
    /// (原先埋在 Impl 里,随扩展面一并提到命名空间层。)
    using PreDestroySceneCallback = void (*)(void* extension, RenderSceneId);

    /// 从命令处理器上下文的 user_state 取回派生类挂的扩展指针
    /// (setExtension 存的那个)。
    ///
    /// 派生类注册的处理器拿到的 user_state 是基类的私有 Impl。此前 UI 为了
    /// 取其中一个字段,把整个 Impl 头 include 了进去 —— 于是 Impl 的每个成员
    /// 都成了它的接口。这个函数就是那一个字段的正门。
    [[nodiscard]] LUX_FUNCTION_PUBLIC void* serverExtensionOf(void* user_state) noexcept;

    template <std::size_t PayloadAlignment = 64>
    class FrameReplyBuilder
    {
    public:
        explicit FrameReplyBuilder(ReplyPacket<PayloadAlignment>& dst) noexcept
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
            if (value > std::numeric_limits<std::uint32_t>::max())
                renderFatal("reply packet exceeds the 32-bit wire limit");
            return static_cast<std::uint32_t>(value);
        }

    private:
        ReplyPacket<PayloadAlignment>* dst_{nullptr};
    };

    // -----------------------------------------------------------------------------
    //  Typed payload readers / views
    // -----------------------------------------------------------------------------

    template <std::size_t PayloadAlignment>
    std::span<const std::byte> resolveBlob(const CommandStorage<PayloadAlignment>& pack, BlobRef ref)
    {
        auto bytes = CommandPacketView{pack}.bytes(ref.offset, ref.size);
        if (!bytes)
            return {};
        return *bytes;
    }

    template <std::size_t PayloadAlignment>
    std::string_view resolveString(const CommandStorage<PayloadAlignment>& pack, StringRef ref)
    {
        const auto bytes = resolveBlob(pack, ref);
        return {
            reinterpret_cast<const char*>(bytes.data()),
            bytes.size(),
        };
    }

    template <std::size_t PayloadAlignment>
    struct ExternalDataView
    {
        std::span<const std::byte> bytes{};
        std::shared_ptr<const void> owner{};
    };

    template <std::size_t PayloadAlignment>
    ExternalDataView<PayloadAlignment> resolveExternalDataView(
        const CommandStorage<PayloadAlignment>& pack,
        ExternalDataRef ref,
        TypeId expected_attachment_type = kInvalidTypeId)
    {
        auto attachment_record =
            CommandPacketView{pack}.attachment(ref.attachment_index);
        if (!attachment_record)
            return {};
        const AttachmentRecord& record = attachment_record->get();
        if (expected_attachment_type != kInvalidTypeId && record.type_id != expected_attachment_type)
            return {};

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
            return {};
        }

        if ((data == nullptr && ref.size != 0) ||
            ref.offset > size || ref.size > size - ref.offset)
            return {};

        return ExternalDataView<PayloadAlignment>{
            .bytes = { data + ref.offset, ref.size },
            .owner = std::move(owner),
        };
    }

    template <std::size_t PayloadAlignment>
    std::span<const std::byte> resolveExternalData(
        const CommandStorage<PayloadAlignment>& pack,
        ExternalDataRef ref,
        TypeId expected_attachment_type = kInvalidTypeId)
    {
        // Synchronous handlers consume the attachment while the command
        // packet itself is alive. Do not manufacture a second shared_ptr just
        // to return a span: besides an unnecessary atomic refcount round trip,
        // that obscures packet-ownership bugs by releasing the backing store
        // in the middle of dispatch. Handlers that retain bytes beyond this
        // call must explicitly use resolveExternalDataView().
        auto attachment_record =
            CommandPacketView{pack}.attachment(ref.attachment_index);
        if (!attachment_record)
            return {};
        const AttachmentRecord& record = attachment_record->get();
        if (expected_attachment_type != kInvalidTypeId &&
            record.type_id != expected_attachment_type)
        {
            return {};
        }

        const std::byte* data = nullptr;
        std::uint32_t size = 0u;
        if (record.type_id == attachment_types::BorrowedBytes)
        {
            const auto& attachment = *static_cast<
                const BorrowedBytesAttachment*>(record.object);
            data = attachment.data;
            size = attachment.size;
        }
        else if (record.type_id == attachment_types::OwnedBytes)
        {
            const auto& attachment = *static_cast<
                const OwnedBytesAttachment*>(record.object);
            data = attachment.data;
            size = attachment.size;
        }
        else
        {
            return {};
        }

        if ((data == nullptr && ref.size != 0u) || ref.offset > size ||
            ref.size > size - ref.offset)
        {
            return {};
        }
        return {data + ref.offset, ref.size};
    }

    // -----------------------------------------------------------------------------
    //  Execute context and dispatch tables
    // -----------------------------------------------------------------------------

    /// 分发失败的自发上报汇(不依附于任何请求)。即发即忘命令的失败此前在
    /// emitFailure 处就到头了 —— 没有回复可路由,也没有别的出口,是彻底的黑洞;
    /// 现在每次分发失败都(额外)打到这个汇上,由具体服务器接进自己的错误事件
    /// 通道(GeneralRenderServer → RenderErrorSink → 客户端 error handler)。
    using DispatchFailureSink = void (*)(void* user_state,
                                         EDispatchFailure code,
                                         const RenderError& error);

    template <std::size_t RequestAlignment = 64, std::size_t ReplyAlignment = 64>
    struct ExecuteContext
    {
        const CommandStorage<RequestAlignment>& program;
        FrameReplyBuilder<ReplyAlignment>&    replies;
        void*                                 user_state{nullptr};
        const CmdRecord*                      current_cmd{nullptr};
        DispatchFailureSink                   on_dispatch_failure{nullptr};

        /// 载荷解码自报的失败(见 unaryThunk / bulkThunk)。`ok()` 为真 = 无错。
        /// 此前是一个 bool + 一个 `const char*` 消息,execute() 把消息打到 stderr,
        /// 交给客户端的只有一个粗粒度的 EDispatchFailure。现在带上期望值/实收值。
        RenderError                           dispatch_error{};

        [[nodiscard]] RequestId currentRequestId() const noexcept
        {
            return current_cmd ? current_cmd->request_id : kInvalidRequestId;
        }

        [[nodiscard]] bool currentExpectsReply() const noexcept
        {
            return current_cmd && hasFlag(current_cmd->flags, CmdFlags::ExpectsReply);
        }

        void markDispatchError(const RenderError& error) noexcept { dispatch_error = error; }
        void clearDispatchError() noexcept { dispatch_error = RenderError{}; }
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

        if (ctx.current_cmd == nullptr)
            return false;
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
            if (opcode >= MaxOpcodes)
                return;
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
            if (opcode >= MaxOpcodes)
                return;
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

        template <FrameBlobPayload T, void (*Fn)(Ctx&, const T&)>
        void registerResource(TypeId type_id, const char* name = "")
        {
            registerUnary<T, Fn>(opcodes::ResourceOp, type_id, name);
        }

        // ── Dynamic slot management ─────────────────────────────────────
        /// Allocate a new slot in the given opcode domain. Returns a TypeId
        /// (index + generation). The caller must register a handler before
        /// any frame uses this TypeId.
        [[nodiscard]] TypeId allocateSlot(OpCode opcode)
        {
            if (opcode >= MaxOpcodes)
                return kInvalidTypeId;
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
            if (opcode >= MaxOpcodes)
                return;
            auto& dom = domains_[opcode];
            const auto idx = typeIdIndex(id);
            if (idx >= dom.handlers.size() ||
                dom.generations[idx] != typeIdGen(id))
                return;
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
            if (id == kInvalidTypeId)
                return id;
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
            if (id == kInvalidTypeId)
                return id;
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
            if (opcode >= MaxOpcodes)
                return;
            auto& dom = domains_[opcode];
            const auto idx = typeIdIndex(id);
            if (idx >= dom.handlers.size() ||
                dom.generations[idx] != typeIdGen(id))
                return;
            dom.handlers[idx] = entry;
        }

        // ── Dispatch ─────────────────────────────────────────────────────

        [[nodiscard]] bool execute(const FrameProgram<RequestAlignment>& program, Ctx& ctx) const
        {
            return executeCommands(program.commands, program, ctx);
        }

        [[nodiscard]] bool executeOne(
            const OperationPacket<RequestAlignment>& packet,
            Ctx& ctx
        ) const
        {
            if (!packet.has_command)
                return false;
            return executeCommands(
                std::span<const CmdRecord>{&packet.command, 1},
                packet,
                ctx
            );
        }

    private:
        [[nodiscard]] bool executeCommands(
            std::span<const CmdRecord> commands,
            const CommandStorage<RequestAlignment>& program,
            Ctx& ctx
        ) const
        {
            const auto packet = CommandPacketView{program};

            // 分发失败转成发给原请求的 CommandFailedReply(当它等回复时),客户端
            // 由此以失败解除等待,而不是死等一个永远不来的回复。按 request_id 路由。
            // 即发即忘的命令没有可路由的目的地 —— 走 on_dispatch_failure 自发汇
            //(每条失败都额外打一份到汇上,等回复的也打:错误事件通道是唯一
            // 会到 stderr/日志的出口,CommandFailedReply 只负责解除等待)。
            auto emitFailure = [&ctx](const CmdRecord& c, EDispatchFailure code, const RenderError& error)
            {
                if (hasFlag(c.flags, CmdFlags::ExpectsReply))
                    ctx.replies.template push<CommandFailedReply>(
                        type_ids::ReplyCommandFailed,
                        CommandFailedReply{static_cast<uint32_t>(code), error},
                        0, c.request_id);
                if (ctx.on_dispatch_failure)
                    ctx.on_dispatch_failure(ctx.user_state, code, error);
            };

            // ★ 一条命令失败**只跳过它自己**(continue),不中止整帧。
            //   program.commands 是结构化记录数组,每条命令的载荷偏移独立成立 ——
            //   「后面的命令不再可信」并不成立,逐条校验会各自把关。此前这里
            //   return false:一条失败让同帧**剩余全部命令悄悄蒸发**,其中等回复的
            //   那些永远等不到(CommandFailedReply 只发给了失败的那条),批量
            //   bring-up(settleFeatures / CameraViewSubsystem::settle)因此挂死 ——
            //   失败被放大成不可诊断的卡死。失败命令的后继若依赖其效果,会拿着
            //   无效句柄被服务端各自的校验闸拒绝,退化是逐级可见的。
            bool all_ok = true;
            for (const CmdRecord& cmd : commands)
            {
                ctx.current_cmd = nullptr;

                if (cmd.opcode >= MaxOpcodes)
                {
                    emitFailure(cmd, EDispatchFailure::InvalidOpcode,
                                renderError<err::comm::InvalidOpcode>(cmd.opcode));
                    all_ok = false;
                    continue;
                }

                auto payload_bytes = packet.bytes(cmd);
                if (!payload_bytes)
                {
                    emitFailure(cmd, EDispatchFailure::PayloadOutOfBounds,
                                payload_bytes.error());
                    all_ok = false;
                    continue;
                }

                const auto& dom = domains_[cmd.opcode];
                const auto  idx = typeIdIndex(cmd.type_id);
                if (idx >= dom.handlers.size())
                {
                    emitFailure(cmd, EDispatchFailure::UnknownTypeId,
                                renderError<err::comm::UnknownTypeId>(idx));
                    all_ok = false;
                    continue;
                }

                if (dom.generations[idx] != typeIdGen(cmd.type_id))
                {
                    emitFailure(cmd, EDispatchFailure::TypeIdGeneration,
                                renderError<err::comm::TypeIdGenerationMismatch>(idx));
                    all_ok = false;
                    continue;
                }

                const auto& entry = dom.handlers[idx];
                if (entry.fn == nullptr)
                {
                    emitFailure(cmd, EDispatchFailure::HandlerRejected,
                                renderError<err::comm::HandlerNotRegistered>(idx));
                    all_ok = false;
                    continue;
                }

                ctx.current_cmd = &cmd;
                ctx.clearDispatchError();
                entry.fn(ctx, cmd, *payload_bytes);

                // 载荷解码失败(长度/对齐对不上声明的类型)。处理器已经跳过了;
                // 同型命令的后继会各自撞上同一道校验,逐条以失败回执解除等待。
                if (!ctx.dispatch_error.ok())
                {
                    emitFailure(cmd, EDispatchFailure::PayloadValidation, ctx.dispatch_error);
                    all_ok = false;
                }
            }

            ctx.current_cmd = nullptr;
            return all_ok;
        }

        void ensureSlot(OpCode opcode, std::uint16_t idx)
        {
            auto& dom = domains_[opcode];
            if (idx >= dom.handlers.size())
            {
                dom.handlers.resize(idx + 1);
                dom.generations.resize(idx + 1, 0);
            }
        }

        // (dispatchOne 已删:它只做一件 execute() 现在自己做的事 —— 空函数指针检查。
        //  那个检查如今是四道闸中的一道,失败带 err::comm::HandlerNotRegistered 交回,
        //  而不是靠一个只在 debug 下存在的 assert 加一个被丢弃的 bool。)

        template <FrameBlobPayload T, void (*Fn)(Ctx&, const T&)>
        static void unaryThunk(Ctx& ctx, const CmdRecord& cmd, std::span<const std::byte> bytes)
        {
            if (bytes.size() != sizeof(T))
            {
                ctx.markDispatchError(renderError<err::comm::PayloadSizeMismatch>(
                    static_cast<std::uint32_t>(sizeof(T)),
                    static_cast<std::uint32_t>(bytes.size())));
                return;
            }
            T value{};
            std::memcpy(&value, bytes.data(), sizeof(T));
            Fn(ctx, value);
        }

        template <FrameBlobPayload T, void (*Fn)(Ctx&, std::span<const T>)>
        static void bulkThunk(Ctx& ctx, const CmdRecord& cmd, std::span<const std::byte> bytes)
        {
            if ((bytes.size() % sizeof(T)) != 0)
            {
                ctx.markDispatchError(renderError<err::comm::BulkPayloadNotMultiple>(
                    static_cast<std::uint32_t>(sizeof(T)),
                    static_cast<std::uint32_t>(bytes.size())));
                return;
            }

            const auto* ptr = reinterpret_cast<const T*>(bytes.data());
            if ((reinterpret_cast<std::uintptr_t>(ptr) % alignof(T)) != 0)
            {
                ctx.markDispatchError(renderError<err::comm::BulkPayloadMisaligned>(
                    static_cast<std::uint32_t>(alignof(T))));
                return;
            }

            Fn(ctx, std::span<const T>{ptr, bytes.size() / sizeof(T)});
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
        using Channel    = RenderFrameChannel<RequestAlignment, ReplyAlignment>;
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

        [[nodiscard]] Channel& endpoint() noexcept { return *channel_; }

        /// True while the primary frame dispatcher owns a populated response
        /// slot that could not be published yet. Auxiliary response producers
        /// must not enter the same SPSC ring until that publication succeeds.
        [[nodiscard]] bool hasPendingReplyPublication() const noexcept
        {
            return pending_reply_publish_;
        }

        /// 安装分发失败的自发上报汇(见 DispatchFailureSink)。具体服务器在 init
        /// 时接进自己的错误事件通道;不装则失败只以 CommandFailedReply 的形式
        /// 回到等回复的请求上(即发即忘命令的失败不可见)。
        void setDispatchFailureSink(DispatchFailureSink sink) noexcept
        {
            dispatch_failure_sink_ = sink;
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
                sync_->notifyReplyProduced();
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
                .program             = request,
                .replies             = *active_reply_,
                .user_state          = user_state,
                .current_cmd         = nullptr,
                .on_dispatch_failure = dispatch_failure_sink_,
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

            sync_->notifyReplyProduced();
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

                const std::uint64_t observed =
                    sync_->work_epoch.load(std::memory_order_acquire);

                // Re-check after observing the epoch to avoid a missed wakeup.
                if (channel_->requests.tryAcquireRead())
                {
                    return true;
                }

                if (sync_->isStopping())
                {
                    return false;
                }
                sync_->work_epoch.wait(observed, std::memory_order_acquire);
                if (sync_->isStopping())
                    return false;
            }
        }

        ReplyPacket<ReplyAlignment>* beginReplyWrite(bool blocking)
        {
            if (!blocking)
            {
                return channel_->responses.tryBeginWrite();
            }

            for (;;)
            {
                if (ReplyPacket<ReplyAlignment>* slot = channel_->responses.tryBeginWrite())
                {
                    return slot;
                }

                const std::uint64_t observed =
                    sync_->work_epoch.load(std::memory_order_acquire);

                if (ReplyPacket<ReplyAlignment>* slot = channel_->responses.tryBeginWrite())
                {
                    return slot;
                }

                if (sync_->isStopping())
                {
                    return nullptr;
                }
                sync_->work_epoch.wait(observed, std::memory_order_acquire);
                if (sync_->isStopping())
                    return nullptr;
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

                const std::uint64_t observed =
                    sync_->work_epoch.load(std::memory_order_acquire);

                if (channel_->responses.publishWrite())
                {
                    return true;
                }

                if (sync_->isStopping())
                {
                    return false;
                }
                sync_->work_epoch.wait(observed, std::memory_order_acquire);
                if (sync_->isStopping())
                    return false;
            }
        }

    private:
        std::shared_ptr<Channel>                channel_;
        std::shared_ptr<RenderChannelSync>      sync_;
        Dispatcher&                             dispatcher_;
        std::optional<FrameReplyBuilder<ReplyAlignment>> active_reply_{};
        bool                                    pending_reply_publish_{false};
        DispatchFailureSink                     dispatch_failure_sink_{nullptr};
    };

    template <class Channel>
    class RenderOperationServer final
    {
    public:
        using Dispatcher = FrameDispatcher<>;

        RenderOperationServer(
            std::shared_ptr<Channel> channel,
            std::shared_ptr<RenderChannelSync> sync,
            Dispatcher& dispatcher)
            : channel_(std::move(channel)),
              sync_(std::move(sync)),
              dispatcher_(dispatcher)
        {
        }

        void setDispatchFailureSink(DispatchFailureSink sink) noexcept
        {
            dispatch_failure_sink_ = sink;
        }

        [[nodiscard]] bool drainAndDispatch(void* user_state = nullptr)
        {
            if (pending_reply_publish_)
            {
                if (!channel_->responses.publishWrite())
                    return false;
                pending_reply_publish_ = false;
                sync_->notifyReplyProduced();
            }

            auto* reply_slot = channel_->responses.tryBeginWrite();
            if (reply_slot == nullptr)
                return false;

            OperationPacket<> packet{};
            if (channel_->requests.tryPop(packet) !=
                lux::cxx::EQueuePopResult::VALUE)
                return false;
            sync_->notifyRequestStateChanged();

            FrameReplyBuilder<> replies(*reply_slot);
            replies.begin();
            ExecuteContext<> ctx{
                .program = packet,
                .replies = replies,
                .user_state = user_state,
                .current_cmd = nullptr,
                .on_dispatch_failure = dispatch_failure_sink_,
            };
            const auto accounted_bytes = packet.accountedBytes();
            (void)dispatcher_.executeOne(packet, ctx);
            if constexpr (requires { channel_->releaseBytes(accounted_bytes); })
                channel_->releaseBytes(accounted_bytes);

            if (!channel_->responses.publishWrite())
            {
                pending_reply_publish_ = true;
                return false;
            }
            sync_->notifyReplyProduced();
            return true;
        }

        [[nodiscard]] Channel& endpoint() noexcept
        {
            return *channel_;
        }

        /// True while this operation dispatcher owns a populated response
        /// slot that could not be published yet. Auxiliary response producers
        /// sharing the lane must defer until the primary publication succeeds.
        [[nodiscard]] bool hasPendingReplyPublication() const noexcept
        {
            return pending_reply_publish_;
        }

    private:
        std::shared_ptr<Channel>               channel_;
        std::shared_ptr<RenderChannelSync>     sync_;
        Dispatcher&                           dispatcher_;
        DispatchFailureSink                   dispatch_failure_sink_{nullptr};
        bool                                  pending_reply_publish_{false};
    };

    using RenderControlServer = RenderOperationServer<RenderControlChannel<>>;
    using RenderUploadServer = RenderOperationServer<RenderUploadChannel<>>;

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
        /// Feature-tier ceiling: session tier = min(device-achievable, this).
        /// Desktop (default) = no restriction; set MobileHigh/Mobile to force
        /// the mobile implementation variants on a desktop GPU for testing
        /// (mobile-adaptation topic ①, item 1-3).
        EFeatureLevel preferred_level{EFeatureLevel::Desktop};
        lux::deployment::RuntimeCapacityRequest capacity_request{};
        lux::deployment::CapacityShortfall* capacity_shortfall_output{
            nullptr};
        /// Optional (requires enable_validation): incremented once per
        /// validation ERROR by the built-in debug callback. Lets lifecycle
        /// tests turn "validation printed something" into a hard assertion
        /// (the scene-cycle stress gate). Must outlive the server.
        std::atomic<int>* validation_error_counter{nullptr};

        /// Optional (requires enable_validation): receives the layer's message
        /// text verbatim. @p severity is 0=info / 1=warn / 2=error, and @p text
        /// is only valid for the duration of the call — copy it to keep it.
        ///
        /// 校验层的消息**只有全文有用**:一条 VUID 违规的价值全在它指名道姓说了
        /// 哪个字段哪条规则。而全文过不了 comm(POD 载荷装不下变长文本),所以
        /// 自发上报那条路只带得走严重级别与指纹 —— 够合并计数,不够诊断。
        ///
        /// 这个钩子是全文的唯一出口。渲染库不替应用决定这些文字去哪个终端/日志/
        /// 面板,由装钩子的人决定;不装则全文就地丢弃(计数与指纹照旧)。
        /// 调用发生在校验层回调所在的线程 —— 可能是任意线程,实现须自己保证安全。
        std::function<void(std::uint32_t severity, std::string_view text)>
            validation_message_sink{};
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
            std::shared_ptr<Channel> frame_channel,
            std::shared_ptr<RenderControlChannel<>> control_channel,
            std::shared_ptr<RenderUploadChannel<>> upload_channel,
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
            /// 与入参 `features` 一一对应。装配失败的位置留一个无效句柄,
            /// 对应的 `feature_errors[i]` 说明原因(成功时 `ok()` 为真)。
            std::vector<FeatureHandle> features;
            std::vector<RenderError>   feature_errors;
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

        /// 设备实际启用了哪些能力。init 之后可读;失败或未初始化时返回错误。
        ///
        /// 这是 `QueryDeviceCaps` 命令的进程内孪生:同一份 `DeviceCaps`,只是直接持有
        /// 服务对象的调用方(编辑器的装配计划就是)不必绕一圈 comm。存在的理由与命令
        /// 面完全一致 —— 引擎不再替上层在延迟/前向、local-read/采样之间选路,那上层
        /// 就必须先看得见设备能做什么。装 feature 之前问它。
        [[nodiscard]] Expected<DeviceCaps> deviceCaps() const noexcept;
        [[nodiscard]] const lux::deployment::RuntimeCapacityPlan&
        capacityPlan() const noexcept;

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
        [[nodiscard]] RenderTargetLayout swapchainLayout() const;

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

        /// Lock-free read on the render thread. Primarily consumed by close
        /// reports and lifecycle probes; active entries are owned exclusively
        /// by the render thread.
        [[nodiscard]] UploadLifecycleSnapshot uploadLifecycle() const noexcept;

        /// Render-thread close phase used after client admission is stopped.
        /// Drains already accepted control/upload packets, joins the transfer
        /// owner, settles every tracked upload, and publishes terminal replies.
        [[nodiscard]] UploadLifecycleSnapshot closeAcceptedUploads() noexcept;

        class Impl;

    protected:
        GeneralRenderServer(
            std::shared_ptr<Channel> frame_channel,
            std::shared_ptr<RenderControlChannel<>> control_channel,
            std::shared_ptr<RenderUploadChannel<>> upload_channel,
            std::shared_ptr<RenderChannelSync> sync,
            std::unique_ptr<Impl> impl
        );

        /// Try to flush pending upload-completion replies without a request.
        /// Opens a standalone reply-only frame, publishes, and returns.
        void flushDeferredRepliesOnly();

        /// Advance + flush in-flight async readbacks (ReadbackTargetAsync):
        /// settle, submit the GPU copy, poll the fence, and send the deferred
        /// reply (by request_id) on completion. Called once per tick().
        void pollPendingReadbacks();

        /// 两阶段销毁后半程:fence 水位越过后拆 swapchain/surface,按
        /// request_id 延迟回执 TargetReleased(宿主此后才可销毁平台窗口)。
        [[nodiscard]] bool stepPendingSurfaceReleases();

        /// 把本 tick 汇集到的自发上报打包成一帧无请求回复推给客户端。与上面三个
        /// 同构:环满或通道关闭就原地留着,下 tick 重试,绝不阻塞渲染线程。
        void flushErrorEvents();

    public:
        /// 记一条自发上报:**没有调用方在等结果**的失败走这里(每帧路径上的失败、
        /// Vulkan 校验层回调、交换链实际选中的呈现模式……)。有调用方的失败一律走
        /// 返回值,不要重复上报。
        ///
        /// 渲染线程调用。同键(场景 + 错误类型 + 实参)在同一批内合并计数,所以
        /// 每帧调用它是安全的 —— 限流是通道的职责,失败点不必自己写。
        void reportError(const RenderError& error,
                         std::uint32_t scene_index = RenderErrorEvent::kNoScene) noexcept;

    protected:

        // ── 派生服务器的扩展面 ───────────────────────────────────
        //
        // 存在的理由:UIRenderServer 此前直接 include 基类的**私有** Impl 头
        // (sinclude/.../RenderServerImpl.hpp),于是 Impl 的每一个成员都成了
        // 它的接口 —— 改 Impl 的任何字段都可能悄悄弄坏 UI。真正的封装破口是
        // 那个 include,不是继承本身;这一组 protected 成员就是用来替掉它的。
        //
        // 为什么是 protected 而不是 public:这些是**扩展面**,给派生的渲染
        // 服务器用;把帧阶段与目标注册表交给任意调用方驱动,是另一回事。
        //
        // 为什么按引用收发前置声明的类型:本头是**安装**头,而
        // FrameTickState / RenderTargetRegistry 住在树内的 sinclude。只声明
        // 引用,安装面就不必认识 FrameRuntime、RenderTargetEntry 和一串
        // Vulkan 句柄 —— 派生类在树内 include 那两个头即可。

        /// 一 tick 的三段。tick() 就是它们的串联。派生类要在段间插自己的事情
        /// 时,按同样顺序调用它们,**而不是整份复制 tick()**。
        enum class ETickStage
        {
            NoTarget,   ///< 无可渲染 target —— 未开帧,直接返回 true,无需收尾
            Skipped,    ///< 已开帧但无命令缓冲(最小化/重建失败)—— 已代为收尾
            Failed,     ///< 帧槽 Vulkan 状态失败——已上报并请求停止 device session
            Ready,      ///< 正常,继续 renderRenderTick / endRenderTick
        };

        /// 阶段 0:生成帧戳 → 排水请求 → 冲刷延迟回复。返回 false = 通道已停,
        /// tick 应返回 false。
        ///
        /// 与阶段 1 分开,是因为**这中间是派生类唯一能看到"本帧要画什么"的时刻**:
        /// 请求已排空(快照、事件、命令都已就位),而 acquire 还没发生。合成链上
        /// 那些"每帧变化"的层必须在这里同步 —— 一旦进了 beginRenderTick,
        /// 编排就会按 layers 决定 acquire 哪些呈现面,来不及了。
        [[nodiscard]] bool drainTick();

        /// 阶段 1:开帧(重建 → 等栅栏 → acquire)→ 退休上一槽记账。
        /// **必须先调 drainTick()。** 返回 Ready 之外的值时调用方应立刻结束本 tick；
        /// Failed 必须作为 tick() 的 false 返回给渲染线程宿主。
        [[nodiscard]] ETickStage beginRenderTick(FrameTickState& fs);

        /// 阶段 2:上传 → 逐 target 逐层渲染 → 各被触及场景 endViewFrame。
        void renderRenderTick(FrameTickState& fs);

        /// 阶段 3:结帧提交/呈现 → 目标池老化 → Surface 拆除步进 → 异步回读
        /// → 收尾记账。
        [[nodiscard]] bool endRenderTick(FrameTickState& fs);

        /// 本实例拥有的全部渲染目标(离屏 / 呈现面)及其合成链。
        [[nodiscard]] RenderTargetRegistry& targets() noexcept;
        /// const 重载 —— 供 const 查询方法用(如"主窗绑了场景没有")。
        [[nodiscard]] const RenderTargetRegistry& targets() const noexcept;

        [[nodiscard]] Renderer&        renderer()        noexcept;
        [[nodiscard]] ResourceContext&  resourceContext() noexcept;
        [[nodiscard]] DeviceContext*    deviceContext()   noexcept;

        /// Last boundary for permanent frame-runtime failures. It emits through
        /// the configured render diagnostic channel, gives that channel one
        /// non-blocking flush opportunity, and stops the server loop.
        [[nodiscard]] bool stopAfterFrameError(
            const RenderError& error,
            std::uint32_t phase);
        [[nodiscard]] SwapchainProvider* swapchainProvider() noexcept;
        [[nodiscard]] const FrameStamp& currentStamp()   const noexcept;
        [[nodiscard]] uint32_t          framesInFlight() const noexcept;
        /// 栅栏证实的 GPU 完成水位(无 FrameDriver 时退化为当前 serial)。
        [[nodiscard]] uint64_t          gpuCompletedSerial() const noexcept;

        /// 派生类的不透明自有数据 + 场景销毁前回调。基类不拥有该指针。
        void  setExtension(void* extension, PreDestroySceneCallback pre_destroy) noexcept;
        [[nodiscard]] void* extension() const noexcept;

        /// 把一个 Surface target 的呈现机件转入两阶段销毁的在途账本:停止
        /// 呈现(entry 已除名),等 fence 水位越过 retire_serial 再拆,拆完
        /// 执行 on_teardown。内部释放不发 TargetReleased 回执。
        void deferSurfaceRelease(RenderTargetId target,
                                 std::unique_ptr<PresentContext> ctx,
                                 std::function<void()> on_teardown);

        /// 关服路径:GPU 已 idle 时立即执行并清空全部在途 Surface 拆除。
        void flushPendingSurfaceReleases();

        std::unique_ptr<Impl>           impl_;
        std::unique_ptr<RenderControlServer> control_server_;
        std::unique_ptr<RenderUploadServer> upload_server_;
    };

} // namespace lux::render
