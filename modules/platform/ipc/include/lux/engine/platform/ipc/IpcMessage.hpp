#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/platform/ipc/visibility.h>

namespace lux::platform::ipc
{
    enum class EIpcReceive : std::uint8_t { RECEIVED, EMPTY, CLOSED };
    enum class EIpcError { INVALID_ARGUMENT, ALREADY_HAS_RESPONDER, NOT_READY, CLOSED, WOULD_BLOCK, INVALID_SOURCE };

    template <typename T>
    using Result = cxx::expected<T, EIpcError>;

    class IpcMessage;
    class IpcTypeOps;

    namespace detail
    {
        LUX_PLATFORM_IPC_PUBLIC void
        moveFrom(IpcMessage&, const IpcTypeOps&, void*, std::shared_ptr<const void>) noexcept;
        [[nodiscard]] LUX_PLATFORM_IPC_PUBLIC void* borrowObject(IpcMessage&) noexcept;
        [[nodiscard]] LUX_PLATFORM_IPC_PUBLIC const void* borrowObject(const IpcMessage&) noexcept;
    }

    // Static per-type operations. Local table address equality is not a cross-module ABI proof.
    class IpcTypeOps final
    {
    public:
        IpcTypeOps(const IpcTypeOps&) = delete;
        IpcTypeOps& operator=(const IpcTypeOps&) = delete;
        [[nodiscard]] cxx::TypeToken token() const noexcept { return token_; }
        [[nodiscard]] std::size_t size() const noexcept { return size_; }
        [[nodiscard]] std::size_t alignment() const noexcept { return alignment_; }
        [[nodiscard]] std::array<std::uint64_t, 2> abi() const noexcept { return abi_; }
        [[nodiscard]] bool usesInlineStorage() const noexcept { return inline_storage_; }

    private:
        friend class IpcMessage;
        friend void detail::moveFrom(IpcMessage&, const IpcTypeOps&, void*, std::shared_ptr<const void>) noexcept;
        friend void* detail::borrowObject(IpcMessage&) noexcept;
        friend const void* detail::borrowObject(const IpcMessage&) noexcept;
        using Move = void (*)(void*, void*) noexcept;
        using Destroy = void (*)(void*) noexcept;
        using Bytes = std::span<const std::byte> (*)(const void*) noexcept;
        using Attachments = std::span<const std::span<const std::byte>> (*)(const void*) noexcept;
        using MovedBytes = std::size_t (*)(const void*) noexcept;

        constexpr IpcTypeOps(cxx::TypeToken token, std::size_t size, std::size_t alignment,
            std::array<std::uint64_t, 2> abi, bool inline_storage, Move construct_from, Move move,
            Destroy destroy, Bytes bytes,
            Attachments attachments, MovedBytes moved_bytes) noexcept
            : token_(token), size_(size), alignment_(alignment), abi_(abi), construct_from_(construct_from),
              move_(move), destroy_(destroy),
              bytes_(bytes), attachments_(attachments), moved_bytes_(moved_bytes), inline_storage_(inline_storage) {}

        cxx::TypeToken token_;
        std::size_t size_;
        std::size_t alignment_;
        std::array<std::uint64_t, 2> abi_;
        Move construct_from_;
        Move move_;
        Destroy destroy_;
        Bytes bytes_;
        Attachments attachments_;
        MovedBytes moved_bytes_;
        bool inline_storage_;
    };

    // Cold validation creates this qualification. The table is static and code_pin keeps its module loaded.
    template <typename T>
    class IpcBoundType final
    {
    public:
        [[nodiscard]] const IpcTypeOps& operations() const noexcept { return *operations_; }

    private:
        friend class IpcMessage;
        IpcBoundType(const IpcTypeOps& operations, std::shared_ptr<const void> pin) noexcept
            : operations_(&operations), code_pin_(std::move(pin)) {}
        const IpcTypeOps* operations_;
        std::shared_ptr<const void> code_pin_;
    };

    // A real move-only C++ object owner. IPC never interprets the opaque metadata or payload contract.
    class alignas(16) LUX_PLATFORM_IPC_PUBLIC IpcMessage final
    {
    public:
        static constexpr std::size_t metadata_capacity = 40;
        // Transitional capacity preserves 128-byte byte callers until the production Wire migration finishes.
        static constexpr std::size_t typed_inline_capacity = 144;
        static constexpr std::size_t inline_capacity = 128;
        static constexpr std::size_t attachment_capacity = 4;
        IpcMessage() noexcept = default;
        ~IpcMessage() noexcept;
        IpcMessage(IpcMessage&& other) noexcept;
        IpcMessage& operator=(IpcMessage&& other) noexcept;
        IpcMessage(const IpcMessage&) = delete;
        IpcMessage& operator=(const IpcMessage&) = delete;

        template <typename T>
        static constexpr bool stores_inline = sizeof(T) <= typed_inline_capacity && alignof(T) <= 16 &&
            std::is_nothrow_move_constructible_v<T>;

        template <typename T>
        [[nodiscard]] static const IpcTypeOps& type() noexcept;

