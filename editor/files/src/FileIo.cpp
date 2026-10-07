#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <lux/engine/editor/detail/FileIo.hpp>
#if defined(_WIN32)
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace lux::editor::detail
{
    namespace
    {
        FileIoFailure ioFailure(int value) noexcept
        {
            return {EFileIoError::IO, {value, std::system_category()}};
        }
        struct TemporaryFile final
        {
            std::filesystem::path path;
#if defined(_WIN32)
            HANDLE handle{INVALID_HANDLE_VALUE};
#else
            int handle{-1};
#endif
            bool owns_path{};
            ~TemporaryFile() noexcept
            {
                close();
                if (owns_path)
                {
                    std::error_code ignored;
                    std::filesystem::remove(path, ignored);
                }
            }
            void close() noexcept
            {
#if defined(_WIN32)
                if (handle != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(handle);
                    handle = INVALID_HANDLE_VALUE;
                }
#else
                if (handle != -1)
                {
                    ::close(handle);
                    handle = -1;
                }
#endif
            }
            TemporaryFile() = default;
            TemporaryFile(const TemporaryFile&) = delete;
            TemporaryFile& operator=(const TemporaryFile&) = delete;
        };
    } // namespace
    FileIoResult<std::vector<std::byte>> readFileBounded(
        const std::filesystem::path& path,
        std::size_t limit,
        std::stop_token stop
    ) noexcept
    {
        if (stop.stop_requested())
        {
            return cxx::unexpected(FileIoFailure{EFileIoError::CANCELLED});
        }
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
        {
            return cxx::unexpected(ioFailure(errno));
        }
        const auto size = stream.tellg();
        if (size < 0)
        {
            return cxx::unexpected(ioFailure(errno));
        }
        if (static_cast<std::uint64_t>(size) > limit)
        {
            return cxx::unexpected(FileIoFailure{EFileIoError::LIMIT});
        }
        std::vector<std::byte> result(static_cast<std::size_t>(size));
        stream.seekg(0);
        for (std::size_t offset = 0; offset < result.size();)
        {
            if (stop.stop_requested())
            {
                return cxx::unexpected(FileIoFailure{EFileIoError::CANCELLED});
            }
            const auto count = std::min<std::size_t>(result.size() - offset, 64 * 1024);
            if (!stream.read(reinterpret_cast<char*>(result.data() + offset), count))
            {
                return cxx::unexpected(ioFailure(errno));
            }
            offset += count;
        }
        // A concurrently appended file is not silently accepted as its old prefix.
        if (stream.peek() != std::ifstream::traits_type::eof())
        {
            return cxx::unexpected(FileIoFailure{EFileIoError::IO});
        }
        return result;
    }
    FileIoResult<void> writeFileAtomic(
        const std::filesystem::path& path,
        std::span<const std::byte> bytes,
        EFileWriteMode mode,
        std::stop_token stop
    ) noexcept
    {
        if (stop.stop_requested())
        {
            return cxx::unexpected(FileIoFailure{EFileIoError::CANCELLED});
        }
        if (!path.is_absolute() || path.filename().empty())
        {
            return cxx::unexpected(FileIoFailure{EFileIoError::INVALID_PATH});
        }
        static std::atomic<std::uint64_t> sequence{};
        TemporaryFile temporary;
#if defined(_WIN32)
        const auto process = GetCurrentProcessId();
#else
        const auto process = getpid();
#endif
        temporary.path = path;
        temporary.path += ".tmp." + std::to_string(process) + "." + std::to_string(++sequence);
#if defined(_WIN32)
        temporary.handle =
            CreateFileW(temporary.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (temporary.handle == INVALID_HANDLE_VALUE)
        {
            return cxx::unexpected(ioFailure(GetLastError()));
        }
#else
        temporary.handle = ::open(temporary.path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (temporary.handle < 0)
        {
            return cxx::unexpected(ioFailure(errno));
        }
#endif
        temporary.owns_path = true;
        for (std::size_t offset = 0; offset < bytes.size();)
        {
            if (stop.stop_requested())
            {
                return cxx::unexpected(FileIoFailure{EFileIoError::CANCELLED});
            }
            const auto count = std::min<std::size_t>(bytes.size() - offset, 64 * 1024);
#if defined(_WIN32)
            DWORD written{};
            if (!WriteFile(temporary.handle, bytes.data() + offset, static_cast<DWORD>(count), &written, nullptr))
            {
                return cxx::unexpected(ioFailure(GetLastError()));
            }
#else
            const auto written = ::write(temporary.handle, bytes.data() + offset, count);
            if (written < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                return cxx::unexpected(ioFailure(errno));
            }
#endif
            if (written == 0)
            {
                return cxx::unexpected(FileIoFailure{EFileIoError::IO});
            }
            offset += written;
        }
#if defined(_WIN32)
        if (!FlushFileBuffers(temporary.handle))
        {
            return cxx::unexpected(ioFailure(GetLastError()));
        }
#else
        if (::fsync(temporary.handle) != 0)
        {
            return cxx::unexpected(ioFailure(errno));
        }
#endif
        temporary.close();
        if (stop.stop_requested())
        {
            return cxx::unexpected(FileIoFailure{EFileIoError::CANCELLED});
        }
#if defined(_WIN32)
        const DWORD flags = MOVEFILE_WRITE_THROUGH | (mode == EFileWriteMode::REPLACE ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!MoveFileExW(temporary.path.c_str(), path.c_str(), flags))
        {
            const auto error = GetLastError();
            if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
            {
                return cxx::unexpected(FileIoFailure{EFileIoError::DESTINATION_EXISTS});
            }
            return cxx::unexpected(ioFailure(error));
        }
        temporary.owns_path = false;
#else
        const auto published = mode == EFileWriteMode::REPLACE ? ::rename(temporary.path.c_str(), path.c_str())
                                                               : ::link(temporary.path.c_str(), path.c_str());
        if (published != 0)
        {
            if (errno == EEXIST)
            {
                return cxx::unexpected(FileIoFailure{EFileIoError::DESTINATION_EXISTS});
            }
            return cxx::unexpected(ioFailure(errno));
        }
        if (mode == EFileWriteMode::CREATE)
        {
            ::unlink(temporary.path.c_str());
        }
        temporary.owns_path = false;
        const int directory = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0)
        {
            return cxx::unexpected(FileIoFailure{EFileIoError::PUBLICATION_UNKNOWN, {errno, std::system_category()}});
        }
        const auto flushed = ::fsync(directory);
        const auto error = errno;
        ::close(directory);
        if (flushed != 0)
        {
            return cxx::unexpected(FileIoFailure{EFileIoError::PUBLICATION_UNKNOWN, {error, std::system_category()}});
        }
#endif
        return {};
    }
} // namespace lux::editor::detail
