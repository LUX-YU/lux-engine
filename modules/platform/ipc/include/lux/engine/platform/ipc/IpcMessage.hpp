#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/platform/ipc/visibility.h>

#ifndef LUX_IPC_INLINE_CAPACITY
#define LUX_IPC_INLINE_CAPACITY 128
#endif

namespace lux::platform::ipc
{
    enum class EIpcError { INVALID_ARGUMENT, ALREADY_HAS_RESPONDER, NOT_READY, CLOSED, WOULD_BLOCK, INVALID_SOURCE };

    template <typename T>
    using Result = cxx::expected<T, EIpcError>;

    // The owner is unique to this message. An upper layer may use an exclusive
    // holder retaining a pre-existing shared asset. Its deleter code must stay loaded.
    class LUX_PLATFORM_IPC_PUBLIC IpcMessage final
    {
    public:
        static constexpr std::size_t inline_capacity = LUX_IPC_INLINE_CAPACITY;
        IpcMessage() noexcept = default;
        ~IpcMessage() noexcept;
        IpcMessage(IpcMessage&& other) noexcept;
        IpcMessage& operator=(IpcMessage&& other) noexcept;
        IpcMessage(const IpcMessage&) = delete;
        IpcMessage& operator=(const IpcMessage&) = delete;

        [[nodiscard]] static Result<IpcMessage> copyInline(std::span<const std::byte> bytes) noexcept;
        [[nodiscard]] static Result<IpcMessage>
        takeBytes(std::unique_ptr<std::byte[]>& owner, std::size_t size) noexcept;

        // bytes must belong to owner and remain stable until its destruction.
        // Failure leaves owner intact. No byte copy or shared control block is created.
        template <typename Owner>
        [[nodiscard]] static Result<IpcMessage>
        takeOwner(std::unique_ptr<Owner>& owner, std::span<const std::byte> bytes) noexcept
        {
            const bool is_missing_owner = !owner;
            const bool is_invalid_bytes = !bytes.empty() && !bytes.data();
            if (is_missing_owner || is_invalid_bytes) return cxx::unexpected(EIpcError::INVALID_ARGUMENT);
            IpcMessage result;
            result.data_ = bytes.data();
            result.size_ = bytes.size();
            result.destroy_ = [](void* value) noexcept { delete static_cast<Owner*>(value); };
            result.owner_ = owner.release();
            return result;
        }

        [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
        void reset() noexcept;

    private:
        std::array<std::byte, inline_capacity> inline_{};
        const std::byte* data_ = nullptr;
        void* owner_ = nullptr;
        void (*destroy_)(void*) noexcept = nullptr;
        std::size_t size_ = 0;
    };

    enum class EIpcDelivery { REPLY, ABORTED };

    struct IpcReply final
    {
        EIpcDelivery delivery = EIpcDelivery::REPLY;
        IpcMessage message;
        // Returned input owner; also preserves opaque correlation on ABORTED.
        IpcMessage original_request;
    };
}
