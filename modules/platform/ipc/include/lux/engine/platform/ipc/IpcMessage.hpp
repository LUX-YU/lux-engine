#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/platform/ipc/visibility.h>

namespace lux::platform::ipc
{
    enum class EIpcReceive : std::uint8_t { RECEIVED, EMPTY, CLOSED };

    enum class EIpcError { INVALID_ARGUMENT, ALREADY_HAS_RESPONDER, NOT_READY, CLOSED, WOULD_BLOCK, INVALID_SOURCE };

    template <typename T>
    using Result = cxx::expected<T, EIpcError>;

    // The owner is unique to this message. An upper layer may use an exclusive
    // holder retaining a pre-existing shared asset. Its deleter code must stay loaded.
    class LUX_PLATFORM_IPC_PUBLIC IpcMessage final
    {
    public:
        static constexpr std::size_t inline_capacity = 128;
        static constexpr std::size_t attachment_capacity = 4;
        IpcMessage() noexcept {} // inline tail intentionally uninitialized, including value initialization
        ~IpcMessage() noexcept;
        IpcMessage(IpcMessage&& other) noexcept;
        IpcMessage& operator=(IpcMessage&& other) noexcept;
        IpcMessage(const IpcMessage&) = delete;
        IpcMessage& operator=(const IpcMessage&) = delete;

        [[nodiscard]] Result<void> assignInline(std::span<const std::byte> bytes) noexcept;
        [[nodiscard]] static Result<IpcMessage>
        takeBytes(std::unique_ptr<std::byte[]>& owner, std::size_t size) noexcept; // caller proves extent

        // bytes must belong to owner and remain stable until its destruction.
        // Failure leaves owner intact. No byte copy or shared control block is created.
        template <typename Owner>
        [[nodiscard]] static Result<IpcMessage>
        takeOwner(std::unique_ptr<Owner>& owner, std::span<const std::byte> bytes) noexcept
        {
            return takeOwner(owner, bytes, {});
        }

        // The descriptor array AND its byte ranges belong to owner and remain stable.
        // Only descriptors are borrowed; there is still exactly one IPC owner/deleter.
        template <typename Owner>
        [[nodiscard]] static Result<IpcMessage> takeOwner(
            std::unique_ptr<Owner>& owner,
            std::span<const std::byte> bytes,
            std::span<const std::span<const std::byte>> attachments
        ) noexcept
        {
            const bool is_invalid_input = !owner || attachments.size() > attachment_capacity;
            if (is_invalid_input) return cxx::unexpected(EIpcError::INVALID_ARGUMENT);
            IpcMessage result;
            result.data_ = bytes.data();
            result.size_ = bytes.size();
            result.attachments_ = attachments;
            result.destroy_ = [](void* value) noexcept { delete static_cast<Owner*>(value); };
            result.owner_ = owner.release();
            return result;
        }

        [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
        [[nodiscard]] std::span<const std::span<const std::byte>> attachments() const noexcept
        {
            return attachments_;
        }
        void reset() noexcept;

    private:
        std::array<std::byte, inline_capacity> inline_; // only [0, size_) is initialized when inline
        const std::byte* data_ = nullptr;
        void* owner_ = nullptr;
        void (*destroy_)(void*) noexcept = nullptr;
        std::size_t size_ = 0;
        std::span<const std::span<const std::byte>> attachments_;
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
