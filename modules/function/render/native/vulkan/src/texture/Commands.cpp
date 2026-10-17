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
        auto pending = reply.defer(sizeof(SetTexture::Response));
        if (!pending) return cxx::unexpected(pending.error());
        auto& resources = device_.resources();
        auto instance = resources.getOrCreate(resources.root(), "default", render::TextureStorage::description, nullptr);
        if (!instance) return pending->fail(instance.error());
        auto& storage = *static_cast<vulkan::TextureStorage*>(resources.native(*instance));
        return upload(storage, std::move(*pending));
    }

    RenderResult<void> TextureCommands::region(const SetTextureRegion::Request&,
        Reply<SetTextureRegion>& reply) noexcept
    {
        auto pending = reply.defer(sizeof(SetTextureRegion::Response));
        if (!pending) return cxx::unexpected(pending.error());
        auto& registry = device_.resources();
        auto id = registry.find(registry.root(), "default", render::TextureStorage::description);
        auto* storage = id ? static_cast<vulkan::TextureStorage*>(registry.native(*id)) : nullptr;
        if (!storage) return pending->fail(RenderError{vulkan::kUnsupported});
        return upload(*storage, std::move(*pending));
    }

    RenderResult<void> TextureCommands::release(const ReleaseTexture::Request& input,
        Reply<ReleaseTexture>& context) noexcept
    {
        auto& registry = device_.resources();
        auto id = registry.find(registry.root(), "default", render::TextureStorage::description);
        auto* storage = id ? static_cast<vulkan::TextureStorage*>(registry.native(*id)) : nullptr;
        if (!storage) return cxx::unexpected(RenderError{vulkan::kUnsupported});
        if (auto result = storage->release(input.texture, input.revision); !result) return result;
        return context.complete(Ack{});
    }

    RenderResult<void> TextureCommands::query(const QueryTexture::Request& input,
        Reply<QueryTexture>& context) noexcept
    {
        auto& registry = device_.resources();
        auto storage = registry.tryGet<render::TextureStorage>(registry.root(), "default");
        if (!storage) return cxx::unexpected(storage.error());
        auto result = storage->query(input);
        if (!result) return cxx::unexpected(result.error());
        return context.complete(*result);
    }

    StorageFactory StorageProvider<render::TextureStorage>::factory() noexcept
    {
        return textureStorageFactory();
    }

    StorageCppType StorageProvider<render::TextureStorage>::implementationType() noexcept
    {
        return storageCppType<Storage>();
    }
}
