#pragma once

#include "../../native/vulkan/test/extension/NativeBuffer.hpp"
#include "../../native/vulkan/test/extension/Feature.hpp"
#include "../../native/vulkan/test/SubmissionSender.hpp"
#include <lux/engine/render/features/Registration.hpp>
#include <lux/engine/render/native/vulkan/Registration.hpp>

namespace extension_test
{
    class BufferCommands final
    {
    public:
        static constexpr std::string_view name = "test.extension.BufferCommands";
        explicit BufferCommands(vk::DeviceSession& session) noexcept : session_(session) {}

        RenderResult<void> setBuffer(const SetBuffer::Request&,
            Reply<SetBuffer>& context) noexcept
        {
            auto& session = session_;
            auto pending = context.defer();
            if (!pending) return lux::cxx::unexpected(pending.error());
            auto& registry = session.resources();
            auto instance = registry.getOrCreate(registry.root(), "values", TestBufferStorage::description, nullptr);
            if (!instance) return pending->fail(instance.error());
            auto& storage = *static_cast<NativeBuffer*>(registry.native(*instance));
            auto upload = lux::render::upload(storage, pending->request());
            if (!upload) return pending->fail(upload.error());
            return pending->completeWhen(vk::GpuWorkSender<BufferBinding>{
                std::move(upload->first), std::move(upload->second)
            });
        }

        RenderResult<void> removeBuffer(const RemoveBuffer::Request& input,
            Reply<RemoveBuffer>& context) noexcept
        {
            auto& session = session_;
            auto& registry = session.resources();
            auto id = registry.find(registry.root(), "values", TestBufferStorage::description);
            if (!id) return lux::cxx::unexpected(id.error());
            auto result = static_cast<NativeBuffer*>(registry.native(*id))->remove(input.id);
            return result ? context.complete(Ack{}) : result;
        }

        RenderResult<void> readBuffer(const ReadBuffer::Request&,
            Reply<ReadBuffer>& context) noexcept
        {
            auto& session = session_;
            auto pending = context.defer(sizeof(ReadBuffer::Response) + 16);
            if (!pending) return lux::cxx::unexpected(pending.error());
            const auto& input = pending->request();
            auto& registry = session.resources();
            auto id = registry.find(registry.root(), "values", TestBufferStorage::description);
            if (!id) return pending->fail(id.error());
            struct Read final
            {
                vk::DeviceSession& session;
                StorageId storage;
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

                vk::SubmissionQueue& queue() noexcept { return session.queue(); }

                RenderResult<vk::SubmissionTicket> submit() noexcept
                {
                    auto commands = queue().begin();
                    if (!commands) return lux::cxx::unexpected(commands.error());
                    VkBufferMemoryBarrier2 before{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                    before.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                    before.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                    before.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                    before.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                    before.srcQueueFamilyIndex = before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    before.buffer = source.buffer->native();
                    before.size = VK_WHOLE_SIZE;
                    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                    dependency.bufferMemoryBarrierCount = 1;
                    dependency.pBufferMemoryBarriers = &before;
                    vkCmdPipelineBarrier2(commands->native(), &dependency);
                    const VkBufferCopy region{0, 0, 16};
                    vkCmdCopyBuffer(commands->native(), source.buffer->native(), session.borrow(buffer)->native(), 1, &region);
                    before.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                    before.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    before.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                    before.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                    before.buffer = session.borrow(buffer)->native();
                    vkCmdPipelineBarrier2(commands->native(), &dependency);
                    auto submitted = std::move(*commands).submit();
                    if (!submitted) return lux::cxx::unexpected(submitted.error());
                    ticket = *submitted;
                    source.last_use->merge(*submitted);
                    return *submitted;
                }

                RenderResult<void> ready() noexcept { return session.read(*session.borrow(buffer), 0, *output); }
            };
            auto output = std::make_shared<std::vector<std::byte>>(16);
            auto payload = RenderPayload::fromVector(output, 0, output->size());
            if (!payload) return pending->fail(payload.error());
            ReadBuffer::Response response{std::move(*payload)};
            registry.retain(*id);
            auto owner = std::make_unique<Read>(session, *id, vk::PreparedStorageBinding{}, vk::BufferPermit{},
                std::optional<vk::SubmissionTicket>{}, output);
            vk::NativeResourcePlan plan{"values", GraphBufferDesc{16}, EGraphOrigin::IMPORTED};
            auto argument = encodeData(input, {}, {8, 0});
            if (!argument) return pending->fail(argument.error());
            auto prepared = static_cast<NativeBuffer*>(registry.native(*id))->prepare(plan, argument->view(), owner->source);
            if (!prepared) return pending->fail(prepared.error());
            auto buffer = session.createBuffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, vk::EMemoryAccess::READBACK);
            if (!buffer) return pending->fail(buffer.error());
            owner->buffer = std::move(*buffer);
            return pending->completeWhen(native_test::SubmissionSender<ReadBuffer::Response, Read>{
                std::move(response), std::move(owner)
            });
        }

        using Bindings = std::tuple<CommandMethod<SetBuffer, &BufferCommands::setBuffer>,
            CommandMethod<RemoveBuffer, &BufferCommands::removeBuffer>,
            CommandMethod<ReadBuffer, &BufferCommands::readBuffer>>;

    private:
        vk::DeviceSession& session_;
    };

}

namespace lux::render::vulkan
{
    template <> struct StorageProvider<extension_test::TestBufferStorage> final
    {
        using Storage = extension_test::NativeBuffer;
        using Handler = extension_test::BufferCommands;
        static StorageFactory factory() noexcept { return extension_test::bufferFactory(); }
        static StorageCppType implementationType() noexcept { return storageCppType<Storage>(); }
    };
}

namespace lux::render
{
    template <> inline FeatureRegistration nativeFeatureBinding<extension_test::TestBufferFeature>() noexcept
    {
        return {&extension_test::definition, &extension_test::definition_graph, extension_test::programs, {}};
    }
}
