#pragma once

#include "Support.hpp"
#include <chrono>
#include <cstring>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/execution/FrameExecution.hpp>
#include <lux/engine/render/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/FeatureDefinition.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/runtime/Registration.hpp>
#include <thread>

using namespace foundation_test;
namespace ipc = lux::platform::ipc;
namespace rdesc = lux::rdesc;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// Test extension: ordinary graph copies, mapped only after Detach and actual GPU cleanup.
// No production Readback command, implicit Draw, Native accessor on Client, or timing-based completion.
struct CapturedPixels
{
    std::vector<std::vector<std::byte>> frames;
    std::uint32_t pitch = 0;
    std::atomic<unsigned> ready = 0;
};

struct CaptureConfig
{
    CapturedPixels* result = nullptr;
    rdesc::ETextureFormat format = rdesc::ETextureFormat::RGBA8_UNORM;
    std::uint32_t layers = 1, layer = 0, bytes = 0;
    bool target_relative = true;
    bool operator==(const CaptureConfig&) const noexcept = default;
};

struct CaptureDescriptor
{
    using Config = CaptureConfig;
    using Ops = void;
    static inline const StorageDescriptor
        descriptor{StorageNameView{"test.deferred.Capture"}, 0, 0, {}, "Test-owned terminal copy buffer."};
};

class CaptureBuffer
{
public:
    explicit CaptureBuffer(StorageCreateContext& context, CaptureConfig config) noexcept
        : device_(context.device), result_(config.result), buffers_(context.frames->frameCount())
    {
    }

    ~CaptureBuffer() noexcept
    {
        auto* device = &device_;
        device_.deferRelease(
            [device, result = result_, buffers = std::move(buffers_), pitch = pitch_, bytes = bytes_]() noexcept
            {
                result->pitch = pitch;
                for (auto* buffer : buffers)
                {
                    if (!buffer)
                    {
                        continue;
                    }
                    const auto* mapped = static_cast<const std::byte*>(device->nriCore().MapBuffer(*buffer, 0, bytes));
                    CHECK(mapped);
                    CHECK(device->invalidateMappedRange(*buffer, 0, bytes) == nri::Result::SUCCESS);
                    result->frames.emplace_back(mapped, mapped + bytes);
                    device->nriCore().UnmapBuffer(*buffer);
                    device->nriCore().DestroyBuffer(buffer);
                }
                result->ready.fetch_add(1, std::memory_order_release);
            }
        );
    }

    static RenderResult<std::unique_ptr<CaptureBuffer>> create(
        StorageCreateContext& context,
        StorageId,
        const CaptureConfig& config
    ) noexcept
    {
        auto value = std::make_unique<CaptureBuffer>(context, config);
        const auto alignment = context.device.description().memoryAlignment.uploadBufferTextureRow;
        value->pitch_ = (256 + alignment - 1) / alignment * alignment;
        value->bytes_ = std::max<std::uint64_t>(value->pitch_ * 64u, config.bytes);
        for (auto*& buffer : value->buffers_)
        {
            auto status = context.device.nriCore().CreatePlacedBuffer(
                context.device.device(),
                NriHostReadbackHeap,
                {value->bytes_, 0, nri::BufferUsageBits::NONE},
                buffer
            );
            CHECK(status == nri::Result::SUCCESS);
            // A short scene need not visit every available FIF slot. Define the unused-slot marker
            // explicitly; newly allocated host memory is not required to contain zeroes.
            auto* mapped = context.device.nriCore().MapBuffer(*buffer, 0, value->bytes_);
            CHECK(mapped);
            std::memset(mapped, 0, value->bytes_);
            CHECK(context.device.flushMappedRange(*buffer, 0, value->bytes_) == nri::Result::SUCCESS);
            context.device.nriCore().UnmapBuffer(*buffer);
        }
        return value;
    }

    static bool compatible(const CaptureBuffer& value, const CaptureConfig& config) noexcept
    {
        return value.result_ == config.result;
    }

    static RenderResult<void> resolve(
        void* object,
        const CompiledResource&,
        const void*,
        StorageSubscription&,
        std::span<ResourceInput> output
    ) noexcept
    {
        auto& value = *static_cast<CaptureBuffer*>(object);
        for (std::size_t i = 0; i < output.size(); ++i)
        {
            output[i] = {value.buffers_[i], 0, value.bytes_};
        }
        return {};
    }

private:
    DeviceSession& device_;
    CapturedPixels* result_;
    std::vector<nri::Buffer*> buffers_;
    std::uint32_t pitch_ = 0;
    std::uint64_t bytes_ = 0;
};

