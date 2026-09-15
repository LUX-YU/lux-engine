#pragma once

#if defined(LUX_EDITOR_PUBLICATION_DIAGNOSTICS)
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace lux::editor::detail
{
    inline void publicationBoundary(std::string_view phase, std::size_t index)
    {
        const auto *requested = std::getenv("LUX_EDITOR_PUBLICATION_INTERRUPT");
        const auto boundary = std::string(phase) + ":" + std::to_string(index);
        if (requested && boundary == requested)
        {
            std::printf("DELIBERATE PUBLICATION INTERRUPTION boundary=%s exit=86\n", boundary.c_str());
            std::fflush(stdout);
            std::_Exit(86);
        }
    }
} // namespace lux::editor::detail
#define LUX_EDITOR_PUBLICATION_BOUNDARY(phase, index) ::lux::editor::detail::publicationBoundary(phase, index)
#define LUX_EDITOR_IO(kind, path, bytes)                                                                               \
    std::printf("DIAGNOSTIC project_io kind=%s bytes=%zu path=%s\n", kind, static_cast<std::size_t>(bytes),            \
                (path).string().c_str())
#else
#define LUX_EDITOR_PUBLICATION_BOUNDARY(phase, index) ((void)0)
#define LUX_EDITOR_IO(kind, path, bytes) ((void)0)
#endif
