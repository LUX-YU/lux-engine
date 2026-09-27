#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace lux::engine::platform
{
    enum class EProcessError : std::uint8_t
    {
        INVALID_ARGUMENT,
        EXECUTABLE_PATH,
        CONFIG_DIRECTORY,
        ARGUMENT_ENCODING,
        START,
        CAPACITY,
        UNSUPPORTED
    };
    struct ProcessFailure final
    {
        EProcessError code;
        std::uint64_t native_code{};
    };
    [[nodiscard]] lux::cxx::expected<std::filesystem::path, ProcessFailure> executablePath() noexcept;
    [[nodiscard]] lux::cxx::expected<std::filesystem::path, ProcessFailure> userConfigDirectory() noexcept;
    [[nodiscard]] lux::cxx::expected<std::vector<std::string>, ProcessFailure> processArguments(int, char**) noexcept;
    // Arguments are UTF-8 values, not a shell command. Success means the OS accepted creation.
    [[nodiscard]] lux::cxx::expected<void, ProcessFailure> launchProcess(
        const std::filesystem::path&,
        std::span<const std::string> arguments
    ) noexcept;
}
