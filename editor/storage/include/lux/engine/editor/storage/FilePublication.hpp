#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace lux::editor::storage
{
    enum class EFilePublicationError : std::uint8_t
    {
        READ,
        WRITE,
        FLUSH,
        REPLACE,
        INVALID_PATH
    };
    struct FilePublicationFailure final
    {
        EFilePublicationError code;
        std::filesystem::path path;
        std::uint64_t native_code{};
    };
    template <class T> using FileResult = lux::cxx::expected<T, FilePublicationFailure>;
    [[nodiscard]] FileResult<std::vector<std::byte>> readPublicationFile(const std::filesystem::path&, std::size_t);
    [[nodiscard]] FileResult<void> writePublicationFile(const std::filesystem::path&, std::span<const std::byte>);
    [[nodiscard]] FileResult<void> replacePublicationFile(const std::filesystem::path&, const std::filesystem::path&);
    [[nodiscard]] std::string publicationDigest(std::span<const std::byte>);
    [[nodiscard]] FileResult<std::string> publicationFileDigest(const std::filesystem::path&);
    // Canonical root-contained path. Hard-link aliases are explicitly unsupported.
    [[nodiscard]] FileResult<std::string> publicationTargetKey(
        const std::filesystem::path& root,
        const std::filesystem::path& address
    );
}
