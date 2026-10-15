#pragma once

#include "../../native/vulkan/test/extension/NativeBuffer.hpp"
#include "../../native/vulkan/test/extension/Feature.hpp"
#include <lux/engine/render/runtime/vulkan/GpuCompletion.hpp>
#include <lux/engine/render/runtime/Operations.hpp>

namespace extension_test
{
    struct UploadBuffer final { BufferId id; ExternalDataRef source; };
}

namespace lux::render
{
    template <> struct DataSchema<extension_test::UploadBuffer>
    {
        static inline constexpr DataField fields[]{
            {"index", cxx::EScalarKind::U32}, {"generation", cxx::EScalarKind::U32},
            {"source", cxx::EScalarKind::U8, 1, EDataFieldKind::EXTERNAL}
        };
        static inline constexpr DataSchemaDescriptor schema{
            DataTypeIdView("test.extension.UploadBuffer"), {1, 0}, {1, 0}, fields, 28, 4
        };
        static const DataSchemaDescriptor& describe() noexcept { return schema; }
        static RenderResult<std::size_t> wireSize(const extension_test::UploadBuffer&) noexcept { return 28; }
        static RenderResult<void> encode(const extension_test::UploadBuffer& value, cxx::BinarySpanWriter& out) noexcept
        {
            out.writeUnsigned(value.id.index);
            out.writeUnsigned(value.id.gen);
            out.writeUnsigned(value.source.attachment_index);
            out.writeUnsigned(value.source.offset);
            out.writeUnsigned(value.source.size);
            return {};
        }
        template <class Wire> static RenderResult<extension_test::UploadBuffer> decode(cxx::BinaryReader& in, Wire) noexcept
        {
            extension_test::UploadBuffer value;
            in.readUnsigned(value.id.index);
            in.readUnsigned(value.id.gen);
            in.readUnsigned(value.source.attachment_index);
            in.readUnsigned(value.source.offset);
            in.readUnsigned(value.source.size);
            return value;
        }
    };
}

namespace extension_test
{
    inline const OperationDescriptor upload_operation = operationDescriptor<UploadBuffer, BufferBinding>(
        "test.extension.UploadBuffer", {1, 0}, ECompletionContract::RESOURCE_GPU_READY);
    inline const OperationDescriptor remove_operation = operationDescriptor<BufferBinding>("test.extension.RemoveBuffer");
    inline const OperationDescriptor read_operation = operationDescriptor<BufferBinding, ReadbackResult>(
        "test.extension.ReadBuffer", {1, 0}, ECompletionContract::RESOURCE_GPU_READY);

    inline RenderResult<void> uploadBuffer(void*, RenderRequest& request, OperationContext& context) noexcept
    {
        auto input = request.decode<UploadBuffer>();
        if (!input) return lux::cxx::unexpected(input.error());
        std::array<std::byte, 8> reply{};
        auto reservation = context.reserveGpuCompletion(reply);
        if (!reservation) return lux::cxx::unexpected(reservation.error());
        auto& registry = context.resources();
        auto instance = registry.getOrCreate(registry.root(), "values", TestBufferStorage::description, nullptr);
        if (!instance) return lux::cxx::unexpected(instance.error());
        auto& storage = *static_cast<NativeBuffer*>(registry.native(*instance));
        const auto bytes = request.payload().attachments[input->source.attachment_index].subspan(input->source.offset, input->source.size);
        auto upload = storage.upload(bytes, input->id);
        if (!upload) return lux::cxx::unexpected(upload.error());
        lux::cxx::BinarySpanWriter writer(reply);
        (void)DataSchema<BufferBinding>::encode({(*upload)->id}, writer);
        if (auto ready = reservation->setReply(reply); !ready) return ready;
        auto work = vk::GpuWork::pending<[](NativeBuffer::Upload& value, vk::DeviceSession&) noexcept {
            return value.publish();
        }>(GpuCompletionReservation::device(context), *upload);
        if (!work) return lux::cxx::unexpected(work.error());
        return reservation->commitWork(*work);
    }

