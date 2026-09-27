#include <lux/engine/platform/Process.hpp>
#include <memory>
#include <cstdlib>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include <shlobj.h>
#else
#include <cerrno>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#endif

namespace lux::engine::platform
{
    namespace
    {
        auto failed(EProcessError code, std::uint64_t native = 0) noexcept
        {
            return lux::cxx::unexpected(ProcessFailure{code, native});
        }
#if defined(_WIN32)
        void appendQuoted(std::wstring& command, std::wstring_view argument)
        {
            command += L'"';
            std::size_t slashes{};
            for (const auto character : argument)
            {
                if (character == L'\\')
                {
                    ++slashes;
                    continue;
                }
                command.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
                command += character;
                slashes = 0;
            }
            command.append(slashes * 2, L'\\');
            command += L'"';
        }
#endif
    }

    lux::cxx::expected<std::filesystem::path, ProcessFailure> userConfigDirectory() noexcept
    {
#if defined(_WIN32)
        PWSTR path{};
        const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path);
        if (FAILED(result))
            return failed(EProcessError::CONFIG_DIRECTORY, result);
        std::filesystem::path directory{path};
        CoTaskMemFree(path);
        return directory;
#else
        if (const auto* path = std::getenv("XDG_CONFIG_HOME"); path && *path)
            return std::filesystem::path{path};
        if (const auto* home = std::getenv("HOME"); home && *home)
            return std::filesystem::path{home} / ".config";
        return failed(EProcessError::UNSUPPORTED);
#endif
    }

    lux::cxx::expected<std::filesystem::path, ProcessFailure> executablePath() noexcept
    {
#if defined(_WIN32)
        std::wstring path(32768, L'\0');
        const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!size || size == path.size())
            return failed(EProcessError::EXECUTABLE_PATH, GetLastError());
        path.resize(size);
        return std::filesystem::path(std::move(path));
#elif defined(__linux__)
        std::error_code error;
        auto path = std::filesystem::canonical("/proc/self/exe", error);
        if (error)
            return failed(EProcessError::EXECUTABLE_PATH, error.value());
        return path;
#else
        return failed(EProcessError::UNSUPPORTED);
#endif
    }

    lux::cxx::expected<std::vector<std::string>, ProcessFailure> processArguments(int argc, char** argv) noexcept
    {
        std::vector<std::string> result;
#if defined(_WIN32)
        int count{};
        auto* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!arguments)
            return failed(EProcessError::ARGUMENT_ENCODING, GetLastError());
        const auto release = [](wchar_t** value) { LocalFree(value); };
        const std::unique_ptr<wchar_t*, decltype(release)> owner(arguments, release);
        result.reserve(count);
        for (int index{}; index < count; ++index)
        {
            const std::wstring_view argument(arguments[index]);
            if (argument.empty())
            {
                result.emplace_back();
                continue;
            }
            const auto bytes = WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                argument.data(),
                int(argument.size()),
                nullptr,
                0,
                nullptr,
                nullptr
            );
            if (!bytes)
                return failed(EProcessError::ARGUMENT_ENCODING, GetLastError());
            std::string value(bytes, '\0');
            if (!WideCharToMultiByte(
                    CP_UTF8,
                    WC_ERR_INVALID_CHARS,
                    argument.data(),
                    int(argument.size()),
                    value.data(),
                    bytes,
                    nullptr,
                    nullptr
                ))
                return failed(EProcessError::ARGUMENT_ENCODING, GetLastError());
            result.push_back(std::move(value));
        }
#else
        if (argc < 0 || (argc && !argv))
            return failed(EProcessError::INVALID_ARGUMENT);
        result.assign(argv, argv + argc);
#endif
        return result;
    }

    lux::cxx::expected<void, ProcessFailure> launchProcess(
        const std::filesystem::path& executable,
        std::span<const std::string> arguments
    ) noexcept
    {
        const bool invalid_path =
            executable.empty() || !executable.is_absolute() ||
            executable.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos;
        if (invalid_path)
            return failed(EProcessError::INVALID_ARGUMENT);
        for (const auto& value : arguments)
            if (value.find('\0') != std::string::npos)
                return failed(EProcessError::INVALID_ARGUMENT);
#if defined(_WIN32)
        std::wstring command;
        appendQuoted(command, executable.native());
        for (const auto& argument : arguments)
        {
            command += L' ';
            if (argument.empty())
            {
                appendQuoted(command, {});
                continue;
            }
            const auto size = MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                argument.data(),
                static_cast<int>(argument.size()),
                nullptr,
                0
            );
            if (!size)
                return failed(EProcessError::ARGUMENT_ENCODING, GetLastError());
            std::wstring value(size, L'\0');
            if (!MultiByteToWideChar(
                    CP_UTF8,
                    MB_ERR_INVALID_CHARS,
                    argument.data(),
                    static_cast<int>(argument.size()),
                    value.data(),
                    size
                ))
                return failed(EProcessError::ARGUMENT_ENCODING, GetLastError());
            appendQuoted(command, value);
        }
        if (command.size() >= 32767)
            return failed(EProcessError::INVALID_ARGUMENT);
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                executable.c_str(),
                command.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startup,
                &process
            ))
            return failed(EProcessError::START, GetLastError());
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return {};
#else
        // Double fork detaches the application without leaving a zombie in the calling host.
        std::vector<char*> values;
        auto path = executable.string();
        values.push_back(path.data());
        for (const auto& argument : arguments)
            values.push_back(const_cast<char*>(argument.c_str()));
        values.push_back(nullptr);
        int channel[2];
        if (pipe(channel) != 0)
            return failed(EProcessError::START, errno);
        if (fcntl(channel[1], F_SETFD, FD_CLOEXEC) == -1)
        {
            const auto error = errno;
            close(channel[0]);
            close(channel[1]);
            return failed(EProcessError::START, error);
        }
        const auto child = fork();
        if (child == 0)
        {
            close(channel[0]);
            const auto detached = fork();
            if (detached > 0)
                _exit(0);
            if (detached == 0)
                execv(path.c_str(), values.data());
            const auto error = errno;
            static_cast<void>(write(channel[1], &error, sizeof(error)));
            _exit(127);
        }
        const auto spawn_error = errno;
        close(channel[1]);
        if (child < 0)
        {
            close(channel[0]);
            return failed(EProcessError::START, spawn_error);
        }
        int status{};
        while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        {
        }
        int error{};
        ssize_t bytes;
        do
        {
            bytes = read(channel[0], &error, sizeof(error));
        } while (bytes < 0 && errno == EINTR);
        const auto read_error = errno;
        close(channel[0]);
        if (bytes != 0)
            return failed(EProcessError::START, bytes < 0 ? read_error : error);
        return {};
#endif
    }
}