        template <typename T>
        [[nodiscard]] static Result<IpcBoundType<T>>
        bindType(const IpcTypeOps& canonical, std::shared_ptr<const void> code_pin = {}) noexcept
        {
            const auto& local = type<T>();
            const bool is_type_mismatch = local.token() != canonical.token();
            const bool is_layout_mismatch = local.size() != canonical.size() ||
                local.alignment() != canonical.alignment();
            const bool is_abi_mismatch = local.abi() != canonical.abi();
            if (is_type_mismatch || is_layout_mismatch || is_abi_mismatch)
                return cxx::unexpected(EIpcError::INVALID_ARGUMENT);
            return IpcBoundType<T>{canonical, std::move(code_pin)};
        }

        template <typename T, typename... Args>
        requires std::is_nothrow_constructible_v<T, Args...>
        T& emplace(Args&&... args) noexcept
        {
            return construct<T>(type<T>(), {}, std::forward<Args>(args)...);
        }

        template <typename T, typename... Args>
        requires std::is_nothrow_constructible_v<T, Args...>
        T& emplace(const IpcBoundType<T>& bound, Args&&... args) noexcept
        {
            return construct<T>(*bound.operations_, bound.code_pin_, std::forward<Args>(args)...);
        }

        [[nodiscard]] const IpcTypeOps* operations() const noexcept { return operations_; }
        [[nodiscard]] bool holds(const IpcTypeOps& canonical) const noexcept { return operations_ == &canonical; }
        [[nodiscard]] bool empty() const noexcept { return operations_ == nullptr; }

        // Precondition: the reception boundary already checked T's canonical qualification once.
        template <typename T>
        [[nodiscard]] T& get() noexcept
        {
            if constexpr (stores_inline<T>) return *std::launder(reinterpret_cast<T*>(object_));
            else return *static_cast<T*>(*std::launder(reinterpret_cast<void**>(object_)));
        }

        template <typename T>
        [[nodiscard]] const T& get() const noexcept
        {
            return storedObject<T>(object_);
        }

        template <typename M>
        requires (std::is_trivially_copyable_v<M> && sizeof(M) <= metadata_capacity)
        void setMetadata(const M& metadata) noexcept { std::memcpy(metadata_.data(), &metadata, sizeof(M)); }

        template <typename M>
        requires (std::is_trivially_copyable_v<M> && std::is_default_constructible_v<M> &&
            sizeof(M) <= metadata_capacity)
        [[nodiscard]] M metadata() const noexcept
        {
            M result{};
            std::memcpy(&result, metadata_.data(), sizeof(M));
            return result;
        }

        // Temporary byte adapters share the same object storage and TypeOps; there is no parallel owner.
        [[nodiscard]] bool tryAssignInline(std::span<const std::byte> bytes) noexcept;

        template <typename Owner>
        [[nodiscard]] bool tryAdopt(std::unique_ptr<Owner>& owner, std::span<const std::byte> bytes,
            std::span<const std::span<const std::byte>> attachments = {}) noexcept
        {
            const bool is_invalid_input = !owner || attachments.size() > attachment_capacity;
            if (is_invalid_input) return false;
            emplace<OwnedBytes>(bytes, owner.release(), [](void* value) noexcept {
                if constexpr (std::is_array_v<Owner>) delete[] static_cast<std::remove_extent_t<Owner>*>(value);
                else delete static_cast<Owner*>(value);
            }, attachments);
            return true;
        }

