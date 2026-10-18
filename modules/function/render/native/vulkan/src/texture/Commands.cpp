#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>
#include <lux/engine/render/resource/Texture.hpp>
#include <lux/engine/render/resource/Upload.hpp>

namespace lux::render::vulkan
{
    static exec::task<SetTexture::Response> rejectedTexture(RenderError error) noexcept
    {
        co_return cxx::unexpected(error);
    }

    exec::task<SetTexture::Response> TextureCommandHandler::setTexture(SetTexture::Request&& input) noexcept
    {
        auto storage = device_.resources().borrow<TextureStorage>(input.texture.storage);
        if (!storage)
        {
            return rejectedTexture(storage.error());
        }
        return render::upload(**storage, std::move(input));
    }

    exec::task<SetTextureRegion::Response> TextureCommandHandler::updateTextureRegion(SetTextureRegion::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<TextureStorage>(input.texture.storage);
        if (!storage)
        {
            return rejectedTexture(storage.error());
        }
        return render::upload(**storage, std::move(input));
    }

    void TextureCommandHandler::releaseTexture(
        const ReleaseTexture::Request& input,
        ReleaseTexture::Response& response
    ) noexcept
    {
        auto storage = device_.resources().borrow<vulkan::TextureStorage>(input.texture.storage);
        if (!storage)
        {
            response = cxx::unexpected(storage.error());
            return;
        }
        if (auto result = (*storage)->release(input.texture, input.revision); !result)
        {
            response = cxx::unexpected(result.error());
            return;
        }
        response = Ack{};
    }

    void TextureCommandHandler::queryTexture(
        const QueryTexture::Request& input,
        QueryTexture::Response& response
    ) noexcept
    {
        auto storage = device_.resources().tryGet<render::TextureStorage>(input.storage);
        if (!storage)
        {
            response = cxx::unexpected(storage.error());
            return;
        }
        auto result = storage->query(input);
        if (!result)
        {
            response = cxx::unexpected(result.error());
            return;
        }
        response = *result;
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
