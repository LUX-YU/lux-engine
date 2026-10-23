#pragma once

#include "Support.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/execution/FrameExecution.hpp>
#include <lux/engine/render/features/FeatureAssembly.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/runtime/FeatureRegistration.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
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
    using Descriptor = CaptureDescriptor;

    static StorageFactory factory() noexcept
    {
        static const StorageBindingOps bindings{resolve};
        return makeStorageFactory<
            CaptureDescriptor,
            CaptureBuffer,
            StorageCreateContext,
            &CaptureBuffer::create,
            &CaptureBuffer::compatible>(nullptr, &bindings);
    }

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
        const VGraphResourceDesc&,
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
    using Descriptor = CaptureFeature;
    using Storages = std::tuple<CaptureBuffer>;
    using Commands = std::tuple<>;
    static const FeatureDescriptor descriptor;
    static const FeatureGraphOps& graph(bool) noexcept;

    static RenderResult<void> describe(const void* value, std::string_view key, FeaturePorts& ports) noexcept
    {
        GraphImageDesc image{64, 64};
        const auto& config = *static_cast<const CaptureConfig*>(value);
        image.format = config.format;
        image.layers = config.layers;
        image.extent.source = config.target_relative ? EImageExtentSource::TARGET : EImageExtentSource::FIXED;
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

inline const FeatureDescriptor CaptureFeature::descriptor{
    FeatureName{"test.deferred.Copy"},
    lux::cxx::typeToken<CaptureConfig>(),
    "Terminal test capture",
    CaptureFeature::describe
};

inline const FeatureGraphOps& CaptureFeature::graph(bool) noexcept
{
    static const FeatureGraphOps value{CaptureFeature::build};
    return value;
}

// Fixture-owned correlation across its sequential calls; production Clients own their own counters.
static std::uint64_t nextSceneRequest() noexcept
{
    static std::atomic<std::uint64_t> sequence{1};
    return sequence.fetch_add(1, std::memory_order_relaxed);
}

template <class C>
static typename C::Response call(
    ipc::IpcRequester& client,
    const CommandList& catalog,
    typename C::Request request,
    bool success = true
)
{
    const auto found = std::ranges::find(catalog.commands, C::name, &CommandInfo::name);
    CHECK(found != catalog.commands.end());
    const auto request_id = nextSceneRequest();
    ipc::IpcMessage message;
    FrameCommand command{found->id, {request_id}, {}};
    const FrameCommandGroup group{std::span(&command, 1)};
    std::uint64_t frame_id = 0;
    if constexpr (C::frame_scoped)
    {
        command.request.emplace<typename C::Request>(std::move(request));
        const auto frame = std::ranges::find(catalog.commands, SubmitFrame::name, &CommandInfo::name);
        CHECK(frame != catalog.commands.end());
        frame_id = nextSceneRequest();
        message.emplace<FrameSubmission>(FrameSubmission{frame_id, std::span(&group, 1)});
        CHECK(client.trySend(commandTag(frame->id), frame_id, message));
    }
    else
    {
        message.emplace<typename C::Request>(std::move(request));
        CHECK(client.trySend(commandTag(found->id), request_id, message));
    }
    typename C::Response response{};
    bool received_response = false;
    bool received_frame = !C::frame_scoped;
    const auto deadline = Clock::now() + 30s;
    auto& notifications = ipc::IpcCore::instance();
    while (!received_response || !received_frame)
    {
        const auto observed = notifications.workEpoch();
        std::uint64_t correlation = 0;
        const auto status = client.tryReceive(correlation, message);
        CHECK(status != ipc::EIpcReceive::CLOSED && Clock::now() < deadline);
        if (status != ipc::EIpcReceive::RECEIVED)
        {
            notifications.waitUntil(observed, deadline);
            continue;
        }
        if (correlation == request_id)
        {
            response = std::move(message.get<typename C::Response>());
            received_response = true;
        }
        else if constexpr (std::same_as<C, SubmitFrame>)
        {
            // This fixture call only needs the aggregate outcome. Each child's error is reflected
            // there; callers needing a child value use the typed frame-scoped call above.
        }
        else
        {
            CHECK(C::frame_scoped && correlation == frame_id);
            const auto& frame = message.get<FrameCompletion>();
            if (frame.error.type != 0 && (response.error.type == 0 || !received_response))
            {
                response.error = frame.error;
            }
            if (frame.outcome == EFrameOutcome::REJECTED)
            {
                received_response = true;
            }
            received_frame = true;
        }
        message.reset();
    }
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

static void renderFrame(ipc::IpcRequester& client, const CommandList& commands)
{
    const auto response = call<SubmitFrame>(client, commands, {nextSceneRequest(), {}});
    CHECK(response.outcome == EFrameOutcome::SUBMITTED);
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
