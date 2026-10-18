#include <lux/engine/render/runtime/CommandBinding.hpp>
#pragma once

#include "../../native/vulkan/test/extension/NativeBuffer.hpp"
#include "../../native/vulkan/test/extension/Feature.hpp"
#include "../../native/vulkan/test/SubmissionSender.hpp"
#include <lux/engine/render/features/Registration.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>

namespace extension_test
{
    class BufferCommands final
    {
    public:
        static constexpr std::string_view name = "test.extension.BufferCommands";
        inline static unsigned constructed = 0;
        inline static unsigned destroyed = 0;
        explicit BufferCommands(vk::DeviceSession& session) noexcept : session_(session) { ++constructed; }
        ~BufferCommands() noexcept { ++destroyed; }

            struct Read final
            {
                vk::DeviceSession& session;
                StorageLease storage;
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
                    storage.reset();
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
        vk::GpuWorkSender<SetBuffer::Response> setBuffer(SetBuffer::Request&& input) noexcept
        {
            auto& session = session_;
            auto& registry = session.resources();
            auto storage = registry.borrow<NativeBuffer>(input.id.storage);
            if (!storage) return vk::GpuWorkSender<SetBuffer::Response>{storage.error()};
            auto upload = lux::render::upload(**storage, input);
            if (!upload) return vk::GpuWorkSender<SetBuffer::Response>{upload.error()};
            return {std::move(upload->first), std::move(upload->second), std::move(input.source)};
        }

        void removeBuffer(const RemoveBuffer::Request& input, RemoveBuffer::Response& response) noexcept
        {
            auto& session = session_;
            auto& registry = session.resources();
            auto storage = registry.borrow<NativeBuffer>(input.id.storage);
            if (!storage) { response = lux::cxx::unexpected(storage.error()); return; }
            auto result = (*storage)->remove(input.id);
            if (!result) response = lux::cxx::unexpected(result.error());
            else response = Ack{};
        }

        native_test::SubmissionSender<ReadBuffer::Response, Read> readBuffer(ReadBuffer::Request&& input) noexcept
        {
            auto& session = session_;
            auto& registry = session.resources();
            auto storage = registry.borrow<NativeBuffer>(input.id.storage);
            if (!storage) return native_test::SubmissionSender<ReadBuffer::Response, Read>{storage.error()};
            auto output = std::make_shared<std::vector<std::byte>>(16);
            auto payload = RenderPayload::fromVector(output, 0, output->size());
            if (!payload) return native_test::SubmissionSender<ReadBuffer::Response, Read>{payload.error()};
            ReadBuffer::Response response{ReadBuffer::ResponseValue{std::move(*payload)}};
            auto lease = registry.lease(input.id.storage);
            if (!lease) return native_test::SubmissionSender<ReadBuffer::Response, Read>{lease.error()};
            auto owner = std::make_unique<Read>(session, std::move(*lease), vk::PreparedStorageBinding{}, vk::BufferPermit{},
                std::optional<vk::SubmissionTicket>{}, output);
            vk::NativeResourcePlan plan{"values", GraphBufferDesc{16}, EGraphOrigin::IMPORTED};
            auto prepared = (*storage)->prepare(plan, input, owner->source);
            if (!prepared) return native_test::SubmissionSender<ReadBuffer::Response, Read>{prepared.error()};
            auto buffer = session.createBuffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, vk::EMemoryAccess::READBACK);
            if (!buffer) return native_test::SubmissionSender<ReadBuffer::Response, Read>{buffer.error()};
            owner->buffer = std::move(*buffer);
            return {std::move(response), std::move(owner)};
        }

        using Bindings = std::tuple<CommandMethod<SetBuffer, &BufferCommands::setBuffer>,
            CommandMethod<RemoveBuffer, &BufferCommands::removeBuffer>,
            CommandMethod<ReadBuffer, &BufferCommands::readBuffer>>;

    private:
        vk::DeviceSession& session_;
    };

}

namespace lux::render
{
    template <> struct StorageProvider<extension_test::TestBufferStorage> final
    {
        using Storage = extension_test::NativeBuffer;
        using Handler = extension_test::BufferCommands;
        static std::unique_ptr<Handler> createHandler(vulkan::DeviceSession& device) noexcept
        { return std::make_unique<Handler>(device); }
        static StorageFactory factory() noexcept { return extension_test::bufferFactory(); }
        static CppType implementationType() noexcept { return cppType<Storage>(); }
    };
}

namespace lux::render
{
    template <> struct FeatureProvider<extension_test::TestBufferFeature> final
    {
        using Handler = extension_test::BufferCommands;
        static std::unique_ptr<Handler> createHandler(vulkan::DeviceSession& device) noexcept
        { return std::make_unique<Handler>(device); }
        feature_detail::Implementation definition() const noexcept
        {
            return {&extension_test::definition, &extension_test::definition_graph, extension_test::programs};
        }
    };
}
