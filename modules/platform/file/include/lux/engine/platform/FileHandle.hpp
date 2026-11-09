#pragma once

#include <lux/engine/platform/FileIo.hpp>

namespace lux::platform
{
    // Owns one open physical file. Rename/replacement does not redirect this handle to another file.
    // readAt may run concurrently; append requires one serial writer. All operations are blocking.
    class FileHandle final
    {
    public:
        FileHandle() noexcept = default;
        ~FileHandle() noexcept;
        FileHandle(FileHandle&& other) noexcept;
        FileHandle& operator=(FileHandle&& other) noexcept;
        FileHandle(const FileHandle&) = delete;
        FileHandle& operator=(const FileHandle&) = delete;

        [[nodiscard]] static FileResult<FileHandle> openRead(const std::filesystem::path& path) noexcept;
        [[nodiscard]] static FileResult<FileHandle> openAppend(const std::filesystem::path& path) noexcept;
        [[nodiscard]] static FileResult<FileHandle> createExclusive(const std::filesystem::path& path) noexcept;
        [[nodiscard]] FileResult<std::uint64_t> size() const noexcept;
        [[nodiscard]] FileResult<void> readAt(std::uint64_t offset, std::span<std::byte> output) const noexcept;
        // Returns the initial offset only after all bytes are written. Failure may leave a partial tail.
        [[nodiscard]] FileResult<std::uint64_t> append(std::span<const std::byte> bytes) noexcept;
        [[nodiscard]] FileResult<void> flush() noexcept;
        [[nodiscard]] FileResult<bool> sameFile(const FileHandle& other) const noexcept;

    private:
        explicit FileHandle(std::intptr_t handle) noexcept;
        std::intptr_t handle_{-1};
    };
} // namespace lux::platform
