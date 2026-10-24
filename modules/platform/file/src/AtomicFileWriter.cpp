#include <lux/engine/platform/AtomicFileWriter.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <utility>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace lux::platform
{
    namespace
    {
        FileFailure ioFailure(int value) noexcept
        {
            return {EFileIoError::IO, {value, std::system_category()}};
        }
    } // namespace

    FileResult<AtomicFileWriter> AtomicFileWriter::create(
        const std::filesystem::path& destination,
        EFileWriteMode mode
    ) noexcept
    {
        if (!destination.is_absolute() || destination.filename().empty())
        {
            return cxx::unexpected(FileFailure{EFileIoError::INVALID_PATH});
        }
        static std::atomic<std::uint64_t> sequence{};
        AtomicFileWriter writer;
        writer.destination_ = destination;
        writer.mode_ = mode;
#if defined(_WIN32)
        const auto process = GetCurrentProcessId();
#else
        const auto process = getpid();
#endif
        for (;;)
        {
            writer.temporary_ = destination;
            writer.temporary_ += ".tmp." + std::to_string(process) + "." +
                                 std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
#if defined(_WIN32)
            const auto handle = CreateFileW(
                writer.temporary_.c_str(),
                GENERIC_WRITE,
                0,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );
            if (handle != INVALID_HANDLE_VALUE)
            {
                writer.handle_ = reinterpret_cast<std::intptr_t>(handle);
                break;
            }
            const auto error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
#else
            const auto handle = ::open(writer.temporary_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (handle >= 0)
            {
                writer.handle_ = handle;
                break;
            }
            const auto error = errno;
            if (error != EEXIST)
#endif
            {
                return cxx::unexpected(ioFailure(error));
            }
        }
        writer.owns_temporary_ = true;
        return writer;
    }

    AtomicFileWriter::AtomicFileWriter(AtomicFileWriter&& other) noexcept
        : destination_(std::move(other.destination_)), temporary_(std::move(other.temporary_)),
          handle_(std::exchange(other.handle_, -1)), mode_(other.mode_),
          owns_temporary_(std::exchange(other.owns_temporary_, false))
    {
    }

    AtomicFileWriter& AtomicFileWriter::operator=(AtomicFileWriter&& other) noexcept
    {
        if (this != &other)
        {
            discard();
            destination_ = std::move(other.destination_);
            temporary_ = std::move(other.temporary_);
            handle_ = std::exchange(other.handle_, -1);
            mode_ = other.mode_;
            owns_temporary_ = std::exchange(other.owns_temporary_, false);
        }
        return *this;
    }

    AtomicFileWriter::~AtomicFileWriter() noexcept
    {
        discard();
    }

    void AtomicFileWriter::close() noexcept
    {
        if (handle_ == -1)
        {
            return;
        }
#if defined(_WIN32)
        CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
        ::close(static_cast<int>(handle_));
#endif
        handle_ = -1;
    }

    void AtomicFileWriter::discard() noexcept
    {
        close();
        if (owns_temporary_)
        {
            std::error_code ignored;
            std::filesystem::remove(temporary_, ignored);
            owns_temporary_ = false;
        }
    }

    FileResult<void> AtomicFileWriter::write(std::span<const std::byte> bytes, std::stop_token stop) noexcept
    {
        for (std::size_t offset = 0; offset < bytes.size();)
        {
            if (stop.stop_requested())
            {
                return cxx::unexpected(FileFailure{EFileIoError::CANCELLED});
            }
            const auto count = (std::min<std::size_t>)(bytes.size() - offset, 64 * 1024);
#if defined(_WIN32)
            DWORD written{};
            if (!WriteFile(reinterpret_cast<HANDLE>(handle_), bytes.data() + offset, DWORD(count), &written, nullptr))
            {
                return cxx::unexpected(ioFailure(GetLastError()));
            }
#else
            const auto written = ::write(static_cast<int>(handle_), bytes.data() + offset, count);
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
                return cxx::unexpected(FileFailure{EFileIoError::IO});
            }
            offset += written;
        }
        return {};
    }

    FileResult<void> AtomicFileWriter::seek(std::uint64_t offset) noexcept
    {
        if (offset > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
        {
            return cxx::unexpected(FileFailure{EFileIoError::LIMIT});
        }
#if defined(_WIN32)
        LARGE_INTEGER position;
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(reinterpret_cast<HANDLE>(handle_), position, nullptr, FILE_BEGIN))
        {
            return cxx::unexpected(ioFailure(GetLastError()));
        }
#else
        if (::lseek(static_cast<int>(handle_), static_cast<off_t>(offset), SEEK_SET) == -1)
        {
            return cxx::unexpected(ioFailure(errno));
        }
#endif
        return {};
    }

    FileResult<void> AtomicFileWriter::commit(std::stop_token stop) noexcept
    {
        if (stop.stop_requested())
        {
            return cxx::unexpected(FileFailure{EFileIoError::CANCELLED});
        }
#if defined(_WIN32)
        if (!FlushFileBuffers(reinterpret_cast<HANDLE>(handle_)))
        {
            return cxx::unexpected(ioFailure(GetLastError()));
        }
#else
        if (::fsync(static_cast<int>(handle_)) != 0)
        {
            return cxx::unexpected(ioFailure(errno));
        }
#endif
        close();
        if (stop.stop_requested())
        {
            return cxx::unexpected(FileFailure{EFileIoError::CANCELLED});
        }
#if defined(_WIN32)
        const DWORD flags = MOVEFILE_WRITE_THROUGH | (mode_ == EFileWriteMode::REPLACE ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!MoveFileExW(temporary_.c_str(), destination_.c_str(), flags))
        {
            const auto error = GetLastError();
            if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
            {
                return cxx::unexpected(FileFailure{EFileIoError::DESTINATION_EXISTS});
            }
            return cxx::unexpected(ioFailure(error));
        }
        owns_temporary_ = false;
#else
        const auto result = mode_ == EFileWriteMode::REPLACE ? ::rename(temporary_.c_str(), destination_.c_str())
                                                             : ::link(temporary_.c_str(), destination_.c_str());
        if (result != 0)
        {
            if (errno == EEXIST)
            {
                return cxx::unexpected(FileFailure{EFileIoError::DESTINATION_EXISTS});
            }
            return cxx::unexpected(ioFailure(errno));
        }
        // A failed unlink leaves the candidate owned so destruction can retry; the target is already published.
        owns_temporary_ = mode_ == EFileWriteMode::CREATE && ::unlink(temporary_.c_str()) != 0;
        const int directory = ::open(destination_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0)
        {
            return cxx::unexpected(FileFailure{EFileIoError::PUBLICATION_UNKNOWN, {errno, std::system_category()}});
        }
        const auto flushed = ::fsync(directory);
        const auto error = errno;
        ::close(directory);
        if (flushed != 0)
        {
            return cxx::unexpected(FileFailure{EFileIoError::PUBLICATION_UNKNOWN, {error, std::system_category()}});
        }
#endif
        return {};
    }
} // namespace lux::platform
