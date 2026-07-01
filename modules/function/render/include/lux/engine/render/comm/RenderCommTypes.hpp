#pragma once
#include <concepts>
#include <array>
#include <cassert>
#include <cstddef>
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

    template <typename T, std::size_t Alignment>
    class AlignedAllocator
    {
    public:
        using value_type = T;

        AlignedAllocator() noexcept = default;

        template <typename U>
        constexpr AlignedAllocator(const AlignedAllocator<U, Alignment>&) noexcept
        {
        }

        [[nodiscard]] T* allocate(std::size_t n)
        {
            if (n > (std::numeric_limits<std::size_t>::max)() / sizeof(T))
            {
                throw std::bad_alloc{};
            }

            void* p = ::operator new(n * sizeof(T), std::align_val_t{Alignment});
            return static_cast<T*>(p);
        }

        void deallocate(T* p, std::size_t) noexcept
        {
            ::operator delete(p, std::align_val_t{Alignment});
        }

        template <typename U>
        struct rebind
        {
            using other = AlignedAllocator<U, Alignment>;
        };

        using is_always_equal = std::true_type;
    };

    template <typename T, typename U, std::size_t Alignment>
    constexpr bool operator==(const AlignedAllocator<T, Alignment>&,
                              const AlignedAllocator<U, Alignment>&) noexcept
    {
        return true;
    }

    template <typename T, typename U, std::size_t Alignment>
    constexpr bool operator!=(const AlignedAllocator<T, Alignment>&,
                              const AlignedAllocator<U, Alignment>&) noexcept
    {
        return false;
    }

    // -----------------------------------------------------------------------------
    //  Protocol primitives
    // -----------------------------------------------------------------------------

    using TypeId      = std::uint32_t;
    using RequestId   = std::uint32_t;
    using OpCode      = std::uint8_t;

    inline constexpr TypeId     kInvalidTypeId    = std::numeric_limits<TypeId>::max();
    inline constexpr RequestId  kInvalidRequestId = std::numeric_limits<RequestId>::max();

    // Reply type id for a generic "the command failed during dispatch" reply (P0-4).
    // A reserved high value so it never collides with a hand-assigned type_ids reply
    // id. Lives in this base comm header so BOTH the client (RenderRequest /
    // ResponseCallbackStore) and the server/protocol can name it without a circular
    // include (RenderProtocol.hpp aliases it as type_ids::ReplyCommandFailed).
    inline constexpr TypeId     kReplyCommandFailedTypeId = 0xFFFFFF01u;

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

    namespace attachment_types
    {
        inline constexpr TypeId BorrowedBytes = 1;
        inline constexpr TypeId OwnedBytes    = 2; ///< Lifetime-managing owner; not resolved directly
        inline constexpr TypeId ImGuiDrawData = 3; ///< Owned ImDrawDataSnapshot for cross-thread ImGui rendering
    }

    enum class CmdFlags : std::uint16_t
    {
        None          = 0,
        ExpectsReply  = 1u << 0,
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

    template <typename T>
    struct CommandTraits
    {
        static constexpr bool has_reply = false;
        static constexpr TypeId reply_type_id = kInvalidTypeId;
    };

    template <typename T>
    inline constexpr bool command_has_reply_v = CommandTraits<T>::has_reply;

    struct CmdRecord
    {
        OpCode        opcode{0};
        std::uint8_t  reserved0{0};
        std::uint16_t flags{0};
        TypeId        type_id{kInvalidTypeId};
        std::uint32_t payload_offset{0};
        std::uint32_t payload_size{0};
        RequestId     request_id{kInvalidRequestId};
    };
    static_assert(sizeof(CmdRecord) == 20);
    static_assert(std::is_trivially_copyable_v<CmdRecord>);

    struct ReplyRecord
    {
        TypeId        type_id{kInvalidTypeId};
        std::uint16_t flags{0};
        std::uint16_t reserved0{0};
        std::uint32_t payload_offset{0};
        std::uint32_t payload_size{0};
        RequestId     request_id{kInvalidRequestId};
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
        TypeId  type_id{kInvalidTypeId};
        void*   object{nullptr};
        size_t  object_size{0};
        void (*destroy)(void*) noexcept {nullptr};

        void reset() noexcept
        {
            if (object && destroy)
            {
                destroy(object);
            }
            type_id = kInvalidTypeId;
            object = nullptr;
            destroy = nullptr;
        }
    };

    struct BorrowedBytesAttachment
    {
        const std::byte*      data{nullptr};
        std::uint32_t         size{0};
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
        const std::byte*            data{nullptr};
        std::uint32_t               size{0};
    };
}