#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/storage/SampledTexture2DCommandHandler.hpp>
#include <lux/engine/render/storage/SampledTexture2DStorage.hpp>

namespace lux::render::detail
{
    template <class Response> static exec::task<Response> rejected(RenderError error) noexcept
    {
        co_return cxx::unexpected(error);
    }

    exec::task<RenderResult<TextureId>> SampledTexture2DCommandHandler::createTextureSource(
        CreateTexture::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<SampledTexture2DStorage>(input.storage);
        if (!storage)
        {
            return rejected<RenderResult<TextureId>>(storage.error());
        }
        return render::upload(**storage, std::move(input));
    }

    exec::task<RenderResult<TextureId>> SampledTexture2DCommandHandler::createPatchedTextureSource(
        CreatePatchedTexture::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<SampledTexture2DStorage>(input.texture.storage);
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
        auto storage = device_.resources().borrow<SampledTexture2DStorage>(input.storage);
        if (!storage)
        {
            response.error = storage.error();
            return;
        }
        const auto released = (*storage)->release(input);
        response.error = released ? RenderError{} : released.error();
    }

} // namespace lux::render::detail

#include <lux/engine/render/runtime/Registration.hpp>

namespace lux::render
{
    template <>
    RenderResult<void> RenderServer::prepareStorage<SampledTexture2DStorageDescriptor>(
        RegistrationBatch& batch,
        ConfigValue configuration,
        bool use_existing,
        std::string_view handler_key
    ) noexcept
    {
        return prepareStorageDefinition<
            SampledTexture2DStorageDescriptor,
            detail::SampledTexture2DStorage,
            detail::SampledTexture2DCommandHandler>(
            batch,
            detail::textureStorageFactory(),
            std::move(configuration),
            use_existing,
            handler_key
        );
    }

    template LUX_RENDER_PUBLIC RenderResult<StorageTypeId> RenderServer::registerStorage<
        SampledTexture2DStorageDescriptor>(std::string_view) noexcept;
} // namespace lux::render
