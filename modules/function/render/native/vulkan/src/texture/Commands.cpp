#include <lux/engine/render/resource/Texture.hpp>
#include <lux/engine/render/native/vulkan/Registration.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>
#include <lux/engine/render/native/vulkan/transfer/GpuCompletion.hpp>
#include <lux/engine/render/resource/Upload.hpp>

namespace lux::render
{
    namespace
    {
        RenderResult<void> commit(vulkan::GpuCompletionReservation& reservation,
            std::pair<TexturePublished, vulkan::GpuWork>& candidate) noexcept
        {
            if (auto prepared = reservation.setReply(&candidate.first); !prepared) return prepared;
            return reservation.commitWork(candidate.second);
        }

        RenderResult<void> setTexture(vulkan::DeviceSession& device, const SetTexture::Request& input,
            Reply<SetTexture>& context) noexcept
        {
            auto& resources = device.resources();
            auto instance = resources.getOrCreate(resources.root(), "default", TextureStorage::description, nullptr);
            if (!instance) return cxx::unexpected(instance.error());
            auto* storage = static_cast<vulkan::TextureStorage*>(resources.native(*instance));
            // Reply storage is funded before preparing or submitting any Native work.
            TexturePublished response;
            auto reservation = vulkan::GpuCompletionReservation::reserve(context, device.queue(), response);
            if (!reservation) return cxx::unexpected(reservation.error());
            auto candidate = render::upload(*storage, input);
            if (!candidate) return cxx::unexpected(candidate.error());
            return commit(*reservation, *candidate);
        }

        RenderResult<void> setRegion(vulkan::DeviceSession& device, const SetTextureRegion::Request& input,
            Reply<SetTextureRegion>& context) noexcept
        {
            auto& registry = device.resources();
            auto id = registry.find(registry.root(), "default", TextureStorage::description);
            auto* storage = id ? static_cast<vulkan::TextureStorage*>(registry.native(*id)) : nullptr;
            if (!storage) return cxx::unexpected(RenderError{vulkan::kUnsupported});
            TexturePublished response;
            auto reservation = vulkan::GpuCompletionReservation::reserve(context, device.queue(), response);
            if (!reservation) return cxx::unexpected(reservation.error());
            auto candidate = render::upload(*storage, input);
            if (!candidate) return cxx::unexpected(candidate.error());
            return commit(*reservation, *candidate);
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
