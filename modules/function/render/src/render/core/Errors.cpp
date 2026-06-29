#include <lux/engine/render/core/Errors.hpp>

namespace lux::render
{
    const RenderErrorCategory& renderErrorCategory()
    {
        static const RenderErrorCategory instance;
        return instance;
    }
} // namespace lux::render
