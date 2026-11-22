#pragma once

#include "SceneCapture.hpp"
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>

namespace sdk_buffers
{
    struct ProductDescriptor final
    {
        using Config = void;
        using Ops = void;
        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.sdk.final_product"}, 0, 0, {}, "Versioned final Buffer.", EResourceDomain::VIEW
        };
    };

    struct ProductBuffer final
    {
        using Descriptor = ProductDescriptor;
        static constexpr std::uint32_t bytes = 32;
        static inline ProductBuffer* live = nullptr;
        static inline std::atomic_bool gate_next_reader{};
        static inline std::atomic_bool swap_candidate_slots{};
        using Submit = decltype(nri::CoreInterface::QueueSubmit);
        static inline Submit submit = nullptr;
        static inline std::atomic_uint countdown{}, accepted_prefix{};
        static inline std::atomic<nri::Result> failure{nri::Result::INVALID_ARGUMENT};
        DeviceSession& device;
        ResourceRegistry& registry;
        ResourceInstanceId id;
        std::vector<nri::Buffer*> buffers;
        nri::Fence* gate = nullptr;

        ProductBuffer(ResourceCreateContext& context, ResourceInstanceId identity) noexcept
            : device(context.device), registry(context.resources), id(identity), buffers(context.frame_count)
        {
            const auto& core = device.nriCore();
            const nri::BufferDesc desc{bytes, 0, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE};
            for (auto& buffer : buffers)
                CHECK(core.CreatePlacedBuffer(device.device(), NriDeviceHeap, desc, buffer) == nri::Result::SUCCESS);
            // Fault injection observes one device queue table. The other product Resources own only their buffers.
            if (live) return;
            CHECK(core.CreateFence(device.device(), {}, gate) == nri::Result::SUCCESS);
            live = this;
            auto& hooks = const_cast<nri::CoreInterface&>(core);
            submit = hooks.QueueSubmit;
            hooks.QueueSubmit = [](nri::Queue& queue, const nri::QueueSubmitDesc& desc) noexcept
            {
                auto n = countdown.load();
                while (n && !countdown.compare_exchange_weak(n, n - 1)) {}
                if (n == 1) return failure.load();
                const auto result = submit(queue, desc);
                if (n && result == nri::Result::SUCCESS) ++accepted_prefix;
                return result;
            };
        }

        ~ProductBuffer() noexcept
        {
            if (live == this)
            {
                const_cast<nri::CoreInterface&>(device.nriCore()).QueueSubmit = submit;
                live = nullptr;
            }
            device.deferRelease([core = &device.nriCore(), owned = std::move(buffers), fence = gate]() noexcept
            {
                for (auto* buffer : owned) core->DestroyBuffer(buffer);
                if (fence) core->DestroyFence(fence);
            });
        }

        bool canDelayRead() const noexcept { return &device.queue() != &device.queue(EQueueRole::COMPUTE); }

        void delayRead() const noexcept
        {
            const nri::FenceSubmitDesc wait{gate, 1, nri::StageBits::ALL};
            const nri::QueueSubmitDesc submit{.waitFences = &wait, .waitFenceNum = 1};
            CHECK(device.nriCore().QueueSubmit(device.queue().native(), submit) == nri::Result::SUCCESS);
        }

        void releaseRead() const noexcept
        {
            // All graph work in this fixture is Graphics. Compute is a distinct idle queue dedicated to this signal.
            const nri::FenceSubmitDesc signal{gate, 1, nri::StageBits::ALL};
            const nri::QueueSubmitDesc submit{.signalFences = &signal, .signalFenceNum = 1};
            auto& queue = device.queue(EQueueRole::COMPUTE);
            CHECK(device.nriCore().QueueSubmit(queue.native(), submit) == nri::Result::SUCCESS);
        }

        static RenderResult<std::unique_ptr<ProductBuffer>>
        create(ResourceCreateContext& context, ResourceInstanceId id) noexcept
        {
            return std::make_unique<ProductBuffer>(context, id);
        }

        static ResourceRegistration factory() noexcept
        {
            static const StorageBindingOps ops{
                .describeGraphImport = [](const void*, std::uint32_t projection) noexcept
                    -> RenderResult<StorageGraphImport>
                {
                    if (projection) return lux::cxx::unexpected(RenderError{kStorageContract});
                    return StorageGraphImport::bufferOutput(bytes, EGraphUse::TRANSFER_SOURCE);
                },
                .resolveGraphImport = [](
                    void* value, std::uint32_t, const VGraphResourceDesc&, std::span<ResourceInput> out
                ) noexcept -> RenderResult<void>
                {
                    auto& owner = *static_cast<ProductBuffer*>(value);
                    const auto& buffers = owner.buffers;
                    CHECK(out.size() == buffers.size());
                    for (std::size_t i = 0; i < out.size(); ++i) out[i] = {buffers[i], 0, bytes};
                    if (swap_candidate_slots.exchange(false))
                    {
                        CHECK(out.size() == 2);
                        // A dormant Buffer receipt still identifies the projection's resource kind.
                        const GraphImagePublication invalid{0, UINT32_MAX, nullptr};
                        auto withdrawn = owner.registry.publishGraphImages(owner.id, {&invalid, 1});
                        CHECK(!withdrawn && withdrawn.error().type == kStorageMismatch);
                        std::swap(out[0], out[1]);
                    }
                    return {};
                }
            };
            return makeResourceRegistration<Descriptor, ProductBuffer, &create>(nullptr, &ops);
        }
    };

    inline JobSubmitted reuseAfterReader(ipc::IpcRequester& client, const CommandList& commands, RenderJobId writer)
    {
        auto& product = *ProductBuffer::live;
        if (!product.canDelayRead()) return call<RequestRenderJob>(client, commands, {writer});
        const auto command = std::ranges::find(commands.commands, RequestRenderJob::name, &CommandInfo::name);
        CHECK(command != commands.commands.end());
        const auto request = nextSceneRequest();
        ipc::IpcMessage message;
        message.emplace<RequestRenderJob::Request>(RequestRenderJob::Request{writer});
        CHECK(client.trySend(commandTag(command->id), request, message));
        // The same IPC requester orders admission. The first request owns the credit while the GPU reader waits.
        CHECK(call<RequestRenderJob>(client, commands, {writer}, false).error.type == kJobBusy);
        auto& notifications = ipc::IpcCore::instance();
        const auto held_until = Clock::now() + 150ms;
        while (Clock::now() < held_until)
        {
            const auto observed = notifications.workEpoch();
            std::uint64_t correlation = 0;
            const auto status = client.tryReceive(correlation, message);
            CHECK(status != ipc::EIpcReceive::RECEIVED && status != ipc::EIpcReceive::CLOSED);
            notifications.waitUntil(observed, held_until);
        }
        CHECK(product.device.nriCore().GetFenceValue(*product.gate) == 0);
        product.releaseRead();
        const auto deadline = Clock::now() + 30s;
        for (;;)
        {
            const auto observed = notifications.workEpoch();
            std::uint64_t correlation = 0;
            const auto status = client.tryReceive(correlation, message);
            if (status == ipc::EIpcReceive::RECEIVED)
            {
                CHECK(correlation == request);
                const auto result = message.get<JobSubmitted>();
                CHECK(!result.error.type);
                return result;
            }
            CHECK(status != ipc::EIpcReceive::CLOSED && Clock::now() < deadline);
            notifications.waitUntil(observed, deadline);
        }
    }
}
