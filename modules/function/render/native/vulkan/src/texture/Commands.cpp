#include <lux/engine/render/resource/Texture.hpp>
#include <lux/engine/render/native/vulkan/Registration.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>
#include <lux/engine/render/native/vulkan/transfer/GpuWork.hpp>
#include <lux/engine/render/resource/Upload.hpp>

namespace lux::render
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

        RenderResult<void> setTexture(vulkan::DeviceSession& device, const SetTexture::Request&,
            Reply<SetTexture>& reply) noexcept
        {
            auto pending = reply.defer(sizeof(SetTexture::Response));
            if (!pending) return cxx::unexpected(pending.error());
            auto& resources = device.resources();
            auto instance = resources.getOrCreate(resources.root(), "default", TextureStorage::description, nullptr);
            if (!instance) return pending->fail(instance.error());
            auto& storage = *static_cast<vulkan::TextureStorage*>(resources.native(*instance));
            return upload(storage, std::move(*pending));
        }

        RenderResult<void> setRegion(vulkan::DeviceSession& device, const SetTextureRegion::Request&,
            Reply<SetTextureRegion>& reply) noexcept
        {
            auto pending = reply.defer(sizeof(SetTextureRegion::Response));
            if (!pending) return cxx::unexpected(pending.error());
            auto& registry = device.resources();
            auto id = registry.find(registry.root(), "default", TextureStorage::description);
            auto* storage = id ? static_cast<vulkan::TextureStorage*>(registry.native(*id)) : nullptr;
            if (!storage) return pending->fail(RenderError{vulkan::kUnsupported});
            return upload(*storage, std::move(*pending));
        }

        RenderResult<void> release(vulkan::DeviceSession& device, const ReleaseTexture::Request& input,
            Reply<ReleaseTexture>& context) noexcept
        {
            auto& registry = device.resources();
            auto id = registry.find(registry.root(), "default", TextureStorage::description);
            auto* storage = id ? static_cast<vulkan::TextureStorage*>(registry.native(*id)) : nullptr;
            if (!storage) return cxx::unexpected(RenderError{vulkan::kUnsupported});
            if (auto result = storage->release(input.texture, input.revision); !result) return result;
            return context.complete(Ack{});
        }

        RenderResult<void> query(vulkan::DeviceSession& device, const QueryTexture::Request& input,
            Reply<QueryTexture>& context) noexcept
        {
            auto& registry = device.resources();
            auto storage = registry.tryGet<TextureStorage>(registry.root(), "default");
            if (!storage) return cxx::unexpected(storage.error());
            auto result = storage->query(input);
            if (!result) return cxx::unexpected(result.error());
            return context.complete(*result);
        }
    }

    template <> StorageRegistration nativeStorageBinding<TextureStorage>() noexcept
    {
        static const std::array handlers{
            vulkan::nativeCommand<SetTexture, setTexture>(),
            vulkan::nativeCommand<SetTextureRegion, setRegion>(),
            vulkan::nativeCommand<ReleaseTexture, release>(),
            vulkan::nativeCommand<QueryTexture, query>()
        };
        return {&TextureStorage::description, handlers, vulkan::textureStorageFactory()};
    }
}
