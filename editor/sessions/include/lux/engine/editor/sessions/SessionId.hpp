#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <cstdint>
#include <limits>
#include <string>

namespace lux::editor::sessions
{
    enum class ESessionError : std::uint8_t
    {
        INVALID_ARGUMENT,
        WRONG_THREAD,
        WRONG_STORE,
        STALE_SESSION,
        WRONG_TYPE,
        CAPACITY,
        ID_EXHAUSTED,
        BUSY,
        STALE_CONTENT,
        STALE_BINDING,
        STALE_PUBLICATION,
        INVALID_CODE_LEASE,
        NOT_PREPARED,
        ALREADY_PUBLISHED
    };
    template <class T> using SessionResult = lux::cxx::expected<T, ESessionError>;

    struct SessionId final
    {
        std::uint64_t domain{};
        std::uint32_t slot{(std::numeric_limits<std::uint32_t>::max)()};
        std::uint64_t generation{};
        [[nodiscard]] bool valid() const noexcept
        {
            return domain != 0 && generation != 0;
        }
        friend bool operator==(SessionId, SessionId) noexcept = default;
    };
    struct SessionKindId final
    {
        std::string name;
        friend bool operator==(const SessionKindId&, const SessionKindId&) noexcept = default;
    };
    class SessionStore;
    template <class T> class TSessionKey final
    {
    public:
        [[nodiscard]] SessionId id() const noexcept
        {
            return id_;
        }
        friend bool operator==(TSessionKey, TSessionKey) noexcept = default;

    private:
        friend class SessionStore;
        explicit TSessionKey(SessionId id) noexcept : id_(id) {}
        SessionId id_;
    };
}