struct CaptureFeature
{
    static RenderResult<void> describe(const void* value, std::string_view key, FeaturePorts& ports) noexcept
    {
        GraphImageDesc image{64, 64};
        const auto& config = *static_cast<const CaptureConfig*>(value);
        image.format = config.format;
        image.layers = config.layers;
        image.extent.target_relative = config.target_relative;
        ports.resources.push_back(
            {"color",
             EFeaturePort::INPUT,
             config.bytes ? VGraphResourceDesc{GraphBufferDesc{config.bytes}} : VGraphResourceDesc{image}}
        );
        ports.storage.attachment.push_back(
            {"pixels",
             EFeaturePort::OUTPUT,
             GraphBufferDesc{std::max(64u * 256, config.bytes)},
             EGraphOrigin::IMPORTED,
             {},
             StorageRequirement{
                 StorageName(CaptureDescriptor::descriptor.type.name()),
                 "capture:" + std::string(key),
                 take(ConfigValue::make(*static_cast<const CaptureConfig*>(value)))
             }}
        );
        return {};
    }

    static RenderResult<void> build(const void* value, std::string_view key, FeatureBuildContext& context) noexcept
    {
        auto initialized = context.graph.addTransferPass(
            featureKey(FeatureName{"test.deferred.Copy"}, key, "initialize"),
            TransferWork{{FillBuffer{{context.resource("pixels")}, 0}}}
        );
        if (!initialized)
        {
            return lux::cxx::unexpected(initialized.error());
        }
        const auto& config = *static_cast<const CaptureConfig*>(value);
        TransferWork copy;
        if (config.bytes)
        {
            copy.operations.push_back(CopyBuffer{
                {context.resource("color")},
                {initialized->written.front(), GraphBufferRange{0, config.bytes}, EGraphAccess::READ_WRITE}
            });
        }
        else
        {
            const auto aspects = rdesc::textureAspectMask(config.format);
            copy.operations.push_back(CopyImageBuffer{
                {context.resource("color"), GraphImageRange{aspects, 0, 1, config.layer, 1}},
                {initialized->written.front(), {}, EGraphAccess::READ_WRITE}
            });
        }
        auto pass =
            context.graph.addTransferPass(featureKey(FeatureName{"test.deferred.Copy"}, key, "copy"), std::move(copy));
        if (!pass)
        {
            return lux::cxx::unexpected(pass.error());
        }
        return context.publish("pixels", pass->written.front());
    }
};

namespace lux::render
{
    template <>
    RenderResult<void> RenderServer::prepareFeature<CaptureFeature>(RegistrationBatch& batch, std::string_view) noexcept
    {
        static const StorageBindingOps bindings{CaptureBuffer::resolve};
        batch.addStorage(
            makeStorageFactory<
                CaptureDescriptor,
                CaptureBuffer,
                StorageCreateContext,
                &CaptureBuffer::create,
                &CaptureBuffer::compatible>(nullptr, &bindings),
            {},
            true
        );
        static const FeatureDescriptor descriptor{
            FeatureName{"test.deferred.Copy"},
            cppType<CaptureConfig>(),
            "Terminal test capture",
            CaptureFeature::describe
        };
        static const FeatureGraphOps graph{CaptureFeature::build};
        batch.addFeature({&descriptor, &graph});
        return {};
    }
} // namespace lux::render

template <class C>
static typename C::Response call(
    ipc::IpcRequester& client,
    const CommandList& catalog,
    typename C::Request request,
    bool success = true
)
{
    auto found = std::ranges::find(catalog.commands, C::name, &CommandInfo::name);
    CHECK(found != catalog.commands.end());
    ipc::IpcMessage message;
    message.emplace<typename C::Request>(std::move(request));
    CHECK(client.trySend(commandTag(found->id), 1, message));
    std::uint64_t correlation = 0;
    const auto deadline = Clock::now() + 30s;
    while (client.tryReceive(correlation, message) != ipc::EIpcReceive::RECEIVED)
    {
        CHECK(Clock::now() < deadline);
        std::this_thread::yield();
    }
    CHECK(correlation == 1);
    auto response = std::move(message.get<typename C::Response>());
    if (success && response.error.type)
    {
        std::fprintf(
            stderr,
            "%.*s error=%llu args=%llu,%llu,%llu\n",
            int(C::name.size()),
            C::name.data(),
            response.error.type,
            response.error.args[0],
            response.error.args[1],
            response.error.args[2]
        );
        CHECK(false);
    }
    return response;
}

static void detach(ipc::IpcRequester& client, const CommandList& commands, AttachmentId attachment)
{
    const auto deadline = Clock::now() + 20s;
    for (;;)
    {
        const auto response = call<Detach>(client, commands, attachment, false);
        if (!response.error.type)
        {
            return;
        }
        // A graph can be between accepted batches. Retry admission, never bypass its lifetime boundary.
        CHECK(response.error.type == kDeviceBusy && Clock::now() < deadline);
        std::this_thread::yield();
    }
}

static StorageId prepare(
    ipc::IpcRequester& client,
    const CommandList& commands,
    StorageNameView type,
    std::string key,
    VStorageLocation location = GlobalStorageLocation{}
)
{
    auto types = call<ListStorages>(client, commands, {});
    auto found = std::ranges::find(types.storages, type.name(), &StorageInfo::name);
    CHECK(found != types.storages.end());
    return call<PrepareStorage>(client, commands, {{location, found->id, key}, {}}).storage;
}
