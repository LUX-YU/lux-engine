#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>

namespace lux::render::vulkan
{
    template <class Response> static exec::task<Response> rejected(RenderError error) noexcept
    {
        co_return cxx::unexpected(error);
    }

    exec::task<RenderResult<TextureId>> TextureCommandHandler::createTextureSource(CreateTexture::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<TextureStorage>(input.storage);
        if (!storage)
        {
            return rejected<RenderResult<TextureId>>(storage.error());
        }
        return render::upload(**storage, std::move(input));
    }

    exec::task<RenderResult<TextureId>> TextureCommandHandler::createPatchedTextureSource(
        CreatePatchedTexture::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<TextureStorage>(input.texture.storage);
        if (!storage)
        {
            return rejected<RenderResult<TextureId>>(storage.error());
        }
        return render::upload(**storage, std::move(input));
    }

    void TextureCommandHandler::releaseTexture(
        const ReleaseTexture::Request& input,
        ReleaseTexture::Response& response
    ) noexcept
    {
        auto storage = device_.resources().borrow<TextureStorage>(input.storage);
        if (!storage)
        {
            response.error = storage.error();
            return;
        }
        const auto released = (*storage)->release(input);
        response.error = released ? RenderError{} : released.error();
    }

} // namespace lux::render::vulkan

namespace lux::render
{
    StorageFactory StorageProvider<render::TextureStorage>::factory() noexcept
    {
        return vulkan::textureStorageFactory();
    }

    CppType StorageProvider<render::TextureStorage>::implementationType() noexcept
    {
        return cppType<Storage>();
    }
} // namespace lux::render
