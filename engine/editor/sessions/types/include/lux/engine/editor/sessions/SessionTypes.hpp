#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace lux::editor::sessions
{
    struct SessionId final
    {
        std::uint64_t value{};
        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return value != 0;
        }
        friend constexpr bool operator==(SessionId, SessionId) noexcept = default;
    };
    enum class ESessionState : std::uint8_t
    {
        OPENING,
        READY,
        CLOSING,
        CLOSED,
        FAILED
    };
    enum class ECloseProgress : std::uint8_t
    {
        PENDING,
        COMPLETE
    };
    struct ChangeStamp final
    {
        SessionId session;
        std::uint64_t content_revision{}, preview_revision{};
        friend constexpr bool operator==(const ChangeStamp &, const ChangeStamp &) noexcept = default;
    };
    struct OperationContext final
    {
        std::uint64_t request{}, subject{};
        std::array<char, 160> message{}; // Bounded diagnostic, never a source of business truth.
        bool truncated{};
    };
    // IDs are issued by the application's private composition owner, not global services.
    // Reopening the same asset issues a new nonzero SessionId; exhaustion is an explicit error.
} // namespace lux::editor::sessions