    inline RenderResult<void> removeBuffer(void*, RenderRequest& request, OperationContext& context) noexcept
    {
        auto input = request.decode<BufferBinding>();
        if (!input) return lux::cxx::unexpected(input.error());
        auto& registry = context.resources();
        auto id = registry.find(registry.root(), "values", TestBufferStorage::description);
        if (!id) return lux::cxx::unexpected(id.error());
        auto result = static_cast<NativeBuffer*>(registry.native(*id))->remove(input->id);
        return result ? request.reply() : result;
    }

    inline RenderResult<void> readBuffer(void*, RenderRequest& request, OperationContext& context) noexcept
    {
        auto input = request.decode<BufferBinding>();
        if (!input) return lux::cxx::unexpected(input.error());
        auto& session = GpuCompletionReservation::device(context);
        auto& registry = context.resources();
        auto id = registry.find(registry.root(), "values", TestBufferStorage::description);
        if (!id) return lux::cxx::unexpected(id.error());
        struct Read final
        {
            vk::DeviceSession& session;
            StorageInstanceId storage;
            vk::PreparedStorageBinding source;
            vk::BufferPermit buffer;
            std::optional<vk::SubmissionTicket> ticket;
            std::shared_ptr<std::array<std::byte, 16>> output;
            ~Read() noexcept
            {
                if (buffer)
                {
                    const auto result = ticket ? session.releaseCompleted(buffer, *ticket) : session.destroyUnsubmitted(buffer);
                    if (!result) std::terminate();
                }
                source.reference.reset();
                session.resources().release(storage);
            }
        };
        auto output = std::make_shared<std::array<std::byte, 16>>();
        const auto attachment = OwnedWireBytes::fromOwner(output, std::span<const std::byte>(*output));
        std::array<std::byte, 20> reply{};
        lux::cxx::BinarySpanWriter writer(reply);
        (void)DataSchema<ReadbackResult>::encode({{0, 0, 16}}, writer);
        auto reservation = context.reserveGpuCompletion(reply, attachment);
        if (!reservation) return lux::cxx::unexpected(reservation.error());
        registry.retain(*id);
        auto owner = std::make_unique<Read>(session, *id, vk::PreparedStorageBinding{}, vk::BufferPermit{},
            std::optional<vk::SubmissionTicket>{}, output);
        vk::ResourcePlan plan{"values", GraphBufferDesc{16}, EGraphOrigin::IMPORTED};
        auto argument = encodeWire(*input, {}, {8, 0});
        if (!argument) return lux::cxx::unexpected(argument.error());
        auto prepared = static_cast<NativeBuffer*>(registry.native(*id))->prepare(plan, argument->view(), owner->source);
        if (!prepared) return prepared;
        auto buffer = session.createBuffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, vk::EMemoryAccess::READBACK);
        if (!buffer) return lux::cxx::unexpected(buffer.error());
        owner->buffer = std::move(*buffer);
        auto commands = session.queue().begin();
        if (!commands) return lux::cxx::unexpected(commands.error());
        VkBufferMemoryBarrier2 before{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        before.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        before.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        before.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        before.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        before.srcQueueFamilyIndex = before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before.buffer = owner->source.buffer->native();
        before.size = VK_WHOLE_SIZE;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &before;
        vkCmdPipelineBarrier2(commands->native(), &dependency);
        const VkBufferCopy region{0, 0, 16};
        vkCmdCopyBuffer(commands->native(), owner->source.buffer->native(), session.borrow(owner->buffer)->native(), 1, &region);
        before.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        before.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        before.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        before.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        before.buffer = session.borrow(owner->buffer)->native();
        vkCmdPipelineBarrier2(commands->native(), &dependency);
        auto ticket = std::move(*commands).submit();
        if (!ticket) return lux::cxx::unexpected(ticket.error());
        owner->ticket = *ticket;
        *owner->source.last_use = *ticket;
        return reservation->commit<[](Read& value, vk::DeviceSession& session) noexcept {
            return session.read(*session.borrow(value.buffer), 0, *value.output);
        }>(*ticket, owner);
    }

    inline StorageRegistration TestBufferStorage::registration() noexcept
    {
        static const std::array handlers{
            OperationHandler{&upload_operation, uploadBuffer, nullptr, true},
            OperationHandler{&remove_operation, removeBuffer},
            OperationHandler{&read_operation, readBuffer, nullptr, true}
        };
        return {&description, handlers, bufferFactory()};
    }

    inline FeatureRegistration TestBufferFeature::registration() noexcept { return {&definition, programs}; }
}
