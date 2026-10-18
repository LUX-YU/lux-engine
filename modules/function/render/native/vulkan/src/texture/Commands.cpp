#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>
#include <lux/engine/render/native/vulkan/transfer/GpuWork.hpp>
#include <lux/engine/render/resource/Texture.hpp>
#include <lux/engine/render/resource/Upload.hpp>

namespace lux::render::vulkan
{
    GpuWorkSender<SetTexture::Response> TextureCommandHandler::setTexture(SetTexture::Request&& input) noexcept
    {
        auto storage = device_.resources().borrow<TextureStorage>(input.texture.storage);
        if (!storage)
        {
            return GpuWorkSender<SetTexture::Response>{storage.error()};
        }
        auto candidate = render::upload(**storage, input);
        if (!candidate)
        {
            return GpuWorkSender<SetTexture::Response>{candidate.error()};
        }
        return {std::move(candidate->first), std::move(candidate->second), std::move(input.source)};
    }

    GpuWorkSender<SetTextureRegion::Response> TextureCommandHandler::updateTextureRegion(
        SetTextureRegion::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<TextureStorage>(input.texture.storage);
        if (!storage)
        {
            return GpuWorkSender<SetTextureRegion::Response>{storage.error()};
        }
        auto candidate = render::upload(**storage, input);
        if (!candidate)
        {
            return GpuWorkSender<SetTextureRegion::Response>{candidate.error()};
        }
        return {std::move(candidate->first), std::move(candidate->second), std::move(input.source)};
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
