#include "GraphFill.hpp"
#include "SceneCapture.hpp"
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/extensions/effect/EffectBuildContext.hpp>
#include <lux/engine/render/extensions/effect/EffectImplementation.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <cstring>

namespace
{
    namespace graph = lux::graph;

    // One 64-texel RGBA8 row matches the shared capture fixture's minimum pitch.
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 32;
    constexpr std::uint32_t row_pitch = width * 4;
    constexpr std::uint32_t readback_bytes = row_pitch * height;
    constexpr std::uint32_t capture_bytes = readback_bytes + sizeof(std::uint32_t);
    constexpr auto color = effectPortSemantic("color");
    constexpr auto pixels = effectPortSemantic("pixels");
    constexpr std::string_view compute_node = "test.sdk.compute_color";
    constexpr std::string_view capture_node = "test.sdk.compute_capture";

    struct ComputeCaptureDescriptor final
    {
        using Config = void;
        using Ops = void;
        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.sdk.compute_readback"}, 0, 0, {}, "Compute output readback.", EResourceDomain::VIEW
        };
    };

    struct ComputeCapture final
    {
        using Descriptor = ComputeCaptureDescriptor;
        static inline CapturedPixels result;
        static inline std::atomic_uint accepted{}, rejected{};
        static constexpr std::uint64_t preparation_revision = 1;
        CaptureBuffer buffer;
        std::uint64_t epoch = 0;

        explicit ComputeCapture(ResourceCreateContext& context) noexcept : buffer(context) {}

        static ResourceRegistration factory() noexcept
        {
            static const StorageBindingOps binding{
                .prepareFrame = [](void* owner, ReplicaFrame frame, ResourceUploader&) noexcept
                    -> RenderResult<bool>
                {
                    auto& storage = *static_cast<ComputeCapture*>(owner);
                    CHECK(frame.slot == 0 && frame.epoch && storage.epoch == 0);
                    storage.epoch = frame.epoch;
                    return true;
                },
                .preparationRevision = [](const void*) noexcept { return &preparation_revision; },
                .describeGraphImport = [](const void*, std::uint32_t projection) noexcept
                    -> RenderResult<StorageGraphImport>
                {
                    if (projection != 0) return lux::cxx::unexpected(RenderError{kStorageContract});
                    return StorageGraphImport{GraphBufferDesc{capture_bytes}, {}, EGraphUse::TRANSFER_DESTINATION};
                },
                .resolveGraphImport = [](
                    void* storage,
                    std::uint32_t,
                    const VGraphResourceDesc& description,
                    StorageSubscription& subscription,
                    std::span<ResourceInput> output
                ) noexcept
                {
                    const CaptureConfig config{.result = &result, .bytes = capture_bytes};
                    return CaptureBuffer::resolve(
                        &static_cast<ComputeCapture*>(storage)->buffer, description, &config, subscription, output
                    );
                },
                .settleFrame = [](void* owner, const ReplicaSettlement& receipt) noexcept
                {
                    auto& storage = *static_cast<ComputeCapture*>(owner);
                    CHECK(storage.epoch && storage.epoch == receipt.frame.epoch);
                    CHECK(!receipt.copies.count && !receipt.accepted_copy.serial);
                    storage.epoch = 0;
                    if (receipt.error.type) ++rejected;
                    else ++accepted;
                }
            };
            return makeResourceRegistration<Descriptor, ComputeCapture, &create>(nullptr, &binding);
        }

        static RenderResult<std::unique_ptr<ComputeCapture>>
        create(ResourceCreateContext& context, ResourceInstanceId) noexcept
        {
            return std::make_unique<ComputeCapture>(context);
        }
    };

    struct SetReplicaValue final
    {
        static constexpr std::string_view name = "test.sdk.set_replica";
        struct Request final
        {
            ResourceInstanceId resource;
            std::uint32_t value;
            bool fail_after_staging;
        };
        using Data = Request;
        using Response = AckResponse;
        [[nodiscard]] static ResourceInstanceId resourceId(const Request& request) noexcept { return request.resource; }
    };

    struct MutableReplicaDescriptor final
    {
        using Config = void;
        using Ops = void;
        using Commands = std::tuple<SetReplicaValue>;
        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.sdk.mutable_replica"}, 0, 0, {}, "Mutable replica.", EResourceDomain::VIEW,
            resourceCommandDescriptors(static_cast<Commands*>(nullptr))
        };
    };

    struct MutableReplica final
    {
        using Descriptor = MutableReplicaDescriptor;
        static inline std::atomic_uint accepted_copies{}, accepted_no_copy{}, rejected{};
        DeviceSession& device;
        nri::Buffer* buffer = nullptr;
        std::uint64_t revision = 1, epoch = 0;
        std::uint32_t value = 0, selected = 0;
        std::optional<std::uint32_t> resident;
        bool fail_after_staging = false;

        explicit MutableReplica(DeviceSession& device) noexcept : device(device) {}

        ~MutableReplica() noexcept
        {
            CHECK(epoch == 0);
            if (buffer)
                device.deferRelease([destroy = device.nriCore().DestroyBuffer, value = buffer]() noexcept
                {
                    destroy(value);
                });
        }

        static RenderResult<std::unique_ptr<MutableReplica>>
        create(ResourceCreateContext& context, ResourceInstanceId) noexcept
        {
            if (!context.uploads || context.frame_count != 1)
                return lux::cxx::unexpected(RenderError{kDeviceUnsupported});
            auto replica = std::make_unique<MutableReplica>(context.device);
            const auto& core = context.device.nriCore();
            const nri::BufferDesc desc{sizeof(std::uint32_t), 4, nri::BufferUsageBits::NONE};
            const auto created = core.CreatePlacedBuffer(context.device.device(), NriDeviceHeap, desc, replica->buffer);
            if (created != nri::Result::SUCCESS) return lux::cxx::unexpected(RenderError{kDeviceFailure});
            return replica;
        }

        static void set(MutableReplica& resource, const SetReplicaValue::Request& request, AckResponse& reply) noexcept
        {
            resource.value = request.value;
            resource.fail_after_staging = request.fail_after_staging;
            ++resource.revision;
            reply = {};
        }

        RenderResult<bool> prepare(ReplicaFrame frame, ResourceUploader& uploads) noexcept
        {
            CHECK(frame.slot == 0 && frame.epoch && epoch == 0);
            epoch = frame.epoch;
            selected = value;
            if (resident == selected) return true;
            auto staged = uploads.stageBuffer({buffer, 0, std::as_bytes(std::span{&selected, 1})});
            if (!staged) return lux::cxx::unexpected(staged.error());
            if (fail_after_staging) return lux::cxx::unexpected(RenderError{kStorageCapacity});
            return true;
        }

        void settle(const ReplicaSettlement& receipt) noexcept
        {
            CHECK(receipt.frame.slot == 0 && receipt.frame.epoch == epoch);
            epoch = 0;
            if (receipt.error.type)
            {
                CHECK(!receipt.accepted_copy.serial && receipt.copies.count == 1);
                ++rejected;
                return;
            }
            if (receipt.copies.count)
            {
                CHECK(receipt.copies.count == 1 && receipt.accepted_copy.serial);
                ++accepted_copies;
            }
            else
            {
                CHECK(!receipt.accepted_copy.serial && resident == selected);
                ++accepted_no_copy;
            }
            resident = selected;
        }

        static ResourceRegistration factory() noexcept
        {
            static const StorageBindingOps bindings{
                .prepareFrame = [](void* owner, ReplicaFrame frame, ResourceUploader& uploads) noexcept
                { return static_cast<MutableReplica*>(owner)->prepare(frame, uploads); },
                .preparationRevision = [](const void* owner) noexcept
                { return &static_cast<const MutableReplica*>(owner)->revision; },
                .describeGraphImport = [](const void*, std::uint32_t projection) noexcept
                    -> RenderResult<StorageGraphImport>
                {
                    if (projection != 0) return lux::cxx::unexpected(RenderError{kStorageContract});
                    return StorageGraphImport{
                        GraphBufferDesc{sizeof(std::uint32_t)}, {GraphBufferRange{0, sizeof(std::uint32_t)}},
                        EGraphUse::TRANSFER_SOURCE
                    };
                },
                .resolveGraphImport = [](
                    void* owner, std::uint32_t, const VGraphResourceDesc&, StorageSubscription&,
                    std::span<ResourceInput> output
                ) noexcept -> RenderResult<void>
                {
                    const auto* resource = static_cast<MutableReplica*>(owner);
                    for (auto& value : output) value = {resource->buffer, 0, sizeof(std::uint32_t)};
                    return {};
                },
                .settleFrame = [](void* owner, const ReplicaSettlement& receipt) noexcept
                { static_cast<MutableReplica*>(owner)->settle(receipt); }
            };
            static const std::array commands{CommandBinding::declare<SetReplicaValue, &set>()};
            return makeResourceRegistration<Descriptor, MutableReplica, &create>(nullptr, &bindings, commands);
        }
    };

    CapabilityDefinition computeDefinition() noexcept
    {
        return {
            .identity = {graph::nodeTypeId(compute_node), std::string{compute_node}, 1},
            .ports = {{color, "color", {"test.sdk.compute_rgba8", 1}, graph::EPinDirection::OUTPUT}}
        };
    }

    CapabilityDefinition captureDefinition() noexcept
    {
        return {
            .identity = {graph::nodeTypeId(capture_node), std::string{capture_node}, 1},
            .ports = {
                {color, "color", {"test.sdk.compute_rgba8", 1}, graph::EPinDirection::INPUT, 1, true},
                {pixels, "pixels", {"test.sdk.readback_bytes", 1}, graph::EPinDirection::OUTPUT}
            }
        };
    }

    RenderResult<void> buildCompute(EffectBuildContext& context, std::string_view) noexcept
    {
        GraphImageDesc image{width, height};
        image.extent.source = EImageExtentSource::TARGET;
        auto& builder = context.builder();
        const auto generated = take(builder.declareResource({"compute storage", image}));
        const auto output = take(builder.declareResource({"compute output", image}));
        ComputeWork work;
        work.interface.program = take(builder.addProgram(GraphProgram{kGraphFill.load(context.shaderTarget())}));
        work.interface.resources = {{"generated", {{generated, 0}, {}, EGraphAccess::WRITE}}};
        work.dispatch = ImageDispatchArguments{generated};
        const auto dispatch = take(builder.addComputePass("generate compute color", std::move(work)));
        const auto copied = take(builder.addTransferPass(
            "copy compute color", TransferWork{{CopyImage{{dispatch.written.front()}, {{output, 0}}}}}
        ));
        return context.publish(color, {{color, copied.written.front()}});
    }

    RenderResult<void> buildCapture(EffectBuildContext& context, std::string_view) noexcept
    {
        const auto input = context.inputs(color);
        CHECK(input.size() == 1 && input.front().size() == 1 && input.front().front().member == color);
        const auto output = take(context.importResource(
            "capture", {ResourceName{ComputeCaptureDescriptor::descriptor.type.name()}},
            {"", GraphBufferDesc{capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION}
        ));
        const ImageCopyRegion region{0, 0, width, height};
        const auto copied = take(context.builder().addTransferPass("copy to readback", TransferWork{{
            CopyImageBuffer{{input.front().front().version}, {output}, row_pitch, readback_bytes, region}
        }}));
        const auto mutable_input = take(context.importResource(
            "mutable", {ResourceName{MutableReplicaDescriptor::descriptor.type.name()}},
            {
                "", GraphBufferDesc{sizeof(std::uint32_t)}, EGraphOrigin::IMPORTED,
                {GraphBufferRange{0, sizeof(std::uint32_t)}}, EGraphUse::TRANSFER_SOURCE
            }
        ));
        const auto complete = take(context.builder().addTransferPass("copy mutable replica", TransferWork{{
            CopyBuffer{{mutable_input}, {copied.written.front(), GraphBufferRange{readback_bytes, sizeof(std::uint32_t)}}}
        }}));
        return context.publish(pixels, {{pixels, complete.written.front()}});
    }

    void registerNode(
        RenderServer& server, const CapabilityDefinition& capability, graph::PinSemanticId output,
        EffectImplementation::Build build
    )
    {
        CHECK(server.registerEffectCapability(capability));
        EffectImplementation implementation;
        implementation.identity = {capability.identity.canonical_name, capability.identity.version};
        implementation.capability = capability.identity.id;
        implementation.supported_outputs = {output};
        implementation.initialize = [](EffectInitializationContext&, std::string_view arguments) noexcept
            -> RenderResult<void>
        {
            if (!arguments.empty()) return lux::cxx::unexpected(RenderError{kInvalidConfig});
            return {};
        };
        implementation.build = build;
        CHECK(server.registerEffectImplementation(std::move(implementation)));
    }
}

