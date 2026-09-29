#pragma once

#include <lux/engine/editor/sessions/ContentStamp.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <string>
#include <vector>

namespace lux::editor::persistence
{
    enum class EPersistenceError : std::uint8_t
    {
        INVALID_ARGUMENT,
        WRONG_THREAD,
        CAPACITY,
        UNKNOWN_ID,
        BUSY,
        UNBOUND,
        STALE_SOURCE,
        CANCELLED,
        ENCODE,
        DECODE,
        CONFLICT,
        IO,
        UNSUPPORTED_TARGET,
        REBIND_UNSUPPORTED,
        EXECUTION,
        NOT_TERMINAL,
        WRITER_ACTIVE
    };
    struct PersistenceFailure final
    {
        EPersistenceError code{EPersistenceError::INVALID_ARGUMENT};
        std::string detail;
        std::uint64_t native_code{};
    };
    template <class T> using PersistenceResult = lux::cxx::expected<T, PersistenceFailure>;
    struct WriteTargetKey final
    {
        std::string value;
        friend bool operator==(const WriteTargetKey&, const WriteTargetKey&) noexcept = default;
    };
    struct WriteTarget final
    {
        WriteTargetKey key;
        std::string expected_version;
        friend bool operator==(const WriteTarget&, const WriteTarget&) noexcept = default;
    };
    struct WriteTicket final
    {
        std::uint64_t value{};
        friend bool operator==(WriteTicket, WriteTicket) noexcept = default;
    };
    struct WriteOrigin final
    {
        sessions::SessionId session;
        sessions::BindingRevision binding;
        friend bool operator==(WriteOrigin, WriteOrigin) noexcept = default;
    };
    struct EncodedArtifact final
    {
        std::vector<std::byte> bytes;
    };
}