        [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
        [[nodiscard]] std::span<const std::span<const std::byte>> attachments() const noexcept;
        void reset() noexcept;

    private:
        friend void detail::moveFrom(IpcMessage&, const IpcTypeOps&, void*, std::shared_ptr<const void>) noexcept;
        friend void* detail::borrowObject(IpcMessage&) noexcept;
        friend const void* detail::borrowObject(const IpcMessage&) noexcept;

        template <typename T>
        [[nodiscard]] static const T& storedObject(const void* object) noexcept
        {
            if constexpr (stores_inline<T>) return *std::launder(static_cast<const T*>(object));
            else return *static_cast<const T*>(*std::launder(static_cast<void* const*>(object)));
        }

        struct InlineBytes final
        {
            std::array<std::byte, inline_capacity> values;
            std::size_t size = 0;
            explicit InlineBytes(std::span<const std::byte> source) noexcept : size(source.size())
            {
                if (size) std::memcpy(values.data(), source.data(), size);
            }

            InlineBytes(InlineBytes&& other) noexcept : size(std::exchange(other.size, 0))
            {
                if (size) std::memcpy(values.data(), other.values.data(), size);
            }
        };

        struct OwnedBytes final
        {
            std::span<const std::byte> values;
            void* owner;
            void (*destroy)(void*) noexcept;
            std::span<const std::span<const std::byte>> attachments;
            OwnedBytes(std::span<const std::byte> source, void* value, void (*deleter)(void*) noexcept,
                std::span<const std::span<const std::byte>> views) noexcept
                : values(source), owner(value), destroy(deleter), attachments(views) {}

            OwnedBytes(OwnedBytes&& other) noexcept
                : values(other.values), owner(std::exchange(other.owner, nullptr)), destroy(other.destroy),
                  attachments(other.attachments) {}

            ~OwnedBytes() noexcept { if (owner) destroy(owner); }
        };

        [[nodiscard]] static constexpr std::array<std::uint64_t, 2> compiledAbi() noexcept
        {
            std::uint64_t compiler = 0;
#if defined(_MSC_FULL_VER)
            compiler = _MSC_FULL_VER;
#elif defined(__clang_major__)
            compiler = (std::uint64_t{1} << 48) | (__clang_major__ << 16) |
                (__clang_minor__ << 8) | __clang_patchlevel__;
#elif defined(__GNUC__)
            compiler = (std::uint64_t{2} << 48) | (__GNUC__ << 16) | (__GNUC_MINOR__ << 8) | __GNUC_PATCHLEVEL__;
#endif
            std::uint64_t configuration = sizeof(void*) | (typed_inline_capacity << 8);
#if defined(_ITERATOR_DEBUG_LEVEL)
            configuration |= std::uint64_t{_ITERATOR_DEBUG_LEVEL} << 24;
#endif
#if defined(_DLL)
            configuration |= std::uint64_t{1} << 32;
#endif
#if defined(_DEBUG)
            configuration |= std::uint64_t{1} << 33;
#endif
            return {compiler, configuration};
        }

        template <typename T, typename... Args>
        T& construct(const IpcTypeOps& operations, std::shared_ptr<const void> pin, Args&&... args) noexcept
        {
            static_assert(std::is_nothrow_destructible_v<T>);
            reset();
            code_pin_ = std::move(pin);
            if constexpr (stores_inline<T>)
                std::construct_at(reinterpret_cast<T*>(object_), std::forward<Args>(args)...);
            else
            {
                auto* value = new T(std::forward<Args>(args)...);
                std::construct_at(reinterpret_cast<void**>(object_), static_cast<void*>(value));
            }
            operations_ = &operations;
            return get<T>();
        }

        std::array<std::byte, metadata_capacity> metadata_{};
        const IpcTypeOps* operations_ = nullptr;
        std::shared_ptr<const void> code_pin_;
        alignas(16) std::byte object_[typed_inline_capacity];
    };

    template <typename T>
    const IpcTypeOps& IpcMessage::type() noexcept
    {
        static_assert(std::is_nothrow_destructible_v<T>);
        static constexpr IpcTypeOps value{
            cxx::typeToken<T>(), sizeof(T), alignof(T), compiledAbi(), stores_inline<T>,
            [](void* target, void* source) noexcept {
                // Trusted erased Command construction requires a nothrow-movable live value. Generic IPC
                // emplace also supports immovable objects, whose subsequent message moves transfer pointers.
                if constexpr (std::is_nothrow_move_constructible_v<T>)
                {
                    auto& input = *static_cast<T*>(source);
                    if constexpr (stores_inline<T>) std::construct_at(static_cast<T*>(target), std::move(input));
                    else std::construct_at(static_cast<void**>(target), static_cast<void*>(new T(std::move(input))));
                }
                else std::terminate();
            },
            [](void* target, void* source) noexcept {
                if constexpr (stores_inline<T>)
                {
                    auto* input = std::launder(static_cast<T*>(source));
                    std::construct_at(static_cast<T*>(target), std::move(*input));
                    std::destroy_at(input);
                }
                else
                {
                    auto* input = std::launder(static_cast<void**>(source));
                    std::construct_at(static_cast<void**>(target), *input);
                    std::destroy_at(input);
                }
            },
            [](void* object) noexcept {
                if constexpr (stores_inline<T>) std::destroy_at(std::launder(static_cast<T*>(object)));
                else
                {
                    auto* pointer = std::launder(static_cast<void**>(object));
                    delete static_cast<T*>(*pointer);
                    std::destroy_at(pointer);
                }
            },
            [](const void* object) noexcept -> std::span<const std::byte> {
                if constexpr (std::is_same_v<T, InlineBytes>)
                {
                    const auto& bytes = storedObject<T>(object);
                    return {bytes.values.data(), bytes.size};
                }
                else if constexpr (std::is_same_v<T, OwnedBytes>)
                    return storedObject<T>(object).values;
                else return {};
            },
            [](const void* object) noexcept -> std::span<const std::span<const std::byte>> {
                if constexpr (std::is_same_v<T, OwnedBytes>)
                    return storedObject<T>(object).attachments;
                else return {};
            },
            [](const void* object) noexcept -> std::size_t {
                if constexpr (std::is_same_v<T, InlineBytes>)
                    return storedObject<T>(object).size;
                else return 0;
            }
        };
        return value;
    }

    enum class EIpcDelivery { REPLY, ABORTED };

    struct IpcReply final
    {
        EIpcDelivery delivery = EIpcDelivery::REPLY;
        IpcMessage message;
        IpcMessage original_request;
    };
}
