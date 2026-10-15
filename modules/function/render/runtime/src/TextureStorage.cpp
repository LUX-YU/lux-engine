#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>
#include <lux/engine/render/native/vulkan/transfer/TextureUpload.hpp>
#include <lux/engine/render/native/vulkan/transfer/GpuWork.hpp>

namespace lux::render
{
    namespace
    {
        std::span<const std::byte> source(RenderRequest& request, ExternalDataRef reference) noexcept
        {
            // Dispatch already validates the wire's external ranges; the original request pins its owner.
            return request.payload().attachments[reference.attachment_index].subspan(reference.offset, reference.size);
        }

        RenderResult<void> commit(OperationContext& context, GpuCompletionReservation& reservation, TexturePublished result,
            std::unique_ptr<vulkan::TextureUpload>& upload) noexcept
        {
            std::array<std::byte, 16> bytes;
            cxx::BinarySpanWriter writer(bytes);
            (void)DataSchema<TexturePublished>::encode(result, writer);
            if (auto prepared = reservation.setReply(bytes); !prepared) return prepared;
            auto work = vulkan::GpuWork::pending<
                [](vulkan::TextureUpload& value, vulkan::DeviceSession&) noexcept { return value.publish(); }
            >(context.native(), upload);
            if (!work) return cxx::unexpected(work.error());
            return reservation.commitWork(*work);
        }

        RenderResult<void> upload(void*, RenderRequest& request, OperationContext& context) noexcept
        {
            auto input = request.decode<UploadTexture>();
            if (!input) return cxx::unexpected(input.error());
            auto& resources = context.native().resources();
            auto instance = resources.getOrCreate(resources.root(), "default", TextureStorage::description, nullptr);
            if (!instance) return cxx::unexpected(instance.error());
            auto* storage = static_cast<vulkan::TextureStorage*>(resources.native(*instance));
            // Extent and reply space funded before any Native allocation/prepare/submit. Values follow reservation.
            const std::array<std::byte, 16> extent{};
            auto reservation = context.reserveGpuCompletion(extent);
            if (!reservation) return cxx::unexpected(reservation.error());
            if (auto admitted = storage->admit(input->source.size, input->texture.isNull()); !admitted) return admitted;
            auto candidate = context.native().uploadTexture(input->info, source(request, input->source));
            if (!candidate) return cxx::unexpected(candidate.error());
            auto publication = storage->reserve(**candidate, input->texture, input->expected_revision);
            if (!publication) return cxx::unexpected(publication.error());
            return commit(context, *reservation, *publication, *candidate);
        }

        RenderResult<void> region(void*, RenderRequest& request, OperationContext& context) noexcept
        {
            auto input = request.decode<UploadTextureRegion>();
            if (!input) return cxx::unexpected(input.error());
            auto* storage = context.native().textures();
            if (!storage) return cxx::unexpected(RenderError{vulkan::kUnsupported});
            const std::array<std::byte, 16> extent{};
            auto reservation = context.reserveGpuCompletion(extent);
            if (!reservation) return cxx::unexpected(reservation.error());
            auto candidate = storage->uploadRegion(input->texture, input->expected_revision,
                input->region, source(request, input->source));
            if (!candidate) return cxx::unexpected(candidate.error());
            return commit(context, *reservation, {input->texture, input->expected_revision + 1}, *candidate);
        }

        RenderResult<void> release(void*, RenderRequest& request, OperationContext& context) noexcept
        {
            auto input = request.decode<TexturePublished>();
            if (!input) return cxx::unexpected(input.error());
            auto* storage = context.native().textures();
            if (!storage) return cxx::unexpected(RenderError{vulkan::kUnsupported});
            if (auto result = storage->release(input->texture, input->revision); !result) return result;
            return request.reply();
        }
    }

    StorageRegistration TextureStorage::registration() noexcept
    {
        static const std::array handlers{
            OperationHandler{&uploadTextureOperation(), upload, nullptr, true},
            OperationHandler{&uploadTextureRegionOperation(), region, nullptr, true},
            OperationHandler{&releaseTextureOperation(), release}
        };
        return {&description, handlers, vulkan::textureStorageFactory()};
    }
}