int main(int argc, char** argv)
{
    const bool d3d12 = argc > 1 && std::string_view{argv[1]} == "d3d12";
    auto execution = lux::execution::ExecutionRuntime::create({2, 128, {128}});
    CHECK(execution);
    RenderServerConfig config{"lux.render.sdk.compute_output", nativeValidationEnabled()};
    config.frames_in_flight = 1;
    config.uploads.emplace();
    DeviceOptions options;
    if (d3d12) options.backend = EGraphicsBackend::D3D12;
    auto server = take(RenderServer::create(config, **execution));
    registerNode(*server, computeDefinition(), color, buildCompute);
    registerNode(*server, captureDefinition(), pixels, buildCapture);
    take(server->registerResource<ComputeCapture>());
    take(server->registerResource<MutableReplica>());
    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread worker([&] { CHECK(server->run()); });
    const auto commands = queryCommands(*client);
    const auto session = call<CreateDeviceSession>(*client, commands, options).device;
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto view = call<CreateView>(*client, commands, {scene}).view;
    const auto format = rdesc::ETextureFormat::RGBA8_UNORM;
    const auto target = call<CreateTarget>(*client, commands, {session, {width, height, format}}).target;
    const auto readback = call<GetOrCreateResource>(
        *client, commands, {session, ViewRequest{scene, view}, ComputeCaptureDescriptor::descriptor.type}
    ).resource_id;
    const auto mutable_resource = call<GetOrCreateResource>(
        *client, commands, {session, ViewRequest{scene, view}, MutableReplicaDescriptor::descriptor.type}
    ).resource_id;
    CreateEffectRequest effect;
    effect.graph = std::make_unique<EffectGraph>();
    EffectGraphEdit edit{*effect.graph};
    const auto compute = take(edit.addNode(computeDefinition(), {std::string{compute_node}, 1}));
    const auto capture = take(edit.addNode(captureDefinition(), {std::string{capture_node}, 1}));
    const auto color_pin = edit.pinId(compute, color);
    CHECK(edit.connect(color_pin, edit.pinId(capture, color)));
    CHECK(edit.setOutput("color", color_pin, true));
    CHECK(edit.setOutput("pixels", edit.pinId(capture, pixels)));
    CHECK(edit.commit());
    const auto created = call<CreateEffect>(*client, commands, std::move(effect)).effect;
    JobBinding binding;
    binding.resources = {{"capture", readback}, {"mutable", mutable_resource}};
    const auto job = startRenderJob(*client, commands, session, scene, view, target, created, std::move(binding));
    constexpr std::uint32_t expected_replica = 0x12345678;
    call<SetReplicaValue>(*client, commands, {mutable_resource, expected_replica, true});
    const auto failed = call<RequestRenderJob>(*client, commands, {job}, false);
    CHECK(failed.error.type == kStorageCapacity && MutableReplica::rejected == 1);
    CHECK(MutableReplica::accepted_copies == 0);
    CHECK(ComputeCapture::rejected == 1 && ComputeCapture::accepted == 0);
    call<SetReplicaValue>(*client, commands, {mutable_resource, expected_replica, false});
    renderFrame(*client, commands, job);
    CHECK(MutableReplica::accepted_copies == 1);
    call<SetReplicaValue>(*client, commands, {mutable_resource, expected_replica, false});
    renderFrame(*client, commands, job);
    CHECK(MutableReplica::accepted_no_copy == 1);
    renderFrame(*client, commands, job);
    CHECK(MutableReplica::accepted_no_copy == 1 && MutableReplica::accepted_copies == 1);
    CHECK(ComputeCapture::rejected == 1 && ComputeCapture::accepted == 1);
    server->stop();
    worker.join();
    CHECK(server->stats().validation_errors == 0);
    const auto& readback_pixels = ComputeCapture::result;
    CHECK(readback_pixels.ready == 1 && readback_pixels.frames.size() == 1);
    constexpr std::size_t pixel_size = 4;
    const auto data = std::span(readback_pixels.frames.front());
    CHECK(data.size() >= capture_bytes);
    std::uint32_t replica = 0;
    std::memcpy(&replica, data.data() + readback_bytes, sizeof(replica));
    CHECK(replica == expected_replica);
    std::puts("SDK MUTABLE REPLICA PASS: staged rejection, retry, zero-copy receipt and GPU readback");
    for (std::size_t y = 0; y < height; ++y)
    {
        for (std::size_t x = 0; x < width; ++x)
        {
            const auto pixel = data.subspan(y * readback_pixels.pitch + x * pixel_size, pixel_size);
            // The shader writes exactly 0.5, halfway between the two nearest UNORM8 values.
            const bool green_matches = pixel[1] == std::byte{127} || pixel[1] == std::byte{128};
            const bool other_channels_match = pixel[0] == std::byte{64} && pixel[2] == std::byte{191} &&
                pixel[3] == std::byte{255};
            if (!green_matches || !other_channels_match)
            {
                std::fprintf(stderr, "Pixel (%zu, %zu) at pitch %u: %u %u %u %u\n", x, y, readback_pixels.pitch,
                    std::to_integer<unsigned>(pixel[0]), std::to_integer<unsigned>(pixel[1]),
                    std::to_integer<unsigned>(pixel[2]), std::to_integer<unsigned>(pixel[3]));
                CHECK(false);
            }
        }
    }
    std::printf("SDK COMPUTE OUTPUT PASS: %s produced %ux%u expected pixels through an external Resource\n",
        d3d12 ? "D3D12" : "Vulkan", width, height);
}
