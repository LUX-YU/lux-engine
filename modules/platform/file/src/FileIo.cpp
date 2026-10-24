#include <lux/engine/platform/AtomicFileWriter.hpp>

#include <algorithm>
#include <cerrno>
#include <fstream>

namespace lux::platform
{
    namespace
    {
        FileFailure ioFailure(int value) noexcept
        {
            return {EFileIoError::IO, {value, std::system_category()}};
        }
    } // namespace

    FileResult<std::vector<std::byte>> readFileBounded(
        const std::filesystem::path& path,
        std::size_t limit,
        std::stop_token stop
    ) noexcept
    {
        if (stop.stop_requested())
        {
            return cxx::unexpected(FileFailure{EFileIoError::CANCELLED});
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
            return cxx::unexpected(FileFailure{EFileIoError::LIMIT});
        }
        std::vector<std::byte> result(static_cast<std::size_t>(size));
        stream.seekg(0);
        for (std::size_t offset = 0; offset < result.size();)
        {
            if (stop.stop_requested())
            {
                return cxx::unexpected(FileFailure{EFileIoError::CANCELLED});
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
            return cxx::unexpected(FileFailure{EFileIoError::IO});
        }
        return result;
    }

    FileResult<void> writeFileAtomic(
        const std::filesystem::path& path,
        std::span<const std::byte> bytes,
        EFileWriteMode mode,
        std::stop_token stop
    ) noexcept
    {
        if (stop.stop_requested())
        {
            return cxx::unexpected(FileFailure{EFileIoError::CANCELLED});
        }
        auto writer = AtomicFileWriter::create(path, mode);
        if (!writer)
        {
            return cxx::unexpected(writer.error());
        }
        auto written = writer->write(bytes, stop);
        if (!written)
        {
            return written;
        }
        return writer->commit(stop);
    }
} // namespace lux::platform
