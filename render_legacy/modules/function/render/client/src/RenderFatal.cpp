#include <lux/engine/function/render/client/core/RenderFatal.hpp>

#include <cstdio>
#include <cstdlib>

namespace lux::render
{
    void renderFatal(std::string_view what, const std::source_location& where) noexcept
    {
        std::fprintf(
            stderr,
            "\n[render] 致命:%.*s\n  位置:%s:%u (%s)\n",
            static_cast<int>(what.size()),
            what.data(),
            where.file_name(),
            where.line(),
            where.function_name()
        );
        std::fflush(stderr);
        std::abort();
    }
} // namespace lux::render
