#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <lux/cxx/compile_time/expected.hpp>
#include <span>
#include <stop_token>
#include <system_error>
#include <vector>

namespace lux::platform
{
    enum class EFileIoError : std::uint8_t
    {
        INVALID_PATH,
        LIMIT,
        CANCELLED,
        IO,
        DESTINATION_EXISTS,
        PUBLICATION_UNKNOWN
    };

    struct FileFailure final
    {
        EFileIoError code{};
        std::error_code system;
    };

    template <class T> using FileResult = cxx::expected<T, FileFailure>;

    enum class EFileWriteMode : std::uint8_t
    {
        CREATE,
        REPLACE
    };

    /// Synchronously read at most limit bytes. A changing file may fail; no snapshot is promised.
    [[nodiscard]] FileResult<std::vector<std::byte>> readFileBounded(
        const std::filesystem::path& path,
        std::size_t limit,
        std::stop_token stop = {}
    ) noexcept;

    /// Publish a complete file without deleting the previous destination on failure.
    /// The absolute path's parent must exist. Stop is observed only before publication.
    [[nodiscard]] FileResult<void> writeFileAtomic(
        const std::filesystem::path& path,
        std::span<const std::byte> bytes,
        EFileWriteMode mode,
        std::stop_token stop = {}
    ) noexcept;
} // namespace lux::platform
