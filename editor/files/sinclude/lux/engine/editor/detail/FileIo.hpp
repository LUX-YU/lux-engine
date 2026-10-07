#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <lux/cxx/compile_time/expected.hpp>
#include <span>
#include <stop_token>
#include <system_error>
#include <vector>

namespace lux::editor::detail
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
    struct FileIoFailure final
    {
        EFileIoError code{};
        std::error_code system;
    };
    template <class T> using FileIoResult = cxx::expected<T, FileIoFailure>;
    enum class EFileWriteMode : std::uint8_t
    {
        CREATE,
        REPLACE
    };
    [[nodiscard]] FileIoResult<std::vector<std::byte>> readFileBounded(
        const std::filesystem::path&,
        std::size_t limit,
        std::stop_token
    ) noexcept;
    [[nodiscard]] FileIoResult<void> writeFileAtomic(
        const std::filesystem::path&,
        std::span<const std::byte>,
        EFileWriteMode,
        std::stop_token
    ) noexcept;
} // namespace lux::editor::detail
