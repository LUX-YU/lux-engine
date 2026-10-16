#include <lux/engine/render/features/Texture.hpp>
#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>
#include <lux/engine/render/native/vulkan/transfer/GpuCompletion.hpp>
#include <lux/engine/render/native/vulkan/transfer/TextureUpload.hpp>

namespace lux::render
{
    namespace
    {
        RenderResult<void> commit(vulkan::DeviceSession& device, vulkan::GpuCompletionReservation& reservation,
            TexturePublished result, std::unique_ptr<vulkan::TextureUpload>& upload) noexcept
        {
            if (auto prepared = reservation.setReply(&result); !prepared) return prepared;
            auto work = vulkan::GpuWork::pending<
                [](vulkan::TextureUpload& value, vulkan::DeviceSession&) noexcept { return value.publish(); }
            >(device, upload);
            if (!work) return cxx::unexpected(work.error());
            return reservation.commitWork(*work);
        }

        RenderResult<void> setTexture(void* native, CommandInvocation& invocation) noexcept
        {
            CommandContext<SetTexture> context(invocation);
            const auto& input = context.request();
            auto& device = *static_cast<vulkan::DeviceSession*>(native);
            auto& resources = device.resources();
            auto instance = resources.getOrCreate(resources.root(), "default", TextureStorage::description, nullptr);
            if (!instance) return cxx::unexpected(instance.error());
            auto* storage = static_cast<vulkan::TextureStorage*>(resources.native(*instance));
            // Reply storage is funded before preparing or submitting any Native work.
            TexturePublished response;
            auto reservation = vulkan::GpuCompletionReservation::reserve(invocation, device.queue(), &response);
            if (!reservation) return cxx::unexpected(reservation.error());
            const auto source = input.source.bytes().view();
            if (auto admitted = storage->admit(source.size(), input.texture.isNull()); !admitted) return admitted;
            auto candidate = vulkan::TextureUpload::create(device, input.info, source);
            if (!candidate) return cxx::unexpected(candidate.error());
            auto publication = storage->reserve(**candidate, input.texture, input.expected_revision);
            if (!publication) return cxx::unexpected(publication.error());
            return commit(device, *reservation, *publication, *candidate);
        }

        RenderResult<void> setRegion(void* native, CommandInvocation& invocation) noexcept
        {
            CommandContext<SetTextureRegion> context(invocation);
            const auto& input = context.request();
            auto& device = *static_cast<vulkan::DeviceSession*>(native);
            auto& registry = device.resources();
            auto id = registry.find(registry.root(), "default", TextureStorage::description);
            auto* storage = id ? static_cast<vulkan::TextureStorage*>(registry.native(*id)) : nullptr;
            if (!storage) return cxx::unexpected(RenderError{vulkan::kUnsupported});
            TexturePublished response;
            auto reservation = vulkan::GpuCompletionReservation::reserve(invocation, device.queue(), &response);
            if (!reservation) return cxx::unexpected(reservation.error());
            auto candidate = storage->uploadRegion(input.texture, input.expected_revision,
                input.region, input.source.bytes().view());
            if (!candidate) return cxx::unexpected(candidate.error());
            return commit(device, *reservation, {input.texture, input.expected_revision + 1}, *candidate);
        }

        RenderResult<void> release(void* native, CommandInvocation& invocation) noexcept
        {
            CommandContext<ReleaseTexture> context(invocation);
            const auto& input = context.request();
            auto& registry = static_cast<vulkan::DeviceSession*>(native)->resources();
            auto id = registry.find(registry.root(), "default", TextureStorage::description);
            auto* storage = id ? static_cast<vulkan::TextureStorage*>(registry.native(*id)) : nullptr;
            if (!storage) return cxx::unexpected(RenderError{vulkan::kUnsupported});
            if (auto result = storage->release(input.texture, input.revision); !result) return result;
            return context.complete(Ack{});
        }

        RenderResult<void> query(void* native, CommandInvocation& invocation) noexcept
        {
            CommandContext<QueryTexture> context(invocation);
            auto& registry = static_cast<vulkan::DeviceSession*>(native)->resources();
            auto storage = registry.tryGet<TextureStorage>(registry.root(), "default");
            if (!storage) return cxx::unexpected(storage.error());
            auto result = storage->query(context.request());
            if (!result) return cxx::unexpected(result.error());
            return context.complete(*result);
        }
    }

    template <> StorageRegistration nativeStorageBinding<TextureStorage>() noexcept
    {
        static const std::array handlers{
            CommandHandler{&commandDescriptor<SetTexture>(), setTexture, nullptr, ECommandOwner::DEVICE_SESSION, true},
            CommandHandler{&commandDescriptor<SetTextureRegion>(), setRegion, nullptr, ECommandOwner::DEVICE_SESSION, true},
            CommandHandler{&commandDescriptor<ReleaseTexture>(), release, nullptr, ECommandOwner::DEVICE_SESSION},
            CommandHandler{&commandDescriptor<QueryTexture>(), query, nullptr, ECommandOwner::DEVICE_SESSION}
        };
        return {&TextureStorage::description, handlers, vulkan::textureStorageFactory()};
    }
}
