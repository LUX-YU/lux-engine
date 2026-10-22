#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/storage/SampledTexture2DCommandHandler.hpp>
#include <lux/engine/render/storage/SampledTexture2DStorage.hpp>

namespace lux::render::detail
{
    template <class Response> static exec::task<Response> rejected(RenderError error) noexcept
    {
        co_return cxx::unexpected(error);
    }

    exec::task<RenderResult<TextureId>> SampledTexture2DCommandHandler::uploadTexture(CreateTexture::Request&& input
    ) noexcept
    {
        auto storage = resources_.borrow<SampledTexture2DStorage>(input.storage);
        if (!storage)
        {
            return rejected<RenderResult<TextureId>>(storage.error());
        }
        return render::upload(**storage, std::move(input));
    }

    exec::task<RenderResult<TextureId>> SampledTexture2DCommandHandler::uploadPatchedTexture(
        CreatePatchedTexture::Request&& input
    ) noexcept
    {
        auto storage = resources_.borrow<SampledTexture2DStorage>(input.texture.storage);
        if (!storage)
        {
            return rejected<RenderResult<TextureId>>(storage.error());
        }
        return render::upload(**storage, std::move(input));
    }

    void SampledTexture2DCommandHandler::releaseTexture(
        const ReleaseTexture::Request& input,
        ReleaseTexture::Response& response
    ) noexcept
    {
        auto storage = resources_.borrow<SampledTexture2DStorage>(input.storage);
        if (!storage)
        {
            response.error = storage.error();
            return;
        }
        const auto released = (*storage)->release(input);
        response.error = released ? RenderError{} : released.error();
    }

} // namespace lux::render::detail
