#include <lux/engine/render/resource/Texture.hpp>

#include <lux/cxx/core/CheckedArithmetic.hpp>

namespace lux::render
{
    RenderResult<std::size_t> CommandValueTraits<TextureWrite>::retainedBytes(
        const TextureWrite& request
    ) noexcept
    {
        const auto bytes = cxx::checkedAdd(sizeof(request), request.source.retainedBytes());
        if (!bytes) return cxx::unexpected(RenderError{kProtocolContract});
        return *bytes;
    }

    RenderResult<std::size_t> CommandValueTraits<TextureRegionWrite>::retainedBytes(
        const TextureRegionWrite& request
    ) noexcept
    {
        const auto bytes = cxx::checkedAdd(sizeof(request), request.source.retainedBytes());
        if (!bytes) return cxx::unexpected(RenderError{kProtocolContract});
        return *bytes;
    }
}
