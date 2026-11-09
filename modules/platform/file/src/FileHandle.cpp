#include <lux/engine/platform/FileHandle.hpp>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <utility>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lux::platform
{
    namespace
    {
        void closeNative(std::intptr_t handle) noexcept
        {
            if (handle == -1)
            {
                return;
            }
#if defined(_WIN32)
            CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
            ::close(static_cast<int>(handle));
#endif
        }

        FileResult<void>
        readPositioned(std::intptr_t handle, std::uint64_t offset, std::span<std::byte> output) noexcept
        {
            if (output.empty())
            {
                return {};
            }
#if defined(_WIN32)
            // Each concurrent operation waits on its own event, never on the shared file handle.
            const auto event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!event)
            {
                return cxx::unexpected(FileFailure{EFileIoError::IO, {int(GetLastError()), std::system_category()}});
            }
            DWORD failure{};
            while (!output.empty())
            {
                OVERLAPPED operation{};
                operation.Offset = static_cast<DWORD>(offset);
                operation.OffsetHigh = static_cast<DWORD>(offset >> 32U);
                operation.hEvent = event;
                const auto count = static_cast<DWORD>(std::min<std::size_t>(output.size(), 1U << 30U));
                DWORD read{};
                const auto ready = ReadFile(reinterpret_cast<HANDLE>(handle), output.data(), count, &read, &operation);
                if (!ready)
                {
                    const auto error = GetLastError();
                    if (error != ERROR_IO_PENDING)
                    {
                        failure = error;
                        break;
                    }
                    if (!GetOverlappedResult(reinterpret_cast<HANDLE>(handle), &operation, &read, TRUE))
                    {
                        failure = GetLastError();
                        break;
                    }
                }
                if (read == 0)
                {
                    failure = ERROR_HANDLE_EOF;
                    break;
                }
                output = output.subspan(read);
                offset += read;
            }
            CloseHandle(event);
            if (failure)
            {
                return cxx::unexpected(FileFailure{EFileIoError::IO, {int(failure), std::system_category()}});
            }
#else
            while (!output.empty())
            {
                const auto count = std::min<std::size_t>(output.size(), 1U << 30U);
                const auto read = ::pread(static_cast<int>(handle), output.data(), count, static_cast<off_t>(offset));
                if (read < 0 && errno == EINTR)
                {
                    continue;
                }
                if (read <= 0)
                {
                    return cxx::unexpected(FileFailure{EFileIoError::IO, {read == 0 ? EIO : errno, std::system_category()}});
                }
                output = output.subspan(static_cast<std::size_t>(read));
                offset += static_cast<std::uint64_t>(read);
            }
#endif
            return {};
        }

        FileResult<void> appendAll(std::intptr_t handle, std::span<const std::byte> bytes) noexcept
        {
#if defined(_WIN32)
            LARGE_INTEGER position{};
            if (!SetFilePointerEx(reinterpret_cast<HANDLE>(handle), position, nullptr, FILE_END))
            {
                return cxx::unexpected(FileFailure{EFileIoError::IO, {int(GetLastError()), std::system_category()}});
            }
#endif
            while (!bytes.empty())
            {
                const auto count = std::min<std::size_t>(bytes.size(), 1U << 30U);
#if defined(_WIN32)
                DWORD written{};
                if (!WriteFile(reinterpret_cast<HANDLE>(handle), bytes.data(), static_cast<DWORD>(count), &written, nullptr))
                {
                    return cxx::unexpected(FileFailure{EFileIoError::IO, {int(GetLastError()), std::system_category()}});
                }
                if (written == 0)
                {
                    return cxx::unexpected(FileFailure{EFileIoError::IO, {ERROR_WRITE_FAULT, std::system_category()}});
                }
#else
                const auto written = ::write(static_cast<int>(handle), bytes.data(), count);
                if (written < 0 && errno == EINTR)
                {
                    continue;
                }
                if (written <= 0)
                {
                    return cxx::unexpected(FileFailure{EFileIoError::IO, {written == 0 ? EIO : errno, std::system_category()}});
                }
#endif
                bytes = bytes.subspan(static_cast<std::size_t>(written));
            }
            return {};
        }
    } // namespace

    FileHandle::FileHandle(std::intptr_t handle) noexcept : handle_(handle) {}

    FileHandle::~FileHandle() noexcept
    {
        closeNative(handle_);
    }

    FileHandle::FileHandle(FileHandle&& other) noexcept : handle_(std::exchange(other.handle_, -1)) {}

    FileHandle& FileHandle::operator=(FileHandle&& other) noexcept
    {
        if (this != &other)
        {
            closeNative(handle_);
            handle_ = std::exchange(other.handle_, -1);
        }
        return *this;
    }

    FileResult<FileHandle> FileHandle::openRead(const std::filesystem::path& path) noexcept
    {
#if defined(_WIN32)
        const auto handle = CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr
        );
        if (handle == INVALID_HANDLE_VALUE)
        {
            return cxx::unexpected(FileFailure{EFileIoError::IO, {int(GetLastError()), std::system_category()}});
        }
        return FileHandle(reinterpret_cast<std::intptr_t>(handle));
