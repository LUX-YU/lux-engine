#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
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
        IpcMessage() noexcept {} // inline tail remains uninitialized
        ~IpcMessage() noexcept;
        IpcMessage(IpcMessage&& other) noexcept;
        IpcMessage& operator=(IpcMessage&& other) noexcept;
        IpcMessage(const IpcMessage&) = delete;
        IpcMessage& operator=(const IpcMessage&) = delete;

        // false iff size exceeds 128; on failure this message is unchanged.
        [[nodiscard]] bool tryAssignInline(std::span<const std::byte> bytes) noexcept;

        // One owner keeps primary, descriptors and attachment backing stable. No Payload copy.
        // false iff owner is null or count exceeds four; neither source nor destination changes.
        template <typename Owner>
        [[nodiscard]] bool tryAdopt(
            std::unique_ptr<Owner>& owner,
            std::span<const std::byte> bytes,
            std::span<const std::span<const std::byte>> attachments = {}
        ) noexcept
        {
            const bool is_invalid_input = !owner || attachments.size() > attachment_capacity;
            if (is_invalid_input) return false;
            reset();
            std::construct_at(&storage_.large, Owned{
                bytes.data(), owner.release(), [](void* value) noexcept {
                    if constexpr (std::is_array_v<Owner>) delete[] static_cast<std::remove_extent_t<Owner>*>(value);
                    else delete static_cast<Owner*>(value);
                }, attachments
            });
            owned_ = true;
            size_ = bytes.size();
            return true;
        }

        [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
        [[nodiscard]] std::span<const std::span<const std::byte>> attachments() const noexcept
        {
            return owned_ ? storage_.large.attachments : std::span<const std::span<const std::byte>>{};
        }
        void reset() noexcept;

    private:
        struct Owned final
        {
            const std::byte* data;
            void* owner;
            void (*destroy)(void*) noexcept;
            std::span<const std::span<const std::byte>> attachments;
        };
        union Storage
        {
            std::array<std::byte, inline_capacity> small;
            Owned large;
            Storage() noexcept { ::new (static_cast<void*>(&small)) std::array<std::byte, inline_capacity>; }
        } storage_;
        std::size_t size_ = 0;
        bool owned_ = false;
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
