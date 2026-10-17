#include <lux/engine/render/protocol/AsyncCommand.hpp>
#include <lux/engine/render/resource/Texture.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureCommands.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>
#include <lux/engine/render/native/vulkan/transfer/GpuWork.hpp>
#include <lux/engine/render/resource/Upload.hpp>

namespace lux::render::vulkan
{
    namespace
    {
        template <class C> RenderResult<void> upload(
            vulkan::TextureStorage& storage, Reply<C> pending
        ) noexcept
        {
            // The former inline input borrow ended at defer(). The single stable Request owns the source.
            auto candidate = render::upload(storage, pending.request());
            if (!candidate) return pending.fail(candidate.error());
            return pending.completeWhen(vulkan::GpuWorkSender<typename C::Response>{
                std::move(candidate->first), std::move(candidate->second)
            });
        }
    }

    RenderResult<void> TextureCommands::set(const SetTexture::Request&,
        Reply<SetTexture>& reply) noexcept
    {
        auto pending = reply.defer();
        if (!pending) return cxx::unexpected(pending.error());
        const auto id = pending->request().texture.storage;
        auto storage = device_.resources().borrow<vulkan::TextureStorage>(id);
        if (!storage) return pending->fail(storage.error());
        return upload(**storage, std::move(*pending));
    }

    RenderResult<void> TextureCommands::region(const SetTextureRegion::Request&,
        Reply<SetTextureRegion>& reply) noexcept
    {
        auto pending = reply.defer();
        if (!pending) return cxx::unexpected(pending.error());
        auto storage = device_.resources().borrow<vulkan::TextureStorage>(pending->request().texture.storage);
        if (!storage) return pending->fail(storage.error());
        return upload(**storage, std::move(*pending));
    }

    RenderResult<void> TextureCommands::release(const ReleaseTexture::Request& input,
        Reply<ReleaseTexture>& context) noexcept
    {
        auto storage = device_.resources().borrow<vulkan::TextureStorage>(input.texture.storage);
        if (!storage) return cxx::unexpected(storage.error());
        if (auto result = (*storage)->release(input.texture, input.revision); !result) return result;
        return context.complete(Ack{});
    }

    RenderResult<void> TextureCommands::query(const QueryTexture::Request& input,
        Reply<QueryTexture>& context) noexcept
    {
        auto storage = device_.resources().tryGet<render::TextureStorage>(input.storage);
        if (!storage) return cxx::unexpected(storage.error());
        auto result = storage->query(input);
        if (!result) return cxx::unexpected(result.error());
        return context.complete(*result);
    }

}

namespace lux::render
{
    StorageFactory StorageProvider<render::TextureStorage>::factory() noexcept
    {
        return vulkan::textureStorageFactory();
    }

    StorageCppType StorageProvider<render::TextureStorage>::implementationType() noexcept
    {
        return storageCppType<Storage>();
    }
}
