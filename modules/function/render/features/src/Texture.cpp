#include <lux/engine/render/features/Texture.hpp>

#include <lux/cxx/core/CheckedArithmetic.hpp>

namespace lux::render
{
    RenderResult<std::size_t> CommandValueTraits<SetTexture::Request>::retainedBytes(
        const SetTexture::Request& request
    ) noexcept
    {
        const auto bytes = cxx::checkedAdd(sizeof(request), request.source.retainedBytes());
        if (!bytes) return cxx::unexpected(RenderError{kProtocolContract});
        return *bytes;
    }

    RenderResult<std::size_t> CommandValueTraits<SetTextureRegion::Request>::retainedBytes(
        const SetTextureRegion::Request& request
    ) noexcept
    {
        const auto bytes = cxx::checkedAdd(sizeof(request), request.source.retainedBytes());
        if (!bytes) return cxx::unexpected(RenderError{kProtocolContract});
        return *bytes;
    }
}
