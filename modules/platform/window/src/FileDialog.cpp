#include <exception>
#include <lux/engine/window/FileDialog.hpp>
#include <lux/engine/window/LuxWindow.hpp>

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#else
#define GLFW_EXPOSE_NATIVE_X11
#endif
#include <nfd_glfw3.h>
#include <vector>

namespace lux::window
{
    FileDialogResult openFileDialog(
        LuxWindow* parent,
        std::span<const FileDialogFilter> filters,
        const std::filesystem::path& directory
    ) noexcept
    try
    {
        if (NFD_Init() != NFD_OKAY)
        {
            return lux::cxx::unexpected(FileDialogFailure{EFileDialogError::INITIALIZATION, NFD_GetError()});
        }

        struct Session final
        {
            ~Session()
            {
                NFD_Quit();
            }
        } session;

        nfdopendialogu8args_t arguments{};
        if (parent && !NFD_GetNativeWindowFromGLFWWindow(parent->handle(), &arguments.parentWindow))
        {
            return lux::cxx::unexpected(FileDialogFailure{EFileDialogError::INVALID_PARENT, {}});
        }
        std::vector<nfdu8filteritem_t> native_filters;
        native_filters.reserve(filters.size());
        for (const auto& filter : filters)
        {
            native_filters.push_back({filter.name, filter.extensions});
        }
        arguments.filterList = native_filters.data();
        arguments.filterCount = static_cast<nfdfiltersize_t>(native_filters.size());
        const auto utf8 = directory.u8string();
        arguments.defaultPath = utf8.empty() ? nullptr : reinterpret_cast<const char*>(utf8.c_str());
        nfdu8char_t* selection{};
        const auto result = NFD_OpenDialogU8_With(&selection, &arguments);
        if (result == NFD_CANCEL)
        {
            return std::nullopt;
        }
        if (result != NFD_OKAY)
        {
            return lux::cxx::unexpected(FileDialogFailure{EFileDialogError::PLATFORM, NFD_GetError()});
        }

        struct Path final
        {
            nfdu8char_t* value;

            ~Path()
            {
                NFD_FreePathU8(value);
            }
        } path{selection};

        return std::filesystem::u8path(selection);
    }

    catch (const std::filesystem::filesystem_error& error)
    {
        return lux::cxx::unexpected(FileDialogFailure{EFileDialogError::PLATFORM, error.what()});
    }
} // namespace lux::window
