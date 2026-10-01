#include <lux/engine/editor/storage/FilePublication.hpp>
#include <lux/cxx/algorithm/Sha256.hpp>
#include <fstream>
#include <array>
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
namespace lux::editor::storage
{
    namespace
    {
        auto failed(EFilePublicationError code, const std::filesystem::path& path, std::uint64_t native = 0)
        {
            return lux::cxx::unexpected(FilePublicationFailure{code, path, native});
        }
    }
    std::string hexDigest(const lux::cxx::algorithm::Sha256Digest& value)
    {
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(value.size() * 2);
        for (const auto item : value)
        {
            const auto byte = static_cast<unsigned char>(item);
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 15]);
        }
        return result;
    }

    std::string publicationDigest(std::span<const std::byte> bytes)
    {
        lux::cxx::algorithm::Sha256 hash;
        hash.update(bytes);
        return hexDigest(hash.digest());
    }

    FileResult<std::vector<std::byte>> readPublicationFile(const std::filesystem::path& path, std::size_t file_limit)
    {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > file_limit)
        {
            return failed(EFilePublicationError::READ, path, error.value());
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        std::ifstream file(path, std::ios::binary);
        if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        {
            return failed(EFilePublicationError::READ, path);
        }
        return bytes;
    }

    FileResult<void> writePublicationFile(const std::filesystem::path& path, std::span<const std::byte> bytes)
    {
#if defined(_WIN32)
        const auto file =
            CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return failed(EFilePublicationError::WRITE, path, GetLastError());
        }
        DWORD count{};
        const bool written = bytes.size() <= MAXDWORD &&
                             WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) &&
                             count == bytes.size();
        const auto write_error = GetLastError();
        const bool flushed = written && FlushFileBuffers(file);
        const auto flush_error = GetLastError();
        CloseHandle(file);
        if (!written)
        {
            return failed(EFilePublicationError::WRITE, path, write_error);
        }
        if (!flushed)
        {
            return failed(EFilePublicationError::FLUSH, path, flush_error);
        }
#else
        const auto file = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (file < 0)
        {
            return failed(EFilePublicationError::WRITE, path, errno);
        }
        std::size_t offset{};
        while (offset < bytes.size())
        {
            const auto count = ::write(file, bytes.data() + offset, bytes.size() - offset);
            if (count <= 0)
            {
                const auto error = errno;
                ::close(file);
                return failed(EFilePublicationError::WRITE, path, error);
            }
            offset += static_cast<std::size_t>(count);
        }
        const auto flushed = ::fsync(file);
        const auto error = errno;
        ::close(file);
        if (flushed != 0)
        {
            return failed(EFilePublicationError::FLUSH, path, error);
        }
#endif
        return {};
    }

    FileResult<void> replacePublicationFile(const std::filesystem::path& staged, const std::filesystem::path& target)
    {
#if defined(_WIN32)
        if (!MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            return failed(EFilePublicationError::REPLACE, target, GetLastError());
        }
#else
        std::error_code error;
        std::filesystem::rename(staged, target, error);
        if (error)
        {
            return failed(EFilePublicationError::REPLACE, target, error.value());
        }
#endif
        return {};
    }

    FileResult<std::string> publicationFileDigest(const std::filesystem::path& path)
    {
        constexpr std::size_t file_limit = 512U * 1024U * 1024U;
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error)
        {
            return failed(EFilePublicationError::READ, path, error.value());
        }
        if (!exists)
        {
            return std::string{"missing"};
        }
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > file_limit)
        {
            return failed(EFilePublicationError::READ, path, error.value());
        }
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return failed(EFilePublicationError::READ, path);
        }
        std::array<std::byte, 64U * 1024U> buffer;
        lux::cxx::algorithm::Sha256 hash;
        std::uint64_t total{};
        while (file)
        {
            file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
            const auto count = static_cast<std::size_t>(file.gcount());
            total += count;
            if (total > file_limit)
            {
                return failed(EFilePublicationError::READ, path);
            }
            hash.update(std::span<const std::byte>(buffer).first(count));
        }
        if (file.bad() || total != size)
        {
            return failed(EFilePublicationError::READ, path);
        }
        return hexDigest(hash.digest());
    }

    FileResult<std::string> publicationTargetKey(
        const std::filesystem::path& root,
        const std::filesystem::path& address
    )
    {
        std::error_code error;
        const auto base = std::filesystem::canonical(root, error);
        if (error)
            return failed(EFilePublicationError::INVALID_PATH, root, error.value());
        auto path = std::filesystem::weakly_canonical(address.is_absolute() ? address : base / address, error);
        if (error)
            return failed(EFilePublicationError::INVALID_PATH, address, error.value());
        const auto relative = path.lexically_relative(base);
        const bool outside = relative.empty() || *relative.begin() == ".." || relative == ".";
        if (outside)
            return failed(EFilePublicationError::INVALID_PATH, address);
        const bool exists = std::filesystem::exists(path, error);
        if (error)
            return failed(EFilePublicationError::INVALID_PATH, path, error.value());
        if (exists)
        {
            const auto links = std::filesystem::hard_link_count(path, error);
            const bool unsupported = error || links != 1 || !std::filesystem::is_regular_file(path, error);
            if (unsupported)
                return failed(EFilePublicationError::INVALID_PATH, path, error.value());
        }
#if defined(_WIN32)
        const auto native = path.native();
        std::wstring lower(native.size(), L'\0');
        if (LCMapStringEx(
                LOCALE_NAME_INVARIANT,
                LCMAP_LOWERCASE,
                native.data(),
                static_cast<int>(native.size()),
                lower.data(),
                static_cast<int>(lower.size()),
                nullptr,
                nullptr,
                0
            ) == 0)
            return failed(EFilePublicationError::INVALID_PATH, path, GetLastError());
        path = std::move(lower);
#endif
        auto utf8 = path.generic_u8string();
        return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    }
}
