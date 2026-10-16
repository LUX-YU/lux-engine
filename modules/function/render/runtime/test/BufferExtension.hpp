#pragma once

#include "../../native/vulkan/test/extension/NativeBuffer.hpp"
#include "../../native/vulkan/test/extension/Feature.hpp"
#include <lux/engine/render/native/vulkan/transfer/GpuCompletion.hpp>
#include <lux/engine/render/features/Registration.hpp>
#include <lux/engine/render/resource/Factory.hpp>

namespace extension_test
{
    inline RenderResult<void> setBuffer(void* native, CommandInvocation& invocation) noexcept
    {
        CommandContext<SetBuffer> context(invocation);
        const auto& input = context.request();
        auto& session = *static_cast<vk::DeviceSession*>(native);
        BufferBinding response;
        auto reservation = vk::GpuCompletionReservation::reserve(invocation, session.queue(), &response);
        if (!reservation) return lux::cxx::unexpected(reservation.error());
        auto& registry = session.resources();
        auto instance = registry.getOrCreate(registry.root(), "values", TestBufferStorage::description, nullptr);
        if (!instance) return lux::cxx::unexpected(instance.error());
        auto& storage = *static_cast<NativeBuffer*>(registry.native(*instance));
        auto upload = storage.upload(input.source.bytes().view(), input.id);
        if (!upload) return lux::cxx::unexpected(upload.error());
        response.id = (*upload)->id;
        if (auto ready = reservation->setReply(&response); !ready) return ready;
        auto work = vk::GpuWork::pending<[](NativeBuffer::Upload& value, vk::DeviceSession&) noexcept {
            return value.publish();
        }>(session, *upload);
        if (!work) return lux::cxx::unexpected(work.error());
        return reservation->commitWork(*work);
    }

    inline RenderResult<void> removeBuffer(void* native, CommandInvocation& invocation) noexcept
    {
        CommandContext<RemoveBuffer> context(invocation);
        const auto& input = context.request();
        auto& registry = static_cast<vk::DeviceSession*>(native)->resources();
        auto id = registry.find(registry.root(), "values", TestBufferStorage::description);
        if (!id) return lux::cxx::unexpected(id.error());
        auto result = static_cast<NativeBuffer*>(registry.native(*id))->remove(input.id);
        return result ? context.complete(Ack{}) : result;
    }

    inline RenderResult<void> readBuffer(void* native, CommandInvocation& invocation) noexcept
    {
        CommandContext<ReadBuffer> context(invocation);
        const auto& input = context.request();
        auto& session = *static_cast<vk::DeviceSession*>(native);
        auto& registry = session.resources();
        auto id = registry.find(registry.root(), "values", TestBufferStorage::description);
        if (!id) return lux::cxx::unexpected(id.error());
        struct Read final
        {
            vk::DeviceSession& session;
            StorageInstanceId storage;
            vk::PreparedStorageBinding source;
            vk::BufferPermit buffer;
            std::optional<vk::SubmissionTicket> ticket;
            std::shared_ptr<std::vector<std::byte>> output;
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
        auto output = std::make_shared<std::vector<std::byte>>(16);
        auto payload = RenderPayload::fromVector(output, 0, output->size());
        if (!payload) return lux::cxx::unexpected(payload.error());
        ReadBuffer::Response response{std::move(*payload)};
        auto reservation = vk::GpuCompletionReservation::reserve(invocation, session.queue(), &response);
        if (!reservation) return lux::cxx::unexpected(reservation.error());
        registry.retain(*id);
        auto owner = std::make_unique<Read>(session, *id, vk::PreparedStorageBinding{}, vk::BufferPermit{},
            std::optional<vk::SubmissionTicket>{}, output);
        vk::ResourcePlan plan{"values", GraphBufferDesc{16}, EGraphOrigin::IMPORTED};
        auto argument = encodeWire(input, {}, {8, 0});
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

}

namespace lux::render
{
    template <> inline StorageRegistration nativeStorageBinding<extension_test::TestBufferStorage>() noexcept
    {
        using namespace extension_test;
        static const std::array handlers{
            CommandHandler{&commandDescriptor<SetBuffer>(), setBuffer, nullptr, ECommandOwner::DEVICE_SESSION, true},
            CommandHandler{&commandDescriptor<RemoveBuffer>(), removeBuffer, nullptr, ECommandOwner::DEVICE_SESSION},
            CommandHandler{&commandDescriptor<ReadBuffer>(), readBuffer, nullptr, ECommandOwner::DEVICE_SESSION, true}
        };
        return {&TestBufferStorage::description, handlers, bufferFactory()};
    }

    template <> inline FeatureRegistration nativeFeatureBinding<extension_test::TestBufferFeature>() noexcept
    {
        return {&extension_test::definition, extension_test::programs, {}};
    }
}