#else
        const auto handle = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (handle < 0)
        {
            return cxx::unexpected(FileFailure{EFileIoError::IO, {errno, std::system_category()}});
        }
        return FileHandle(handle);
#endif
    }

    FileResult<FileHandle> FileHandle::openAppend(const std::filesystem::path& path) noexcept
    {
#if defined(_WIN32)
        const auto handle = CreateFileW(
            path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr
        );
        if (handle == INVALID_HANDLE_VALUE)
        {
            return cxx::unexpected(FileFailure{EFileIoError::IO, {int(GetLastError()), std::system_category()}});
        }
        return FileHandle(reinterpret_cast<std::intptr_t>(handle));
#else
        const auto handle = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
        if (handle < 0)
        {
            return cxx::unexpected(FileFailure{EFileIoError::IO, {errno, std::system_category()}});
        }
        return FileHandle(handle);
#endif
    }

    FileResult<FileHandle> FileHandle::createExclusive(const std::filesystem::path& path) noexcept
    {
#if defined(_WIN32)
        const auto handle = CreateFileW(
            path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr
        );
        if (handle == INVALID_HANDLE_VALUE)
        {
            const auto error = GetLastError();
            const bool exists = error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS;
            return cxx::unexpected(FileFailure{
                exists ? EFileIoError::DESTINATION_EXISTS : EFileIoError::IO, {int(error), std::system_category()}
            });
        }
        return FileHandle(reinterpret_cast<std::intptr_t>(handle));
#else
        const auto handle = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (handle < 0)
        {
            const auto error = errno;
            return cxx::unexpected(FileFailure{
                error == EEXIST ? EFileIoError::DESTINATION_EXISTS : EFileIoError::IO, {error, std::system_category()}
            });
        }
        return FileHandle(handle);
#endif
    }

    FileResult<std::uint64_t> FileHandle::size() const noexcept
    {
#if defined(_WIN32)
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(reinterpret_cast<HANDLE>(handle_), &size))
        {
            return cxx::unexpected(FileFailure{EFileIoError::IO, {int(GetLastError()), std::system_category()}});
        }
        return static_cast<std::uint64_t>(size.QuadPart);
#else
        struct stat info{};
        if (::fstat(static_cast<int>(handle_), &info) != 0)
        {
            return cxx::unexpected(FileFailure{EFileIoError::IO, {errno, std::system_category()}});
        }
        return static_cast<std::uint64_t>(info.st_size);
#endif
    }

    FileResult<void> FileHandle::readAt(std::uint64_t offset, std::span<std::byte> output) const noexcept
    {
        constexpr auto limit = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
        const bool outside = offset > limit || output.size() > limit - offset;
        if (outside)
        {
            return cxx::unexpected(FileFailure{EFileIoError::LIMIT});
        }
        return readPositioned(handle_, offset, output);
    }

    FileResult<std::uint64_t> FileHandle::append(std::span<const std::byte> bytes) noexcept
    {
        auto start = size();
        if (!start)
        {
            return cxx::unexpected(start.error());
        }
        constexpr auto limit = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
        const bool outside = *start > limit || bytes.size() > limit - *start;
        if (outside)
        {
            return cxx::unexpected(FileFailure{EFileIoError::LIMIT});
        }
        auto written = appendAll(handle_, bytes);
        if (!written)
        {
            return cxx::unexpected(written.error());
        }
        return *start;
    }

    FileResult<void> FileHandle::flush() noexcept
    {
#if defined(_WIN32)
        if (!FlushFileBuffers(reinterpret_cast<HANDLE>(handle_)))
        {
            return cxx::unexpected(FileFailure{EFileIoError::IO, {int(GetLastError()), std::system_category()}});
        }
#else
        while (::fsync(static_cast<int>(handle_)) != 0)
        {
            if (errno != EINTR)
            {
                return cxx::unexpected(FileFailure{EFileIoError::IO, {errno, std::system_category()}});
            }
        }
#endif
        return {};
    }

    FileResult<bool> FileHandle::sameFile(const FileHandle& other) const noexcept
    {
#if defined(_WIN32)
        FILE_ID_INFO first{}, second{};
        const auto first_ok = GetFileInformationByHandleEx(
            reinterpret_cast<HANDLE>(handle_), FileIdInfo, &first, sizeof(first)
        );
        const auto second_ok = first_ok && GetFileInformationByHandleEx(
            reinterpret_cast<HANDLE>(other.handle_), FileIdInfo, &second, sizeof(second)
        );
        if (!second_ok)
        {
            return cxx::unexpected(FileFailure{EFileIoError::IO, {int(GetLastError()), std::system_category()}});
        }
        return first.VolumeSerialNumber == second.VolumeSerialNumber &&
            std::equal(std::begin(first.FileId.Identifier), std::end(first.FileId.Identifier), second.FileId.Identifier);
#else
        struct stat first{}, second{};
        if (::fstat(static_cast<int>(handle_), &first) != 0 || ::fstat(static_cast<int>(other.handle_), &second) != 0)
        {
            return cxx::unexpected(FileFailure{EFileIoError::IO, {errno, std::system_category()}});
        }
        return first.st_dev == second.st_dev && first.st_ino == second.st_ino;
#endif
    }
} // namespace lux::platform
