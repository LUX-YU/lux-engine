#pragma once
#include <lux/engine/function/render/client/core/RenderError.hpp> // GenericOkReply::error

#include <concepts>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::render
{
    // -----------------------------------------------------------------------------
    //  Core payload traits
    // -----------------------------------------------------------------------------

    template <typename T>
    concept FrameBlobPayload = std::is_trivially_copyable_v<T>;

    // -----------------------------------------------------------------------------
    //  Aligned allocator for the payload blob
    // -----------------------------------------------------------------------------

    template <typename T, std::size_t Alignment> class AlignedAllocator
    {
    public:
        using value_type = T;

        AlignedAllocator() noexcept = default;

        template <typename U> constexpr AlignedAllocator(const AlignedAllocator<U, Alignment>&) noexcept
        {
        }

        [[nodiscard]] T* allocate(std::size_t n)
        {
            if (n > (std::numeric_limits<std::size_t>::max)() / sizeof(T))
                std::abort();

            void* p = ::operator new(n * sizeof(T), std::align_val_t{Alignment}, std::nothrow);
            if (!p)
                std::abort();
            return static_cast<T*>(p);
        }

        void deallocate(T* p, std::size_t) noexcept
        {
            ::operator delete(p, std::align_val_t{Alignment});
        }

        template <typename U> struct rebind
        {
            using other = AlignedAllocator<U, Alignment>;
        };

        using is_always_equal = std::true_type;
    };

    template <typename T, typename U, std::size_t Alignment>
    constexpr bool operator==(const AlignedAllocator<T, Alignment>&, const AlignedAllocator<U, Alignment>&) noexcept
    {
        return true;
    }

    template <typename T, typename U, std::size_t Alignment>
    constexpr bool operator!=(const AlignedAllocator<T, Alignment>&, const AlignedAllocator<U, Alignment>&) noexcept
    {
        return false;
    }

    // -----------------------------------------------------------------------------
    //  Protocol primitives
    // -----------------------------------------------------------------------------

    using TypeId = std::uint32_t;
    using RequestId = std::uint32_t;
    using OpCode = std::uint8_t;

    inline constexpr TypeId kInvalidTypeId = std::numeric_limits<TypeId>::max();
    inline constexpr RequestId kInvalidRequestId = std::numeric_limits<RequestId>::max();

    // Reply type id for a generic "the command failed during dispatch" reply.
    // A reserved high value so it never collides with a hand-assigned type_ids reply
    // id. Lives in this base comm header so BOTH the client (RenderRequest /
    // ResponseCallbackStore) and the server/protocol can name it without a circular
    // include (RenderProtocol.hpp aliases it as type_ids::ReplyCommandFailed).
    inline constexpr TypeId kReplyCommandFailedTypeId = 0xFFFFFF01u;

    // TypeId encodes a 16-bit slot index (low) and 16-bit generation (high).
    // Static type_ids have generation 0 — they are never recycled.
    inline constexpr TypeId makeTypeId(std::uint16_t index, std::uint16_t generation) noexcept
    {
        return static_cast<TypeId>(index) | (static_cast<TypeId>(generation) << 16);
    }

    inline constexpr std::uint16_t typeIdIndex(TypeId id) noexcept
    {
        return static_cast<std::uint16_t>(id & 0xFFFF);
    }

    inline constexpr std::uint16_t typeIdGen(TypeId id) noexcept
    {
        return static_cast<std::uint16_t>(id >> 16);
    }

    namespace opcodes
    {
        inline constexpr OpCode BulkData = 0;
        inline constexpr OpCode ResourceOp = 1;
        inline constexpr OpCode CommandOp = 2;
        inline constexpr OpCode Attachment = 3;
        inline constexpr OpCode Debug = 4;
    }

    /// 附件的**生命周期机制**标签 —— 不是分发键。
    ///
    /// 分发完全走 `CmdRecord::type_id`(op id);附件类型只是 `AttachmentRecord`
    /// 上的自证标签,供消费方在 `static_cast` 前做一次等值守卫。
    ///
    /// **1/2/4/5 为引擎保留;其余值归各模块自取。**
    ///
    /// ⚠️ 这里**没有分配机制**(对比 op 侧的 allocateSlot / freeSlot / generation /
    /// 名字索引),所以模块间不撞号只能靠这条约定。谁新加一个类型,自己往下取值,
    /// 并在自己的头里定义 —— 不要加回这里:引擎不认识、也不该认识那些类型。
    ///
    /// 已知在用:3 = ImGuiDrawData(定义在 `lux/engine/ui/ImGuiCommConfig.hpp`)。
    namespace attachment_types
    {
        inline constexpr TypeId BorrowedBytes = 1;
        inline constexpr TypeId OwnedBytes = 2; ///< Lifetime-managing owner; not resolved directly
        /// Owns a module or package while render-thread function pointers remain
        /// registered. The payload is std::shared_ptr<const void>.
        inline constexpr TypeId LifetimeLease = 4;
        /// The attachment record directly owns one concrete object and exposes
        /// `object/object_size`; unlike BorrowedBytes/OwnedBytes there is no
        /// intermediate byte-range wrapper to unwrap on the render thread.
        inline constexpr TypeId OwnedObject = 5;
    }

    enum class CmdFlags : std::uint16_t
    {
        None = 0,
        ExpectsReply = 1u << 0,
        DeferredReply = 1u << 1,
    };

    inline constexpr std::uint16_t toUnderlying(CmdFlags flags) noexcept
    {
        return static_cast<std::uint16_t>(flags);
    }

    inline constexpr CmdFlags operator|(CmdFlags lhs, CmdFlags rhs) noexcept
    {
        return static_cast<CmdFlags>(toUnderlying(lhs) | toUnderlying(rhs));
    }

    inline constexpr CmdFlags operator&(CmdFlags lhs, CmdFlags rhs) noexcept
    {
        return static_cast<CmdFlags>(toUnderlying(lhs) & toUnderlying(rhs));
    }

    inline constexpr std::uint16_t operator|(std::uint16_t lhs, CmdFlags rhs) noexcept
    {
        return static_cast<std::uint16_t>(lhs | toUnderlying(rhs));
    }

    inline constexpr bool hasFlag(std::uint16_t flags, CmdFlags bit) noexcept
    {
        return (flags & toUnderlying(bit)) != 0;
    }

    constexpr std::size_t alignUp(std::size_t value, std::size_t alignment) noexcept
    {
        return (value + (alignment - 1)) & ~(alignment - 1);
    }

    template <typename T> struct CommandTraits
    {
        static constexpr bool has_reply = false;
        static constexpr TypeId reply_type_id = kInvalidTypeId;
    };

    /// 通用"操作成功"回复。此前定义在 comm/RenderProtocol.hpp 里,但它既不含
    /// 领域语义也不依赖任何线协议类型 —— 有两个特性头已经在**前置声明**它而不
    /// 包含那个重头,正说明它归错了层。与 CommandTraits 主模板同处。
    struct GenericOkReply
    {
        uint32_t code{0};
        /// code != 0 时说明为什么。此前只有一个不透明的 code —— 调用方看得出失败了,
        /// 看不出失败在哪,于是原因只能同时打一份到服务端 stderr。
        RenderError error{};
    };

    /// 一条命令在**分发期**失败的方式(还没到 handler 的业务逻辑)。
    enum class EDispatchFailure : uint32_t
    {
        Unspecified = 0,
        InvalidOpcode = 1,
        PayloadOutOfBounds = 2,
        UnknownTypeId = 3,
        TypeIdGeneration = 4,
        HandlerRejected = 5,
        PayloadValidation = 6,
    };

    // Payload of a ReplyCommandFailed reply (kReplyCommandFailedTypeId, above).
    // `code` classifies the dispatch failure; the failing request is identified by
    // the ReplyRecord's request_id, not by this payload. 与 GenericOkReply 同理由
    // 从 comm/RenderProtocol.hpp 下沉:客户端的 RenderRequest 要**解码**它(把
    // error 存进请求的失败态),不该为此拖进整个线协议头。
    struct CommandFailedReply
    {
        uint32_t code{0}; ///< EDispatchFailure
        /// 分发失败的具体那一条,实参槽里带着定位所需的数字(越界的 opcode、
        /// 出界的载荷偏移与长度、认不出的 TypeId 下标)。此前这些数字只打在
        /// 服务端 stderr 上,客户端拿到的是一个粗粒度的 code。
        RenderError error{};
    };
    static_assert(std::is_trivially_copyable_v<CommandFailedReply>);

    template <typename T> inline constexpr bool command_has_reply_v = CommandTraits<T>::has_reply;

    struct CmdRecord
    {
        OpCode opcode{0};
        std::uint8_t reserved0{0};
        std::uint16_t flags{0};
        TypeId type_id{kInvalidTypeId};
        std::uint32_t payload_offset{0};
        std::uint32_t payload_size{0};
        RequestId request_id{kInvalidRequestId};
    };
    static_assert(sizeof(CmdRecord) == 20);
    static_assert(std::is_trivially_copyable_v<CmdRecord>);

    struct ReplyRecord
    {
        TypeId type_id{kInvalidTypeId};
        std::uint16_t flags{0};
        std::uint16_t reserved0{0};
        std::uint32_t payload_offset{0};
        std::uint32_t payload_size{0};
        RequestId request_id{kInvalidRequestId};
    };
    static_assert(sizeof(ReplyRecord) == 20);
    static_assert(std::is_trivially_copyable_v<ReplyRecord>);

    struct BlobRef
    {
        std::uint32_t offset{0};
        std::uint32_t size{0};
    };
    static_assert(std::is_trivially_copyable_v<BlobRef>);

    using StringRef = BlobRef;

    struct ExternalDataRef
    {
        std::uint32_t attachment_index{0};
        std::uint32_t offset{0};
        std::uint32_t size{0};
    };
    static_assert(std::is_trivially_copyable_v<ExternalDataRef>);

    struct FrameMemoryHints
    {
        std::size_t command_capacity{256};
        std::size_t payload_capacity{64 * 1024};
        std::size_t attachment_capacity{8};
        std::size_t reply_capacity{64};
        std::size_t reply_payload_capacity{8 * 1024};
    };

    // -----------------------------------------------------------------------------
    //  Cold-path attachments
    // -----------------------------------------------------------------------------

    struct AttachmentRecord
    {
        TypeId type_id{kInvalidTypeId};
        void* object{nullptr};
        std::size_t object_size{0};
        std::size_t accounted_size{0};
        void (*destroy)(void*) noexcept {nullptr};

        AttachmentRecord() noexcept = default;

        AttachmentRecord(
            TypeId type,
            void* value,
            std::size_t value_size,
            std::size_t charged_size,
            void (*deleter)(void*) noexcept
        ) noexcept
            : type_id(type), object(value), object_size(value_size), accounted_size(charged_size), destroy(deleter)
        {
        }

        ~AttachmentRecord()
        {
            reset();
        }

        AttachmentRecord(const AttachmentRecord&) = delete;
        AttachmentRecord& operator=(const AttachmentRecord&) = delete;

        AttachmentRecord(AttachmentRecord&& other) noexcept
            : type_id(std::exchange(other.type_id, kInvalidTypeId)), object(std::exchange(other.object, nullptr)),
              object_size(std::exchange(other.object_size, 0)), accounted_size(std::exchange(other.accounted_size, 0)),
              destroy(std::exchange(other.destroy, nullptr))
        {
        }

        AttachmentRecord& operator=(AttachmentRecord&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                type_id = std::exchange(other.type_id, kInvalidTypeId);
                object = std::exchange(other.object, nullptr);
                object_size = std::exchange(other.object_size, 0);
                accounted_size = std::exchange(other.accounted_size, 0);
                destroy = std::exchange(other.destroy, nullptr);
            }
            return *this;
        }

        void reset() noexcept
        {
            if (object && destroy)
            {
                destroy(object);
            }
            type_id = kInvalidTypeId;
            object = nullptr;
            object_size = 0;
            accounted_size = 0;
            destroy = nullptr;
        }
    };

    struct BorrowedBytesAttachment
    {
        const std::byte* data{nullptr};
        std::uint32_t size{0};
    };

    /// Owns or aliases a byte range via shared ownership.
    ///
    /// - `owner` keeps the backing storage alive.
    /// - `data` / `size` describe the byte slice consumed by a command.
    ///
    /// This is intended for async resource pipelines where borrowed pointers
    /// are not safe beyond the frame packet lifetime.
    struct OwnedBytesAttachment
    {
        std::shared_ptr<const void> owner{};
        const std::byte* data{nullptr};
        std::uint32_t size{0};
    };
}
