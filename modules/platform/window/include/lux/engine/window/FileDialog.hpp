#pragma once

#include <lux/engine/window/visibility.h>
#include <lux/cxx/compile_time/expected.hpp>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace lux::window
{
    class LuxWindow;
    struct FileDialogFilter final
    {
        const char* name;
        const char* extensions; // Comma-separated extensions, without dots or wildcards.
    };
    enum class EFileDialogError : std::uint8_t
    {
        INITIALIZATION,
        INVALID_PARENT,
        PLATFORM,
        CAPACITY
    };
    struct FileDialogFailure final
    {
        EFileDialogError code;
        std::string detail;
    };
    using FileDialogResult = lux::cxx::expected<std::optional<std::filesystem::path>, FileDialogFailure>;

    // Owner thread only. This asks for a native path; it performs no asset reading or publication.
    // A disengaged optional is cancellation, not failure. Null parent supports project selection at startup.
    [[nodiscard]] LUX_PLATFORM_WINDOW_PUBLIC FileDialogResult openFileDialog(
        LuxWindow* parent,
        std::span<const FileDialogFilter> filters = {},
        const std::filesystem::path& directory = {}
    ) noexcept;
}
