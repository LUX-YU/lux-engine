#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/runtime/commands/CommandBinding.hpp>
#include <lux/engine/render/runtime/commands/ViewCommands.hpp>
#include <lux/engine/render/frame/FrameError.hpp>
#include "GraphFill.hpp"
#include "ParticleColor.hpp"
#include "ViewTint.hpp"
#include "SdkSampleTarget.hpp"
#include "ParticleStorage.hpp"
#include "DebugDrawResource.hpp"
#include "TypedGraphFill.hpp"
#include "BufferProductFixture.hpp"
#include "RasterProgramFixture.hpp"
#include "SceneCapture.hpp"
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <lux/engine/render/effect/EffectImplementation.hpp>
#include <lux/engine/render/effect/EffectInitializationContext.hpp>
#include <lux/engine/render/device/ResourceUploader.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/resource/view/Resource.hpp>
#include <cstring>
#include <future>
#include <exec/create.hpp>

struct OutputConstants final
{
    float red;
};

template <> struct lux::render::ConstantBlockTraits<OutputConstants> final
{
    static ConstantBlockLayout layout(rdesc::EShaderTarget target) noexcept
    {
        static constexpr std::array members{ConstantMemberLayout{"red", 0, rdesc::EDataScalar::FLOAT}};
        return {sizeof(OutputConstants), target == rdesc::EShaderTarget::DXIL ? 16u : 4u, members};
    }
};

struct alignas(16) OutputChannels final
{
    std::array<float, 4> channels;
};

template <> struct lux::render::ConstantBlockTraits<OutputChannels> final
{
    static ConstantBlockLayout layout(rdesc::EShaderTarget) noexcept
    {
        static constexpr std::array members{ConstantMemberLayout{"channels", 0, rdesc::EDataScalar::FLOAT, 4}};
        return {sizeof(OutputChannels), alignof(OutputChannels), members};
    }
};

namespace
{
    struct OutputState final
    {
        OutputConstants previous{0.75f}; // A wrong projection produces a different red channel.
        OutputConstants constants{0.25f};
        OutputChannels channels{{0.0f, 0.5f, 0.75f, 1.0f}};
        std::uint64_t revision = 1;
    };
    namespace graph = lux::graph;

    // One 64-texel RGBA8 row matches the shared capture fixture's minimum pitch.
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 32;
    constexpr std::uint32_t row_pitch = width * 4;
    constexpr std::uint32_t readback_bytes = row_pitch * height;
    constexpr std::uint32_t replica_bytes = 2 * sizeof(std::uint32_t);
    constexpr std::uint32_t capture_bytes = readback_bytes + replica_bytes;
    constexpr auto color = effectPortSemantic("color");
    constexpr auto pixels = effectPortSemantic("pixels");
    constexpr std::string_view compute_node = "test.sdk.compute_color";
    constexpr std::string_view capture_node = "test.sdk.compute_capture";

    struct ComputeCaptureDescriptor final
    {
        using Config = void;

        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.sdk.compute_readback"}, 0, 0, {}, "Compute output readback.", EResourceDomain::VIEW
        };
    };

    using sdk_buffers::ProductBuffer;
    using sdk_buffers::ProductDescriptor;
    using sdk_buffers::reuseAfterReader;

    struct MutableReplica;
    MutableReplica* selected_replica = nullptr;
    void checkSelectedBacking(ResourceInstanceId other, const ResourceInput& backing) noexcept;

    struct ComputeCapture final
    {
        using Descriptor = ComputeCaptureDescriptor;
        static inline std::atomic_bool reject_resolve{};
        static inline std::array<CapturedPixels, 3> results;
        static inline std::size_t created = 0;
        static inline std::atomic_uint accepted{}, rejected{};
        static constexpr std::uint64_t preparation_revision = 1;
        static inline std::uint64_t captured_upload_bytes{};
        const ResourceUploader& uploads;
        const std::uint64_t initial_upload_bytes;
        CaptureBuffer buffer;
        CapturedPixels& result;
        std::uint32_t capacity;
        std::uint64_t epoch = 0;

        ResourceInstanceId identity;

        ComputeCapture(ResourceCreateContext& context, CapturedPixels& pixels, ResourceInstanceId id) noexcept
            : uploads(*context.uploads), initial_upload_bytes(uploads.statistics().copied_bytes), buffer(context), result(pixels),
              capacity(capture_bytes + (&pixels == &results[1] ? 16384u : 0u)), identity(id)
        {
        }

        ~ComputeCapture() noexcept
        {
            captured_upload_bytes = uploads.statistics().copied_bytes - initial_upload_bytes;
        }

        static ResourceRegistration factory() noexcept
        {
            static const StorageBindingOps binding{
                .prepareFrame = [](void* owner, ReplicaFrame frame, ResourceUploader&) noexcept
                    -> RenderResult<bool>
                {
                    auto& storage = *static_cast<ComputeCapture*>(owner);
                    CHECK(frame.epoch && storage.epoch == 0);
                    if (ProductBuffer::gate_next_reader.exchange(false)) ProductBuffer::live->delayRead();
                    storage.epoch = frame.epoch;
                    return true;
                },
                .preparationRevision = [](const void*) noexcept { return &preparation_revision; },
                .describeGraphImport = [](const void* owner, std::uint32_t projection) noexcept
                    -> RenderResult<ResourceProjection>
                {
                    if (projection != 0) return lux::cxx::unexpected(RenderError{kStorageContract});
                    const auto capacity = static_cast<const ComputeCapture*>(owner)->capacity;
                    return ResourceProjection::bufferOutput(capacity, EGraphUse::TRANSFER_DESTINATION);
                },
                .resolveGraphImport = [](
                    void* storage,
                    std::uint32_t,
                    const VGraphResourceDesc& description,
                    std::span<ResourceInput> output
                ) noexcept
                {
                    auto& capture = *static_cast<ComputeCapture*>(storage);
                    const CaptureConfig config{.result = &capture.result, .bytes = capture.capacity};
                    auto resolved = CaptureBuffer::resolve(&capture.buffer, description, &config, output);
                    if (resolved && reject_resolve.exchange(false))
                    {
                        checkSelectedBacking(capture.identity, output.front());
                        return RenderResult<void>{lux::cxx::unexpected(RenderError{kStorageMismatch})};
                    }
                    return resolved;
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
            return makeResourceRegistration<Descriptor, ComputeCapture, &create>(&binding);
        }

        static RenderResult<std::unique_ptr<ComputeCapture>>
        create(ResourceCreateContext& context, ResourceInstanceId id) noexcept
        {
            CHECK(created < results.size());
            return std::make_unique<ComputeCapture>(context, results[created++], id);
        }
    };

    struct ParkRequests final
    {
        static constexpr std::string_view name = "test.sdk.park_requests";
        using Request = EmptyRequest;
        using Response = AckResponse;
        static inline std::atomic_bool parked{}, entered{};

        static void wait(const Request&, Response& reply) noexcept
        {
            entered.store(true);
            parked.wait(true);
            reply = {};
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

    struct InitializeReplica final
    {
        static constexpr bool requires_idle_resource = true;
        static constexpr std::string_view name = "test.sdk.initialize_replica";
        enum class EProbe : std::uint8_t { WRITE, CANCEL, UNKNOWN, REPLACE };
        struct Request final
        {
            ResourceInstanceId resource;
            std::uint32_t marker;
            EProbe probe = EProbe::WRITE;
        };
        using Data = Request;
        using Response = AckResponse;
        [[nodiscard]] static ResourceInstanceId resourceId(const Request& request) noexcept { return request.resource; }
    };

    struct MutableReplicaDescriptor final
    {
        using Config = void;

        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.sdk.mutable_replica"}, 0, 0, {}, "Mutable replica.", EResourceDomain::VIEW
        };
    };

    struct MutableReplica final
    {
        static inline std::atomic_bool omit_upper_range{};
        using Descriptor = MutableReplicaDescriptor;
        static inline std::atomic_uint accepted_copies{}, accepted_no_copy{}, rejected{};
        DeviceSession& device;
        ResourceUploader& uploads;
        nri::Buffer* buffer = nullptr;
        std::uint64_t revision = 1, epoch = 0;
        std::uint32_t value = 0, selected = 0;
        std::optional<std::uint32_t> resident;
        bool fail_after_staging = false;

        static inline std::atomic_bool held{}, resume{};
        static inline std::atomic_uint injected{}, retired_candidates{};
        static inline decltype(nri::CoreInterface::QueueSubmit) submit{};

        ResourceRegistry& registry;
        ResourceInstanceId identity;
        static inline std::atomic_size_t notified{}, resolutions{};

        MutableReplica(ResourceCreateContext& context, ResourceInstanceId id) noexcept
            : device(context.device), uploads(*context.uploads), registry(context.resources), identity(id) {}

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
        create(ResourceCreateContext& context, ResourceInstanceId id) noexcept
        {
            if (!context.uploads || context.frame_count != 1)
                return lux::cxx::unexpected(RenderError{kDeviceUnsupported});
            return std::make_unique<MutableReplica>(context, id);
        }

        static exec::task<AckResponse>
        initialize(MutableReplica& storage, InitializeReplica::Request&& request) noexcept
        {
            if (storage.buffer && request.probe != InitializeReplica::EProbe::REPLACE) co_return AckResponse{RenderError{kStorageContract}};
            if (request.probe == InitializeReplica::EProbe::CANCEL)
            {
                co_await stdexec::starts_on(storage.device.nativeScheduler(), stdexec::just() | stdexec::then([]() noexcept
                {
                    held.store(true);
                    resume.wait(false);
                }));
                co_await stdexec::just_stopped();
            }
            const auto& core = storage.device.nriCore();
            const nri::BufferDesc desc{replica_bytes, 4, nri::BufferUsageBits::NONE};
            nri::Buffer* candidate = nullptr;
            const auto created = core.CreatePlacedBuffer(storage.device.device(), NriDeviceHeap, desc, candidate);
            if (created != nri::Result::SUCCESS) co_return AckResponse{RenderError{kDeviceFailure}};
            lux::cxx::scope_exit release_candidate([&]() noexcept
            {
                storage.device.deferRelease([destroy = core.DestroyBuffer, candidate]() noexcept
                {
                    destroy(candidate);
                    ++retired_candidates;
                });
            });
            const std::array words{0u, request.marker};
            const BufferWrite write{candidate, 0, std::as_bytes(std::span(words))};
            auto& hooks = const_cast<nri::CoreInterface&>(core);
            if (request.probe == InitializeReplica::EProbe::UNKNOWN)
            {
                submit = hooks.QueueSubmit;
                hooks.QueueSubmit = [](nri::Queue&, const nri::QueueSubmitDesc&) noexcept
                {
                    ++injected;
                    return nri::Result::FAILURE;
                };
            }
            auto accepted = storage.device.makeFrontiers();
            auto result = co_await storage.uploads.writeBuffers({&write, 1}, {}, &accepted);
            if (request.probe == InitializeReplica::EProbe::UNKNOWN) hooks.QueueSubmit = submit;
            if (result) result = co_await storage.device.waitFrontiers(std::move(accepted));
            if (!result) co_return AckResponse{result.error()};
            const ResourceInput input{candidate, 0, replica_bytes};
            const auto untouched = storage.registry.publishGraphInput(storage.identity, 1, 0, input);
            CHECK(untouched && *untouched == 0);
            const auto cleared = storage.registry.publishGraphInput(storage.identity, 0, UINT32_MAX, {});
            CHECK(cleared);
            if (*cleared)
            {
                const auto invalid = storage.registry.publishGraphInput(storage.identity, 0, 1, input);
                CHECK(!invalid && invalid.error().type == kFrameContract);
            }
            const auto changed = storage.registry.publishGraphInput(storage.identity, 0, 0, input);
            CHECK(changed && *changed == *cleared);
            notified = *changed;
            auto* previous = std::exchange(storage.buffer, candidate);
            release_candidate.release();
            if (previous)
            {
                storage.device.deferRelease([destroy = core.DestroyBuffer, previous]() noexcept { destroy(previous); });
                storage.resident.reset();
            }
            co_return AckResponse{};
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

        static std::span<const CommandBinding> commandBindings() noexcept
        {
            static const std::array commands{
                CommandBinding::declare<SetReplicaValue, &set>(),
                CommandBinding::declare<InitializeReplica, &initialize>()
            };
            return commands;
        }

        static ResourceRegistration factory() noexcept
        {
            static const StorageBindingOps bindings{
                .prepareFrame = [](void* owner, ReplicaFrame frame, ResourceUploader& uploads) noexcept
                { return static_cast<MutableReplica*>(owner)->prepare(frame, uploads); },
                .preparationRevision = [](const void* owner) noexcept
                { return &static_cast<const MutableReplica*>(owner)->revision; },
                .describeGraphImport = [](const void*, std::uint32_t projection) noexcept
                    -> RenderResult<ResourceProjection>
                {
                    if (projection != 0) return lux::cxx::unexpected(RenderError{kStorageContract});
                    ResourceProjection schema{
                        GraphBufferDesc{replica_bytes}, {GraphBufferRange{0, 4}},
                        EGraphUse::TRANSFER_SOURCE
                    };
                    if (!omit_upper_range) schema.initial_ranges.push_back(GraphBufferRange{4, 4});
                    return schema;
                },
                .resolveGraphImport = [](
                    void* owner, std::uint32_t, const VGraphResourceDesc&, std::span<ResourceInput> output
                ) noexcept -> RenderResult<void>
                {
                    ++resolutions;
                    auto* resource = static_cast<MutableReplica*>(owner);
                    if (ComputeCapture::reject_resolve) selected_replica = resource;
                    for (auto& value : output) value = {resource->buffer, 0, replica_bytes};
                    return {};
                },
                .settleFrame = [](void* owner, const ReplicaSettlement& receipt) noexcept
                { static_cast<MutableReplica*>(owner)->settle(receipt); }
            };
            return makeResourceRegistration<Descriptor, MutableReplica, &create>(&bindings);
        }
    };

    void checkSelectedBacking(ResourceInstanceId other, const ResourceInput& backing) noexcept
    {
        auto* selected = std::exchange(selected_replica, nullptr);
        CHECK(selected);
        const auto rejected = selected->registry.publishGraphInput(selected->identity, 0, UINT32_MAX, {});
        CHECK(!rejected && rejected.error().type == kDeviceBusy);
        // This owner has one previously installed reader and its backing has not yet been selected by the candidate.
        auto unchanged = selected->registry.publishGraphInput(other, 0, 0, backing);
        CHECK(unchanged && *unchanged == 1);
    }

    static_assert(CommandMethodFor<InitializeReplica, &MutableReplica::initialize>);

    inline constexpr EffectPortDefinition kComputeCapabilityPorts[]{{color, "color", {"test.sdk.compute_rgba8", 1}, graph::EPinDirection::OUTPUT}};

    inline constexpr EffectCapability kComputeCapability{
        .id = graph::nodeTypeId(compute_node), .canonical_name = compute_node, .ports = kComputeCapabilityPorts
    };

    inline constexpr EffectPortDefinition kCaptureCapabilityPorts[]{
                {color, "color", {"test.sdk.compute_rgba8", 1}, graph::EPinDirection::INPUT, 1, true},
                {pixels, "pixels", {"test.sdk.readback_bytes", 1}, graph::EPinDirection::OUTPUT}
            };

    inline constexpr EffectCapability kCaptureCapability{
        .id = graph::nodeTypeId(capture_node), .canonical_name = capture_node, .ports = kCaptureCapabilityPorts
    };

    struct ComputeRed final
    {
        using Configuration = std::monostate;
        using LinkedData = Configuration;

        static RenderResult<std::tuple<float, RevisionedValue<OutputConstants>>>
        prepare(const LinkedData&, const CpuFrameInputs<OutputState>& inputs) noexcept
        {
            const auto& state = std::get<0>(inputs.values).value;
            const float red = state.previous.red - state.constants.red;
            return std::tuple{red, RevisionedValue<OutputConstants>{{red}, state.revision}};
        }
    };

    struct CheckRed final
    {
        using Configuration = float;
        using LinkedData = Configuration;

        static RenderResult<void> prepare(const float& expected, const CpuFrameInputs<float>& inputs) noexcept
        {
            CHECK(std::get<0>(inputs.values).value == expected);
            return {};
        }
    };

    struct WriteParticles final
    {
        static constexpr std::string_view name = "example.particles.update";
        struct Request final
        {
            ResourceInstanceId resource;
            std::span<const sdk_particles::ParticleUpdate> rows;
        };
        using Data = sdk_particles::ParticleUpdate;
        using Response = AckResponse;
        static ResourceInstanceId resourceId(const Request& request) noexcept { return request.resource; }
    };

    void writeParticles(
        sdk_particles::ParticleStorage& owner, const WriteParticles::Request& request, AckResponse& reply
    ) noexcept
    {
        const auto result = owner.update(request.rows);
        reply.error = result ? RenderError{} : result.error();
    }

    struct HoldDebugGeometry final
    {
        static constexpr std::string_view name = "example.debug_geometry.hold";
        static constexpr bool requires_idle_resource = true;
        using Request = ResourceInstanceId;
        using Data = ResourceInstanceId;
        using Response = AckResponse;
        static ResourceInstanceId resourceId(Request id) noexcept { return id; }
    };

    struct ReleaseDebugGeometry final
    {
        static constexpr std::string_view name = "example.debug_geometry.release";
        using Request = EmptyRequest;
        using Response = AckResponse;

        static inline std::atomic_bool entered{};
        static inline void* pending{};
        static inline void (*complete)(void*) noexcept{};

        static exec::task<AckResponse> hold(sdk_debug::DebugDrawResource& resource, ResourceInstanceId&&) noexcept
        {
            resource.held = true;
            const auto reply = co_await exec::create<stdexec::set_value_t(AckResponse)>([](auto& operation) noexcept
            {
                using Operation = std::remove_reference_t<decltype(operation)>;
                pending = &operation;
                complete = [](void* address) noexcept
                {
                    stdexec::set_value(std::move(static_cast<Operation*>(address)->receiver), AckResponse{});
                };
                entered.store(true);
            });
            resource.held = false;
            co_return reply;
        }

        static void release(const EmptyRequest&, AckResponse& reply) noexcept
        {
            CHECK(pending);
            complete(std::exchange(pending, nullptr));
            reply = {};
        }
    };

    RenderResult<void> buildDebugGeometry(EffectCompileContext& context, std::string_view) noexcept
    {
        using Resource = sdk_debug::DebugDrawResource;
        const auto geometry = take(context.importProjection("geometry", "draw_bytes", Resource::projection()));
        const auto view = take(context.importProjection("scene_view", "view_row", ViewParameterResource::projection()));
        GraphImageDesc image{width, height};
        image.extent.source = EImageExtentSource::TARGET;
        const auto output = take(context.declareResource({"debug color", image}));
        RasterPassTemplate list;
        list.purpose = "debug geometry";
        list.draw_list = "geometry";
        list.inputs = {
            {"scene_view", rdesc::EDescriptorType::STORAGE_BUFFER, EGraphUse::STORAGE_READ, 2},
            {.name = "source_color", .kind = rdesc::EDescriptorType::SAMPLED_IMAGE, .usage = EGraphUse::SAMPLED,
             .stages = 2, .target_format = rdesc::ETextureFormat::RGBA8_UNORM}
        };
        list.resources = {
            {.version = geometry, .usage = EGraphUse::VERTEX}, {.version = geometry, .usage = EGraphUse::INDEX},
            {.version = view, .usage = EGraphUse::STORAGE_READ}
        };
        const auto vertex = kSdkFullscreen.load(context.shaderTarget());
        std::vector<RasterPassTemplate::StreamProgram> programs;
        const auto target = context.shaderTarget();
        for (auto shader : {kViewTint.load(target), kSdkGreen.load(target), kSdkSampleTarget.load(target)})
        {
            sdk_programs::attachVertexShader(shader, vertex);
            auto& program = programs.emplace_back();
            program.program = take(context.addProgram(GraphProgram{std::move(shader)}));
            program.graphics.vertex_bindings = {{0, sizeof(sdk_debug::Vertex)}};
            program.graphics.vertex_attributes = {{
                vertex.interface.vertex_inputs.front().location, 0, 0, rdesc::ETextureFormat::RG32_SFLOAT
            }};
        }
        list.programs = std::move(programs);
        RasterWork work;
        work.colors = {{{{output, 0}}, EAttachmentLoad::CLEAR, EAttachmentStore::STORE, {0, 0, 1, 1}}};
        work.program = std::move(list);
        work.viewport = RasterViewport{0, 0, 1, .5f};
        const auto upper = take(context.addRasterPass("overlay upper half", work));
        work.viewport = RasterViewport{0, .5f, 1, .5f};
        work.colors.front().access.version = upper.written.front();
        work.colors.front().load = EAttachmentLoad::LOAD;
        const auto lower = take(context.addRasterPass("overlay lower half", std::move(work)));
        return context.publish(color, {{color, lower.written.front()}});
    }

    RenderResult<void> buildParticleColor(EffectCompileContext& context, std::string_view) noexcept
    {
        using Resource = sdk_particles::ParticleResource;
        const auto particles = take(context.importProjection("foreground", "particles", Resource::projection()));
        const auto trail = take(context.importProjection("trail", "trail_rows", Resource::projection()));
        GraphImageDesc image{width, height};
        image.extent.source = EImageExtentSource::TARGET;
        const auto generated = take(context.declareResource({"particle colors", image}));
        const auto output = take(context.declareResource({"particle output", image}));
        ComputeWork work;
        work.interface.program = take(context.addProgram(GraphProgram{kParticleColor.load(context.shaderTarget())}));
        work.interface.resources = {
            {"particles", {particles}}, {"trail", {trail}}, {"generated", {{generated, 0}, {}, EGraphAccess::WRITE}}
        };
        work.dispatch = ImageDispatchArguments{generated};
        const auto computed = take(context.addComputePass("read particle rows", std::move(work)));
        const auto copied = take(context.addTransferPass(
            "copy particle pixels", TransferWork{{CopyImage{{computed.written.front()}, {{output, 0}}}}}
        ));
        return context.publish(color, {{color, copied.written.front()}});
    }

    RenderResult<void> buildCompute(EffectCompileContext& context, std::string_view mode) noexcept
    {
        GraphImageDesc image{width, height};
        image.extent.source = EImageExtentSource::TARGET;
        const auto generated = take(context.declareResource({"compute storage", image}));
        const auto output = take(context.declareResource({"compute output", image}));
        const auto colors = context.constantBuffer<OutputChannels, OutputState,
            &OutputState::channels, &OutputState::revision>();
        if (!colors) return lux::cxx::unexpected(colors.error());
        ComputeWork work;
        work.interface.program = take(context.addProgram(GraphProgram{kTypedGraphFill.load(context.shaderTarget())}));
        work.interface.parameters.setInlineConstant(OutputConstants{0});
        work.interface.resources = {{"generated", {{generated, 0}, {}, EGraphAccess::WRITE}}, {"colors", {*colors}}};
        work.interface.resources.back().setUniformConstant<OutputChannels>();
        work.dispatch = ImageDispatchArguments{generated};
        const auto dispatch = take(context.addComputePass("generate compute color", std::move(work)));
        if (mode == "computed")
        {
            const auto red = take(context.declareCpuData<float>());
            const auto state = take(context.importEffectState<OutputState>());
            if (auto check = context.addCpuCall<CheckRed>(.5f, {.inputs = {red}, .passes = {dispatch.pass}}); !check)
                return check;
            if (auto result = context.addCpuCall<ComputeRed>({}, {
                .inputs = {state}, .outputs = {red}, .inline_blocks = {dispatch.pass}
            }); !result) return result;
        }
        else
        {
            auto copy = context.copyInlineBlock<OutputConstants, OutputState, &OutputState::constants>(dispatch.pass);
            if (!copy) return copy;
        }
        const auto copied = take(context.addTransferPass(
            "copy compute color", TransferWork{{CopyImage{{dispatch.written.front()}, {{output, 0}}}}}
        ));
        return context.publish(color, {{color, copied.written.front()}});
    }

    RenderResult<void> buildResidentRaster(EffectCompileContext& context, std::string_view) noexcept
    {
        using namespace sdk_programs;
        auto image = take(context.declareResource({"raster output", GraphImageDesc{width, height}}));
        RasterPassTemplate stage;
        stage.purpose = "installed programs";
        DrawResourceType mask{ResourceName{DrawMask::descriptor.type.name()}, lux::cxx::make_basic_type_info<DrawMask>()};
        stage.draw_resources.push_back({"clip_height", std::move(mask), 0, true});
        stage.draw_resources.push_back({
            .binding = "triangle",
            .source = {
                ResourceName{TriangleBatch::descriptor.type.name()}, lux::cxx::make_basic_type_info<TriangleBatch>()
            },
            .indexed_geometry = true
        });
        const bool reads_records = context.device().texture_descriptors && context.device().record_table;
        if (reads_records)
        {
            stage.draw_resources.push_back({
                "colors",
                {ResourceName{ColorPage::descriptor.type.name()}, lux::cxx::make_basic_type_info<ColorPage>()},
                ColorPage::capability
            });
            stage.inputs = ProgramDraws::arguments(true);
            stage.programs = RasterPassTemplate::ReadyPrograms{};
        }
        else
        {
            auto description = ProgramDraws::describe(context.shaderTarget(), false);
            stage.inputs = std::move(description.arguments);
            std::vector<std::uint32_t> ids;
            for (auto& shader : description.shaders)
                ids.push_back(take(context.addProgram(GraphProgram{std::move(shader)})));
            for (auto& program : description.programs) program.program = ids[program.program];
            stage.programs = std::move(description.programs);
        }
        const auto camera = take(context.importStorageSnapshot<ViewGpuParameters>(
            ViewParameterResource::snapshot_binding, kViewParameterResourceName,
            ESnapshotScope::JOB_LOCAL
        ));
        RasterWork raster;
        raster.program = std::move(stage);
        raster.colors.push_back({{{image, 0}}, EAttachmentLoad::CLEAR, EAttachmentStore::STORE, {0, 0, 1, 1}});
        auto draw = context.addRasterPass("two programs", std::move(raster));
        if (!draw) return lux::cxx::unexpected(draw.error());
        auto added = context.addCpuCall<ProgramDraws>(reads_records, {.storage_inputs = {camera}, .draw_streams = {draw->pass}});
        if (!added) return added;
        return context.publish(color, {{color, draw->written.front()}});
    }

    RenderResult<void> buildCaptureRange(EffectCompileContext& context, std::string_view mode) noexcept
    {
        const std::uint64_t offset = mode == "upper" ? 8u : mode == "overlap" ? 4u : 0u;
        const GraphBufferRange claim{offset, 8};
        const bool shared = mode == "unauthorized";
        GraphResource declaration{
            "", GraphBufferDesc{capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION, claim
        };
        if (shared) declaration = {
            "", GraphBufferDesc{replica_bytes}, EGraphOrigin::IMPORTED,
            {GraphBufferRange{0, replica_bytes}}, EGraphUse::TRANSFER_SOURCE, claim
        };
        const auto type = shared ? MutableReplicaDescriptor::descriptor.type :
            ComputeCaptureDescriptor::descriptor.type;
        const auto output = context.importFinalBuffer(
            "capture", type, std::move(declaration)
        );
        if (!output) return lux::cxx::unexpected(output.error());
        const auto scratch = take(context.declareResource({"pattern", GraphBufferDesc{claim.size}}));
        const auto pattern = offset == 0 ? 0x5a5a5a5au : 0x6b6b6b6bu;
        const auto filled = take(context.addTransferPass(
            "fill pattern", TransferWork{{FillBuffer{{{scratch, 0}}, pattern}}}
        ));
        const GraphBufferRange destination{mode == "spill" ? 4u : offset, claim.size};
        const auto copy = context.addTransferPass("copy", TransferWork{{
            CopyBuffer{{filled.written.front()}, {*output, destination}}
        }});
        if (!copy) return lux::cxx::unexpected(copy.error());
        return context.publish(pixels, {{pixels, copy->written.front()}});
    }

    RenderResult<void> buildCapture(EffectCompileContext& context, std::string_view mode) noexcept
    {
        if (mode == "publish-7" || mode == "publish-11" || mode == "publish-low")
        {
            const GraphBufferRange range{mode == "publish-low" ? 0u : 8u, 8};
            const auto dest = take(context.importFinalBuffer(
                "product", ProductDescriptor::descriptor.type,
                {
                    "", GraphBufferDesc{ProductBuffer::bytes}, EGraphOrigin::IMPORTED, {},
                    EGraphUse::TRANSFER_SOURCE, range
                }
            ));
            const auto tmp = take(context.declareResource({"sample", GraphBufferDesc{range.size}}));
            const auto word = mode == "publish-7" ? 7u : 11u;
            const auto fill = take(context.addTransferPass("sample", TransferWork{{
                FillBuffer{{{tmp, 0}, GraphBufferRange{0, 4}}, word},
                FillBuffer{{{tmp, 0}, GraphBufferRange{4, 4}}, word + 1}
            }}));
            const auto copy = take(context.addTransferPass(
                "publish", TransferWork{{CopyBuffer{{fill.written.front()}, {dest, range}}}}
            ));
            return context.publish(pixels, {{pixels, copy.written.front()}});
        }
        if (mode == "consume-version" || mode == "consume-word")
        {
            const GraphBufferRange bytes{0, mode == "consume-word" ? 4u : 8u};
            const auto source = context.importResource(
                "version", ProductDescriptor::descriptor.type,
                {GraphBufferDesc{bytes.size}, {bytes}, EGraphUse::TRANSFER_SOURCE}
            );
            if (!source) return lux::cxx::unexpected(source.error());
            const auto dest = take(context.importFinalBuffer(
                "capture", ComputeCaptureDescriptor::descriptor.type,
                {"", GraphBufferDesc{capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION, bytes}
            ));
            const auto copy = take(context.addTransferPass(
                "read version", TransferWork{{CopyBuffer{{*source}, {dest, bytes}}}}
            ));
            return context.publish(pixels, {{pixels, copy.written.front()}});
        }
        const bool raster_only = mode == "raster-pixels";
        if (!mode.empty() && !raster_only) return buildCaptureRange(context, mode);
        const auto input = context.inputs(color);
        CHECK(input.size() == 1 && input.front().size() == 1 && input.front().front().member == color);
        const GraphBufferRange claim{0, raster_only ? readback_bytes : capture_bytes};
        const auto output = take(context.importFinalBuffer(
            "capture", ComputeCaptureDescriptor::descriptor.type,
            {"", GraphBufferDesc{capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION, claim}
        ));
        const ImageCopyRegion region{0, 0, width, height};
        const auto copied = take(context.addTransferPass("copy to readback", TransferWork{{
            CopyImageBuffer{{input.front().front().version}, {output}, row_pitch, readback_bytes, region}
        }}));
        if (raster_only) return context.publish(pixels, {{pixels, copied.written.front()}});
        const auto mutable_input = take(context.importResource(
            "a_mutable", MutableReplicaDescriptor::descriptor.type,
            {
                GraphBufferDesc{replica_bytes}, {GraphBufferRange{0, replica_bytes}}, EGraphUse::TRANSFER_SOURCE
            }
        ));
        const auto complete = take(context.addTransferPass("copy mutable replica", TransferWork{{
            CopyBuffer{{mutable_input}, {copied.written.front(), GraphBufferRange{readback_bytes, replica_bytes}}}
        }}));
        return context.publish(pixels, {{pixels, complete.written.front()}});
    }

    constexpr std::string_view fault_node = "test.sdk.partial_output";

    inline constexpr EffectPortDefinition kFaultCapabilityPorts[]{
                {color, "color", {"test.sdk.compute_rgba8", 1}, graph::EPinDirection::OUTPUT},
                {pixels, "pixels", {"test.sdk.readback_bytes", 1}, graph::EPinDirection::OUTPUT}
            };

    inline constexpr EffectCapability kFaultCapability{
        .id = graph::nodeTypeId(fault_node), .canonical_name = fault_node, .ports = kFaultCapabilityPorts
    };

    RenderResult<void> buildPartialOutput(EffectCompileContext& context, std::string_view) noexcept
    {
        constexpr GraphBufferRange range{8, 8};
        const auto dest = take(context.importFinalBuffer(
            "product", ProductDescriptor::descriptor.type,
            {"", GraphBufferDesc{ProductBuffer::bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_SOURCE, range}
        ));
        const auto write = take(context.addTransferPass(
            "write product", TransferWork{{FillBuffer{{dest, range}, 11}}}, {.estimated_gpu_ns = 1000}
        ));
        const auto image = take(context.declareResource({"async result", GraphImageDesc{width, height}}));
        ComputeWork work;
        work.allow_async_compute = true;
        work.interface.program = take(context.addProgram(GraphProgram{kGraphFill.load(context.shaderTarget())}));
        work.interface.parameters.setInlineConstant(OutputConstants{0.25f});
        work.interface.resources = {{"generated", {{image, 0}, {}, EGraphAccess::WRITE}}};
        work.dispatch = ImageDispatchArguments{image};
        const auto async = take(context.addComputePass(
            "async work", std::move(work), {.side_effect = true, .estimated_gpu_ns = 100000}
        ));
        const auto output = take(context.declareResource({"graphics color", GraphImageDesc{width, height}}));
        const auto graphics = take(context.addTransferPass(
            "graphics work", TransferWork{{ClearImage{{{output, 0}}}}}, {.estimated_gpu_ns = 100000}
        ));
        CHECK(context.dependsOn(write.pass, async.pass));
        CHECK(context.dependsOn(write.pass, graphics.pass));
        if (auto published = context.publish(color, {{color, graphics.written.front()}}); !published) return published;
        return context.publish(pixels, {{pixels, write.written.front()}});
    }

    template <EffectImplementation::Build Build>
    void registerNode(RenderServer& server, const EffectCapability& capability, graph::PinSemanticId output)
    {
        CHECK(server.registerEffectCapability(capability));
        EffectImplementation implementation;
        implementation.identity = {std::string{capability.canonical_name}, capability.version};
        implementation.capability = capability.id;
        implementation.supported_outputs = {output};
        if constexpr (Build == buildPartialOutput) implementation.supported_outputs.push_back(color);
        implementation.initialize = [](EffectInitializationContext& context, std::string_view arguments) noexcept
            -> RenderResult<void>
        {
            if constexpr (Build == buildCompute)
            {
                if (!arguments.empty() && arguments != "computed")
                    return lux::cxx::unexpected(RenderError{kInvalidConfig});
                return context.initializeState<OutputState>();
            }
            const bool known = arguments.empty() || arguments == "lower" || arguments == "upper" ||
                arguments == "overlap" || arguments == "spill" || arguments == "unauthorized" ||
                arguments == "publish-7" || arguments == "publish-11" || arguments == "publish-low" ||
                arguments == "consume-version" || arguments == "consume-word" || arguments == "raster-pixels";
            if (!known) return lux::cxx::unexpected(RenderError{kInvalidConfig});
            return {};
        };
        implementation.build = Build;
        CHECK(server.registerEffectImplementation(std::move(implementation)));
    }
}

int main(int argc, char** argv)
{
    const bool d3d12 = argc > 1 && std::string_view{argv[1]} == "d3d12";
    auto execution = lux::execution::ExecutionRuntime::create({2, 128, {128}});
    CHECK(execution);
    RenderServerConfig config{"lux.render.sdk.compute_output", nativeValidationEnabled()};
    const auto mode = argc > 2 ? std::string_view{argv[2]} : std::string_view{};
    const bool records = mode == "resident-records";
    const bool resident_programs = records || mode == "cold-programs";
    const bool receipts = mode == "receipts";
    const bool product_fault = mode == "receipt-partial" || mode == "receipt-unknown";
    const bool particles = mode == "particles";
    const bool target_list = mode == "target-list";
    const bool debug_draw = target_list || mode == "debug-draw";
    if (particles || debug_draw)
    {
        config.diagnostics.report = [](void*, const RenderDiagnostic& diagnostic) noexcept
        {
            if (diagnostic.severity == ERenderDiagnosticSeverity::ERROR_MESSAGE)
                std::fprintf(stderr, "Resource SDK: %.*s\n", int(diagnostic.message.size()), diagnostic.message.data());
        };
    }
    config.frames_in_flight = receipts || particles || debug_draw ? 2 : 1;
    config.uploads.emplace();
    DeviceOptions options;
    options.texture_descriptors = target_list;
    options.multiple_queues = receipts || product_fault || target_list;
    if (product_fault) options.graph_queue_costs = {100, 100, 100};
    if (d3d12) options.backend = EGraphicsBackend::D3D12;
    auto server = take(RenderServer::create(config, **execution));
    if (resident_programs)
    {
        options.cached_unused_programs = 0;
        options.texture_descriptors = records;
        if (records) take(server->registerResource<sdk_programs::ColorPage>());
        take(server->registerResource<sdk_programs::DrawMask>());
        const std::array order_command{
            CommandBinding::declare<sdk_programs::SetDrawOrder, &sdk_programs::TriangleBatch::setOrder>(),
            CommandBinding::declare<sdk_programs::PrepareRecordPrograms, &sdk_programs::TriangleBatch::preparePrograms>()
        };
        take(server->registerResource<sdk_programs::TriangleBatch>(order_command));
        registerNode<buildResidentRaster>(*server, kComputeCapability, color);
    }
    else if (debug_draw)
    {
        using Resource = sdk_debug::DebugDrawResource;
        const std::array holds{CommandBinding::declare<HoldDebugGeometry, &ReleaseDebugGeometry::hold>()};
        const std::array releases{CommandBinding::declare<ReleaseDebugGeometry, &ReleaseDebugGeometry::release>()};
        take(server->registerResource<Resource>(holds));
        CHECK(server->registerCommands(releases));
        auto capability = kComputeCapability;
        const ResourceRequirement resources[]{
            {"geometry", Resource::descriptor.type, EResourceDomain::JOB, EResourceContentLifetime::PER_RUN},
            {"scene_view", ViewParameterResource::descriptor.type, EResourceDomain::VIEW}
        };
        capability.resources = resources;
        registerNode<buildDebugGeometry>(*server, capability, color);
    }
    else if (particles)
    {
        const std::array updates{CommandBinding::declare<WriteParticles, &writeParticles>()};
        take(server->registerResource<sdk_particles::ParticleStorage>(updates));
        auto capability = kComputeCapability;
        std::string type_name{sdk_particles::ParticleResource::descriptor.type.name()};
        const ResourceNameView type{type_name};
        std::string foreground = "foreground";
        const ResourceRequirement resources[]{{foreground, type, EResourceDomain::VIEW}, {"trail", type, EResourceDomain::VIEW}};
        capability.resources = resources;
        registerNode<buildParticleColor>(*server, capability, color);
        foreground.clear();
        type_name.clear();
    }
    else registerNode<buildCompute>(*server, kComputeCapability, color);
    registerNode<buildCapture>(*server, kCaptureCapability, pixels);
    take(server->registerResource<ComputeCapture>());
    take(server->registerResource<MutableReplica>(MutableReplica::commandBindings()));
    if (!resident_programs) take(server->registerResource<ProductBuffer>());
    if (product_fault) registerNode<buildPartialOutput>(*server, kFaultCapability, pixels);
    const std::array park_commands{CommandBinding::declare<ParkRequests, &ParkRequests::wait>()};
    CHECK(server->registerCommands(park_commands));
    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    RenderResult<void> ran;
    std::thread worker([&] { ran = server->run(); });
    const auto commands = queryCommands(*client);
    const auto request_pair = [&](RenderJobId a, RenderJobId b)
    {
        auto park = ipc::IpcCore::instance().createRequester(config.channel_name);
        auto other = ipc::IpcCore::instance().createRequester(config.channel_name);
        CHECK(park && other);
        ParkRequests::entered = false;
        ParkRequests::parked = true;
        const auto parked = sendSceneRequest<ParkRequests>(*park, commands, {});
        const auto deadline = Clock::now() + 10s;
        while (!ParkRequests::entered.load())
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
        const auto first = sendSceneRequest<RequestRenderJob>(*client, commands, {a});
        const auto second = sendSceneRequest<RequestRenderJob>(*other, commands, {b});
        ParkRequests::parked = false;
        ParkRequests::parked.notify_all();
        receiveSceneReply<ParkRequests>(*park, parked);
        return std::pair{receiveSceneReply<RequestRenderJob>(*client, first, false),
                         receiveSceneReply<RequestRenderJob>(*other, second, false)};
    };
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto view = call<CreateView>(*client, commands, {scene}).view;
    const auto set_viewport = [&](ViewId id, unsigned inset)
    {
        ViewGpuParameters parameters;
        const auto left = static_cast<float>(inset) / width;
        parameters.viewport = {left, 0, 1 - left, 1};
        call<UpdateView>(*client, commands, {{scene, id}, parameters});
    };
    if (resident_programs) set_viewport(view, 1);
    const auto session = call<CreateDeviceSession>(*client, commands, options).device;
    const auto format = rdesc::ETextureFormat::RGBA8_UNORM;
    const TargetDesc output_desc{.width = width, .height = height, .format = format, .sampled = target_list};
    const auto target = call<CreateTarget>(*client, commands, {session, output_desc}).target;
    const auto readback = call<GetOrCreateResource>(
        *client, commands, {session, ViewScope{scene, view}, ComputeCaptureDescriptor::descriptor.type}
    ).resource_id;
    const auto capture_request = [&](std::string mode)
    {
        CreateEffectRequest effect;
        effect.output = output_desc.outputContract();
        effect.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*effect.graph};
        std::string source_mode;
        if (!resident_programs && mode == "computed") source_mode = std::exchange(mode, {});
        const auto compute = take(edit.addNode(
            kComputeCapability, {std::string{compute_node}, 1}, std::move(source_mode)
        ));
        const auto capture = take(edit.addNode(kCaptureCapability, {std::string{capture_node}, 1}, std::move(mode)));
        const auto color_pin = edit.pinId(compute, color);
        CHECK(edit.connect(color_pin, edit.pinId(capture, color)));
        CHECK(edit.setOutput("color", color_pin, true));
        CHECK(edit.setOutput("pixels", edit.pinId(capture, pixels)));
        CHECK(edit.commit());
        return effect;
    };
    const auto create_capture = [&](std::string mode)
    {
        return createPreparedEffect(*client, commands, session, capture_request(std::move(mode)));
    };
    if (debug_draw)
    {
        using namespace sdk_debug;
        using Resource = DebugDrawResource;
        const auto effect = create_capture("raster-pixels");
        const auto peer_target = call<CreateTarget>(*client, commands, {session, output_desc}).target;
        const auto peer_capture = call<GetOrCreateResource>(*client, commands, {
            session, ViewScope{scene, view}, ComputeCaptureDescriptor::descriptor.type, "peer"
        }).resource_id;
        JobBinding binding;
        binding.resources = {{"capture", readback}};
        const auto first = startRenderJob(*client, commands, session, scene, view, target, effect, binding);
        binding.resources.front().resource = peer_capture;
        const auto peer = startRenderJob(*client, commands, session, scene, view, peer_target, effect, binding);
        ViewGpuParameters tint;
        tint.background = {1, 0, 0, 1};
        call<UpdateView>(*client, commands, {{scene, view}, tint});
        const auto other_view = call<CreateView>(*client, commands, {scene}).view;
        tint.background = {1, 1, 0, 1};
        call<UpdateView>(*client, commands, {{scene, other_view}, tint});
        const auto other_target = call<CreateTarget>(*client, commands, {session, output_desc}).target;
        binding.resources.front().resource = call<GetOrCreateResource>(*client, commands, {
            session, ViewScope{scene, other_view}, ComputeCaptureDescriptor::descriptor.type
        }).resource_id;
        const auto overlay = startRenderJob(*client, commands, session, scene, other_view, other_target, effect, binding);
        const auto geometry = [&](RenderJobId job)
        {
            return call<QueryResource>(*client, commands, {session, job, Resource::descriptor.type, "geometry"}).address();
        };
        const auto a = geometry(first);
        const auto b = geometry(peer);
        const auto c = geometry(overlay);
        CHECK(a != b && b != c && a != c && Resource::allocated_buffers == 6);
        std::array packets{
            take(FrameData::create<std::tuple<Resource>>({a})), take(FrameData::create<std::tuple<Resource>>({b})),
            take(FrameData::create<std::tuple<Resource>>({c}))
        };
        const auto update = [&](ResourceAddress address, std::span<const Vertex> points,
                                std::span<const Index> indices, std::span<const Draw> commands_in)
        {
            auto& packet = packets[address == a ? 0 : address == b ? 1 : 2];
            packet.reset();
            for (auto point : points) CHECK(packet.addData<Resource>(point));
            for (auto index : indices) CHECK(packet.addData<Resource>(index));
            for (auto draw : commands_in) CHECK(packet.addData<Resource>(draw));
            CHECK(packet.seal());
            const auto error = call<UpdateFrameData>(*client, commands, {&packet}, false).error;
            packet.release();
            return error;
        };
        constexpr std::array full{Vertex{-1,-1}, Vertex{3,-1}, Vertex{-1,3}};
        constexpr std::array half{Vertex{-1,-1}, Vertex{0,-1}, Vertex{0,1}, Vertex{-1,1}};
        constexpr std::array triangle{Index{0}, Index{1}, Index{2}};
        constexpr std::array quad{Index{0}, Index{1}, Index{2}, Index{0}, Index{2}, Index{3}};
        const std::array full_list{
            Draw{0, 3, 0, {-12, -4, width, height}},
            Draw{0, 3, 1, {int(width / 2), 0, width + 2, height}},
            Draw{0, 3, 0, {width + 1, 0, width + 3, height}}
        };
        const std::array half_list{Draw{0, 6, 0, {0, 2, width, height}}};
        if (target_list)
        {
            const std::array solid{Draw{0, 3, 0, {0, 0, width, height}}};
            CHECK(!update(b, full, triangle, solid).type);
            renderFrame(*client, commands, peer);
            CHECK(!update(c, full, triangle, solid).type);
            renderFrame(*client, commands, overlay);

            const auto sample = [](TargetId id, EProductRead mode, int left)
            {
                return Draw{0, 3, 2, {left, 0, width, height}, {id, mode}};
            };
            const std::array latest{
                sample(peer_target, EProductRead::LATEST_PUBLISHED, 0),
                sample(other_target, EProductRead::LATEST_PUBLISHED, width / 2)
            };
            const std::array current{
                sample(other_target, EProductRead::CURRENT_RUN, 0),
                sample(other_target, EProductRead::LATEST_PUBLISHED, 3 * width / 4),
                sample(peer_target, EProductRead::LATEST_PUBLISHED, width - 4)
            };
            CHECK(!update(a, full, triangle, current).type);
            CHECK(call<RequestRenderJob>(*client, commands, {first}, false).error.type == kRuntimeState);
            const std::array cycle{sample(target, EProductRead::CURRENT_RUN, 0)};
            CHECK(!update(c, full, triangle, cycle).type);
            const auto cyclic = request_pair(first, overlay);
            CHECK(cyclic.first.error.type == kJobCycle && cyclic.second.error.type == kJobCycle);

            CHECK(!update(a, full, triangle, latest).type);
            const auto mixed = call<RequestRenderJob>(*client, commands, {first});
            CHECK(!update(a, full, triangle, current).type);
            CHECK(!update(c, half, quad, half_list).type);
            call<GetOrCreateResource>(*client, commands, {session, ViewScope{scene, view}, ProductDescriptor::descriptor.type});
            auto& gate = *ProductBuffer::live;
            const bool delayed = gate.canDelayRead();
            ProductBuffer::gate_next_reader = delayed;
            const auto [read, wrote] = request_pair(first, overlay);
            CHECK(!read.error.type && !wrote.error.type && read.frame_index == wrote.frame_index);
            CHECK(mixed.slot_index != read.slot_index);
            stopAndRemoveJob(*client, commands, peer);
            stopAndRemoveJob(*client, commands, overlay);
            // The peer contributed only a previously published image to the last pair.
            // Retire its Target with the consumer's GPU read still pending on devices with a separate signal queue.
            call<RemoveTarget>(*client, commands, peer_target);
            if (delayed)
            {
                CHECK(gate.device.nriCore().GetFenceValue(*gate.gate) == 0);
                gate.releaseRead();
            }
            call<RemoveTarget>(*client, commands, other_target);
            CHECK(!update(a, full, triangle, current).type);
            CHECK(call<RequestRenderJob>(*client, commands, {first}, false).error.type == kRuntimeIdentity);
            stopAndRemoveJob(*client, commands, first);
            call<RemoveView>(*client, commands, {scene, other_view});
            call<RemoveView>(*client, commands, {scene, view});
            server->stop();
            worker.join();
            CHECK(ran && server->stats().validation_errors == 0);
            server.reset();
            const auto& result = ComputeCapture::results[0];
            CHECK(result.ready && result.frames.size() == 2);
            for (auto slot : {mixed.slot_index, read.slot_index})
                for (std::uint32_t y = 0; y < height; ++y)
                    for (std::uint32_t x = 0; x < width; ++x)
                    {
                        const bool left = x < width / 2;
                        const bool previous = slot == mixed.slot_index;
                        const bool peer_strip = !previous && x >= width - 4;
                        const bool yellow = previous ? !left : !peer_strip && ((left && y >= 2) || x >= 3 * width / 4);
                        const bool blue = !previous && !yellow && !peer_strip;
                        const std::array<std::uint8_t, 4> pixel{
                            std::uint8_t(blue ? 0 : 255), std::uint8_t(yellow ? 255 : 0),
                            std::uint8_t(blue ? 255 : 0), 255
                        };
                        CHECK(std::memcmp(result.frames[slot].data() + y * result.pitch + x * 4, pixel.data(), 4) == 0);
                    }
            std::printf("TARGET DRAW LIST PASS: current and published pixels, stale/cycle rejected, delayed reader=%u\n",
                unsigned(delayed));
            return 0;
        }
        CHECK(!update(a, full, triangle, full_list).type);
        CHECK(!update(b, half, quad, half_list).type);
        CHECK(!update(c, half, quad, half_list).type);
        CHECK(update(b, half, std::span{quad}.first(2), half_list).type == kFrameContract);
        const auto check_runs = [&](RenderJobId job)
        {
            const auto draw = call<RequestRenderJob>(*client, commands, {job}).slot_index;
            const auto empty = call<RequestRenderJob>(*client, commands, {job}).slot_index;
            CHECK(draw != empty);
            return draw;
        };
        std::array draws{check_runs(first), check_runs(peer), check_runs(overlay)};
        // Reuse both slots and change only the clip edge. The retained CB for the first slot must be invalidated.
        auto narrower = full_list;
        narrower[1].clip.left = 3 * width / 4;
        CHECK(!update(a, full, triangle, narrower).type);
        CHECK(check_runs(first) == draws[0]);
        CHECK(Resource::allocated_buffers == 6);

        // Address vertices and indices beyond the former 256/768 limits on the same live Job.
        std::vector<Vertex> large_points(1024);
        std::ranges::copy(full, large_points.end() - full.size());
        std::vector<Index> large_indices(900);
        for (std::uint32_t i = 0; i < 3; ++i) large_indices[897 + i] = Index{1021 + i};
        for (auto& draw : narrower) draw.first = 897;
        Resource::reject_next_publication = true;
        CHECK(!update(a, large_points, large_indices, narrower).type);
        const auto rejected = call<RequestRenderJob>(*client, commands, {first}, false);
        CHECK(rejected.error.type == kGraphInvalidRange && Resource::discarded_candidates == 1);
        renderFrame(*client, commands, first);
        CHECK(Resource::allocated_buffers == 7);
        CHECK(!update(a, full, triangle, full_list).type);
        check_runs(first);
        CHECK(Resource::allocated_buffers == 7);
        for (int upload = 0; upload < 2; ++upload)
        {
            CHECK(!update(a, large_points, large_indices, narrower).type);
            draws[0] = check_runs(first);
            CHECK(Resource::allocated_buffers == 8);
        }
        auto holding = ipc::IpcCore::instance().createRequester(config.channel_name);
        CHECK(holding);
        auto held = std::async(std::launch::async, [&] { return call<HoldDebugGeometry>(*holding, commands, a.instance); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!ReleaseDebugGeometry::entered && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        CHECK(ReleaseDebugGeometry::entered);
        CHECK(!update(b, half, quad, half_list).type);
        // An empty PER_RUN batch replaces pending geometry. Busy on the second address must preserve the first.
        auto clear_both = take(FrameData::create<std::tuple<Resource, Resource>>({b, a}));
        CHECK(clear_both.seal());
        CHECK(call<UpdateFrameData>(*client, commands, {&clear_both}, false).error.type == kDeviceBusy);
        clear_both.release();
        draws[1] = check_runs(peer);
        binding.resources.front().resource = readback;
        call<RebindRenderJobEffect>(*client, commands, {session, first, effect, target, binding});
        CHECK(geometry(first) != a && update(a, full, triangle, full_list).type == kStorageIdentity);
        stopAndRemoveJob(*client, commands, first);
        stopAndRemoveJob(*client, commands, peer);
        stopAndRemoveJob(*client, commands, overlay);
        call<RemoveView>(*client, commands, {scene, other_view});
        CHECK(call<RemoveView>(*client, commands, {scene, view}, false).error.type == kDeviceBusy);
        call<ReleaseDebugGeometry>(*client, commands, {});
        CHECK(!held.get().error.type && update(a, full, triangle, full_list).type == kStorageIdentity);
        call<RemoveView>(*client, commands, {scene, view});
        server->stop();
        worker.join();
        CHECK(ran && server->stats().validation_errors == 0);
        server.reset();
        for (std::size_t job = 0; job < draws.size(); ++job)
        {
            const auto& result = ComputeCapture::results[job];
            CHECK(result.ready && result.frames.size() == 2);
            for (std::size_t slot = 0; slot < result.frames.size(); ++slot)
                for (std::uint32_t y = 0; y < height; ++y)
                    for (std::uint32_t x = 0; x < width; ++x)
                    {
                        const bool covered = slot == draws[job] && (job == 0 || (x < width / 2 && y >= 2));
                        const bool green = covered && job == 0 && x >= 3 * width / 4;
                        const std::array<std::uint8_t, 4> rgba{
                            std::uint8_t(covered && !green ? 255 : 0), std::uint8_t(green || (covered && job == 2) ? 255 : 0),
                            std::uint8_t(covered ? 0 : 255), 255
                        };
                        CHECK(std::memcmp(result.frames[slot].data() + y * result.pitch + x * 4, rgba.data(), 4) == 0);
                    }
        }
        std::puts("RESOURCE DRAW LIST PASS: two programs, scissor and empty runs; failed growth rolled back, one FIF grew and reused its backing");
        return 0;
    }
    if (particles)
    {
        using namespace sdk_particles;
        const auto effect = create_capture("raster-pixels");
        const auto job = call<CreateRenderJob>(*client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
        const ResourceRequest foreground{session, ViewScope{scene, view}, ParticleResource::descriptor.type, "foreground"};
        const ResourceRequest trail_input{session, ViewScope{scene, view}, ParticleResource::descriptor.type, "trail"};
        const auto before_start = server->stats().storages_created;
        CHECK(call<StartRenderJob>(*client, commands, {session, job, target}, false).error.type == kStorageIdentity);
        CHECK(server->stats().storages_created == before_start + 2);
        for (const auto& key : {foreground, trail_input})
            CHECK(call<QueryResource>(*client, commands, key, false).error.type == kStorageIdentity);
        JobBinding binding;
        binding.resources = {{"capture", readback}};
        call<StartRenderJob>(*client, commands, {session, job, target, std::move(binding)});
        CHECK(server->stats().storages_created == before_start + 4);
        const auto foreground_address = call<QueryResource>(*client, commands, foreground).address();
        const auto storage = foreground_address.instance;
        const auto trail = call<QueryResource>(*client, commands, trail_input).address();
        CHECK(storage != trail.instance && call<GetOrCreateResource>(*client, commands, foreground).resource_id == storage);
        constexpr std::array initial{
            ParticleUpdate{0, {{0, 0, 0, 1}, {1, 0, 0, 1}}},
            ParticleUpdate{1, {{0, 0, 0, 1}, {0, 1, 0, 1}}}
        };
        call<WriteParticles>(*client, commands, {trail.instance, std::span{initial}.last(1)});
        auto both = take(FrameData::create<std::tuple<ParticleResource, ParticleResource>>({foreground_address, trail}));
        const auto first_column = take(both.resolveColumn<ParticleResource, ParticleUpdate>(0));
        const auto second_column = take(both.resolveColumn<ParticleResource, ParticleUpdate>(1));
        take(both.appendData(first_column, 1)).front() = initial[0];
        auto invalid = take(both.appendData(second_column, 2));
        invalid[0] = {1, {{0, 0, 0, 1}, {0, 0, 1, 1}}};
        invalid[1] = {ParticleResource::row_count, {}};
        CHECK(both.seal());
        CHECK(call<UpdateFrameData>(*client, commands, {&both}, false).error.type == kStorageIdentity);
        both.release();
        // Readback must show the first batch's red and the second owner's pre-existing green.
        auto packet = take(FrameData::create<std::tuple<ParticleResource>>({trail}));
        renderFrame(*client, commands, job);
        const auto previous = call<RequestRenderJob>(*client, commands, {job});
        packet.reset();
        CHECK(packet.addData<ParticleResource>(ParticleUpdate{1, {{0, 0, 0, 1}, {0, 0, 1, 1}}}));
        CHECK(packet.seal());
        call<UpdateFrameData>(*client, commands, {&packet});
        packet.release();
        const auto current = call<RequestRenderJob>(*client, commands, {job});
        CHECK(previous.slot_index != current.slot_index);

        stopAndRemoveJob(*client, commands, job);
        call<RemoveView>(*client, commands, {scene, view});
        CHECK(call<WriteParticles>(*client, commands, {storage, initial}, false).error.type);
        server->stop();
        worker.join();
        CHECK(ran && server->stats().validation_errors == 0);
        server.reset();

        const auto& result = ComputeCapture::results.front();
        CHECK(result.ready && result.frames.size() == 2);
        // Two instances each initialize both rows in both FIF replicas, followed by one changed row.
        CHECK(ComputeCapture::captured_upload_bytes == 9 * sizeof(ParticleGpu));
        for (const auto slot : {previous.slot_index, current.slot_index})
        {
            const auto& frame = result.frames[slot];
            for (std::uint32_t y = 0; y < height; ++y)
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    const bool left = x < width / 2;
                    const std::array<std::uint8_t, 4> expected{
                        std::uint8_t(left ? 255 : 0),
                        std::uint8_t(!left && slot == previous.slot_index ? 255 : 0),
                        std::uint8_t(!left && slot == current.slot_index ? 255 : 0), 255
                    };
                    CHECK(std::memcmp(frame.data() + y * result.pitch + x * 4, expected.data(), 4) == 0);
                }
        }
        std::puts("PARTICLE STORAGE PASS: failed Start rolled back both named instances; independent GPU rows and both FIF replicas matched");
        return 0;
    }
    if (resident_programs)
    {
        const auto triangle = call<GetOrCreateResource>(*client, commands, {
            session, DeviceScope{}, sdk_programs::TriangleBatch::descriptor.type
        }).resource_id;
        const auto view_resource = [&](ViewId id)
        {
            return call<GetOrCreateResource>(*client, commands, {
                session, ViewScope{scene, id}, kViewParameterResourceName
            }).resource_id;
        };
        const auto camera_resource = view_resource(view);
        JobBinding binding;
        binding.resources = {
            {"capture", readback},
            {"triangle", triangle, 0, EJobResourceUse::DRAW_RESOURCE},
            {std::string{ViewParameterResource::snapshot_binding}, camera_resource, 0, EJobResourceUse::CPU_SNAPSHOT}
        };
        if (records)
        {
            const auto page = call<GetOrCreateResource>(*client, commands, {
                session, DeviceScope{}, sdk_programs::ColorPage::descriptor.type
            }, false);
            if (page.error.type == kDeviceUnsupported)
            {
                server->stop();
                worker.join();
                return 77;
            }
            CHECK(!page.error.type);
            binding.resources.push_back({"colors", page.resource_id, 0, EJobResourceUse::DRAW_RESOURCE});
        }
        using Probe = sdk_programs::NativeRasterProbe;
        const auto effect = call<CreateEffect>(*client, commands, capture_request("raster-pixels")).effect;
        CHECK(call<ListEffectJobs>(*client, commands, {effect}).jobs.empty());
        const auto prepare_programs = [&]() {
            if (records) return call<sdk_programs::PrepareRecordPrograms>(*client, commands, {triangle}, false).error;
            return call<PrepareEffect>(*client, commands, {effect, session}, false).error;
        };
        if (records) call<PrepareEffect>(*client, commands, {effect, session});
        Probe::watch_next = true;
        Probe::fail_after_prefix = true;
        CHECK(prepare_programs().type == kDeviceFailure);
        Probe::fail_after_prefix = false;
        CHECK(Probe::created == 1);
        Probe::waitForRetirement(1);
        Probe::watch_next = true;
        CHECK(!prepare_programs().type);
        CHECK(Probe::created == 4 && call<ListEffectJobs>(*client, commands, {effect}).jobs.empty());
        const auto primary = startRenderJob(*client, commands, session, scene, view, target, effect, binding);
        renderFrame(*client, commands, primary);
        CHECK(Probe::created == 4);
        const auto peer_view = call<CreateView>(*client, commands, {scene}).view;
        const auto peer_target = call<CreateTarget>(*client, commands, {session, {width, height, format}}).target;
        auto broken = binding;
        broken.resources.push_back({"clip_height", triangle, 0, EJobResourceUse::DRAW_RESOURCE});
        const auto mismatched = call<RebindRenderJobEffect>(
            *client, commands, {session, primary, effect, target, broken}, false
        );
        CHECK(mismatched.error.type == kStorageMismatch);
        auto peer_binding = binding;
        const auto peer_camera = std::ranges::find(
            peer_binding.resources, ViewParameterResource::snapshot_binding, &JobResourceInput::binding
        );
        CHECK(peer_camera != peer_binding.resources.end());
        set_viewport(peer_view, 2);
        peer_camera->resource = view_resource(peer_view);
        peer_binding.resources.front().resource = call<GetOrCreateResource>(*client, commands, {
            session, ViewScope{scene, peer_view}, ComputeCaptureDescriptor::descriptor.type
        }).resource_id;
        const auto mask = call<GetOrCreateResource>(*client, commands, {
            session, DeviceScope{}, sdk_programs::DrawMask::descriptor.type
        }).resource_id;
        peer_binding.resources.push_back({"clip_height", mask, 0, EJobResourceUse::DRAW_RESOURCE});
        const auto peer = startRenderJob(
            *client, commands, session, scene, peer_view, peer_target, effect, peer_binding
        );
        renderFrame(*client, commands, peer);
        call<RebindRenderJobEffect>(*client, commands, {session, primary, effect, target, binding});
        renderFrame(*client, commands, primary);
        call<sdk_programs::SetDrawOrder>(*client, commands, {triangle, {0}});
        ViewGpuParameters unrelated;
        unrelated.viewport = {1.0f / width, 0, 1.0f - 1.0f / width, 1};
        unrelated.background[0] = 0.5f;
        call<UpdateView>(*client, commands, {{scene, view}, unrelated});
        for (unsigned i = 0; i < 8; ++i)
        {
            call<QueryExecutionProgress>(*client, commands, primary);
            call<QueryExecutionProgress>(*client, commands, peer);
            renderFrame(*client, commands, primary);
            renderFrame(*client, commands, peer);
        }
        CHECK(Probe::created == 4);

        auto packet = framePacket<sdk_programs::TriangleBatch, ViewParameterResource>(*client, commands, session, primary);
        CHECK(packet.addData<sdk_programs::TriangleBatch>(sdk_programs::DrawOrder{1}));
        auto invalid_view = unrelated;
        invalid_view.direction_far[3] = 0;
        CHECK(packet.addData<ViewParameterResource>(invalid_view));
        CHECK(packet.seal());
        CHECK(call<UpdateFrameData>(*client, commands, {&packet}, false).error.type == kFrameContract);
        packet.release();
        renderFrame(*client, commands, primary);
        renderFrame(*client, commands, peer);
        CHECK(Probe::created == 4);
        stopAndRemoveJob(*client, commands, peer);
        call<RemoveView>(*client, commands, {scene, peer_view});
        call<sdk_programs::SetDrawOrder>(*client, commands, {triangle, {0}});
        renderFrame(*client, commands, primary);
        CHECK(Probe::created == 4);
        stopAndRemoveJob(*client, commands, primary);
        CHECK(Probe::candidate_destroys == 1);
        call<RemoveEffect>(*client, commands, {effect});
        if (records) call<sdk_programs::PrepareRecordPrograms>(*client, commands, {triangle, true});
        Probe::waitForRetirement(2);
        call<RemoveView>(*client, commands, {scene, view});
        server->stop();
        worker.join();
        CHECK(ran && server->stats().validation_errors == 0);
        server.reset();
        CHECK(ComputeCapture::created == 2);
        for (std::size_t i = 0; i < ComputeCapture::created; ++i)
        {
            const auto& result = ComputeCapture::results[i];
            CHECK(result.ready && result.frames.size() == 1);
            const auto& bytes = result.frames.front();
            for (std::uint32_t y = 0; y < height; ++y)
            {
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    const auto pixel = y * row_pitch + x * 4;
                    const std::array red{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
                    const std::array green{std::byte{0}, std::byte{255}, std::byte{0}, std::byte{255}};
                    // The peer draws the same full-width shader with destination-preserving blending.
                    const std::array blue{std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255}};
                    const std::array yellow{std::byte{255}, std::byte{255}, std::byte{0}, std::byte{255}};
                    const auto& base = records ? yellow : red;
                    const auto& foreground = x < width / 2 ? (i == 1 ? blue : base) : green;
                    const bool clipped = x < (i == 1 ? 2u : 1u) || (i == 1 && y >= height / 2);
                    const auto& expected = clipped ? blue : foreground;
                    if (std::memcmp(bytes.data() + pixel, expected.data(), expected.size()) != 0)
                    {
                        std::fprintf(stderr, "Raster output %zu pixel (%u,%u) = %u %u %u %u\n", i, x, y,
                            std::to_integer<unsigned>(bytes[pixel]), std::to_integer<unsigned>(bytes[pixel + 1]),
                            std::to_integer<unsigned>(bytes[pixel + 2]), std::to_integer<unsigned>(bytes[pixel + 3]));
                        CHECK(false);
                    }
                }
            }
        }
        if (records) std::puts("SDK RECORDS: yellow at byte 16, aligned and unaligned invalid ranges rejected, 4096 RGBA pixels");
        std::puts("SDK INDEXED GEOMETRY: 16-bit native indices, nonzero buffer offsets, copied vertex layout");
        std::puts("DRAW RESOURCE: optional SDK mask clips half the peer image; invalid type and foreign name rejected");
        std::puts("SDK COLD RASTER PASS: zero Jobs, failed native prefix retired, three pipelines from two shader programs prepared, zero draw recompiles");
        return 0;
    }
    if (receipts || product_fault)
    {
        const auto product = call<GetOrCreateResource>(
            *client, commands, {session, ViewScope{scene, view}, ProductDescriptor::descriptor.type}
        ).resource_id;
        JobBinding writer_binding;
        writer_binding.resources = {{"product", product}};
        const auto writer = startRenderJob(
            *client, commands, session, scene, view, target, create_capture("publish-7"), writer_binding
        );
        const auto first = call<RequestRenderJob>(*client, commands, {writer});
        const auto consumer_effect = create_capture("consume-version");
        const auto reader_target = call<CreateTarget>(*client, commands, {session, {width, height, format}}).target;
        JobBinding reader_binding;
        reader_binding.resources = {{"capture", readback}, {"version", product}};
        reader_binding.resources.back().publication = first.bufferVersion({8, 8});
        const auto reader = startRenderJob(
            *client, commands, session, scene, view, reader_target, consumer_effect, reader_binding
        );
        auto outside = reader_binding;
        outside.resources.back().publication = first.bufferVersion({4, 8});
        CHECK(call<RebindRenderJobEffect>(
            *client, commands, {session, reader, consumer_effect, reader_target, outside}, false
        ).error.type == kGraphInvalidRange);
        if (product_fault)
        {
            if (!ProductBuffer::live->canDelayRead())
            {
                server->stop();
                worker.join();
                return 77;
            }
            CreateEffectRequest request;
            request.graph = std::make_unique<EffectGraph>();
            EffectGraphEdit edit{*request.graph};
            const auto node = take(edit.addNode(kFaultCapability, {std::string{fault_node}, 1}));
            CHECK(edit.setOutput("color", edit.pinId(node, color), true));
            CHECK(edit.setOutput("pixels", edit.pinId(node, pixels)));
            CHECK(edit.commit());
            const auto fault_effect = createPreparedEffect(*client, commands, session, std::move(request));
            call<RebindRenderJobEffect>(*client, commands, {session, writer, fault_effect, target, writer_binding});

            ProductBuffer::countdown = 1;
            const auto rejected = call<RequestRenderJob>(*client, commands, {writer}, false);
            CHECK(rejected.error.type == kDeviceFailure && ProductBuffer::accepted_prefix == 0);
            const auto old_read = call<RequestRenderJob>(*client, commands, {reader});

            const bool unknown = mode == "receipt-unknown";
            ProductBuffer::failure = unknown ? nri::Result::FAILURE : nri::Result::INVALID_ARGUMENT;
            ProductBuffer::countdown = 2;
            const auto partial = call<RequestRenderJob>(*client, commands, {writer}, false);
            std::fprintf(stderr, "partial request error=%llu accepted=%u countdown=%u\n",
                static_cast<unsigned long long>(partial.error.type), ProductBuffer::accepted_prefix.load(),
                ProductBuffer::countdown.load());
            CHECK(partial.error.type == (unknown ? kOutcomeUnknown : kDeviceFailure));
            CHECK(ProductBuffer::accepted_prefix == 1 && partial.bufferVersion({8, 8}).writer.isNull());
            if (!unknown)
            {
                const auto stale = call<RequestRenderJob>(*client, commands, {reader}, false);
                CHECK(stale.error.type == kRuntimeIdentity);
                CHECK(call<RequestRenderJob>(*client, commands, {writer}, false).error.type == kDeviceFailure);
                call<RebindRenderJobEffect>(
                    *client, commands, {session, writer, create_capture("publish-11"), target, writer_binding}
                );
                const auto recovered = call<RequestRenderJob>(*client, commands, {writer});
                const auto next_view = call<CreateView>(*client, commands, {scene}).view;
                const auto capture_type = ComputeCaptureDescriptor::descriptor.type;
                reader_binding.resources.front().resource = call<GetOrCreateResource>(
                    *client, commands, {session, ViewScope{scene, next_view}, capture_type}
                ).resource_id;
                reader_binding.resources.back().publication = recovered.bufferVersion({8, 8});
                call<RebindRenderJobEffect>(
                    *client, commands, {session, reader, consumer_effect, reader_target, reader_binding}
                );
                call<RequestRenderJob>(*client, commands, {reader});
                server->stop();
            }
            worker.join();
            CHECK(unknown ? !ran && ran.error().type == kOutcomeUnknown : bool(ran));
            CHECK(server->stats().validation_errors == 0);
            server.reset();
            for (unsigned i = 0; i < (unknown ? 1u : 2u); ++i)
            {
                std::array<std::uint32_t, 2> words{};
                const auto& captured = ComputeCapture::results[i];
                CHECK(captured.ready == 1 && captured.frames.size() == 1 && old_read.slot_index == 0);
                std::memcpy(words.data(), captured.frames.front().data(), sizeof(words));
                CHECK(words[0] == (i ? 11u : 7u) && words[1] == words[0] + 1);
            }
            std::printf("BUFFER RECEIPT: zero-prefix old=7, accepted-prefix=1, %s\n",
                unknown ? "UNKNOWN retired" : "stale rejected, recovered=11");
            return 0;
        }
        call<RebindRenderJobEffect>(
            *client, commands, {session, writer, create_capture("publish-11"), target, writer_binding}
        );
        const bool delay = ProductBuffer::live->canDelayRead();
        ProductBuffer::gate_next_reader = delay;
        const auto read = call<RequestRenderJob>(*client, commands, {reader});
        CHECK(first.slot_index != read.slot_index);
        stopAndRemoveJob(*client, commands, reader);
        const auto second = reuseAfterReader(*client, commands, writer);
        CHECK(second.slot_index == first.slot_index && second.frame_index > first.frame_index);
        stopAndRemoveJob(*client, commands, writer);
        const auto candidate = call<CreateRenderJob>(
            *client, commands, {scene, view, create_capture("publish-7"), EJobCadence::REQUESTED}
        ).job;
        ProductBuffer::swap_candidate_slots = true;
        CHECK(call<StartRenderJob>(
            *client, commands, {session, candidate, target, writer_binding}, false
        ).error.type == kStorageMismatch);
        call<RemoveRenderJob>(*client, commands, candidate);
        const auto another = call<CreateRenderJob>(
            *client, commands, {scene, view, consumer_effect, EJobCadence::REQUESTED}
        ).job;
        CHECK(call<StartRenderJob>(
            *client, commands, {session, another, reader_target, reader_binding}, false
        ).error.type == kRuntimeIdentity);
        const auto peer_view = call<CreateView>(*client, commands, {scene}).view;
        reader_binding.resources.front().resource = call<GetOrCreateResource>(
            *client, commands, {session, ViewScope{scene, peer_view}, ComputeCaptureDescriptor::descriptor.type}
        ).resource_id;
        reader_binding.resources.back().publication = second.bufferVersion({8, 8});
        call<StartRenderJob>(*client, commands, {session, another, reader_target, reader_binding});
        const auto last = call<RequestRenderJob>(*client, commands, {another});
        reader_binding.resources.back().publication = second.bufferVersion({12, 4});
        call<RebindRenderJobEffect>(
            *client, commands, {session, another, create_capture("consume-word"), reader_target, reader_binding}
        );
        const auto subrange = call<RequestRenderJob>(*client, commands, {another});
        CHECK(subrange.slot_index != last.slot_index);
        stopAndRemoveJob(*client, commands, another);
        server->stop();
        worker.join();
        CHECK(ran && server->stats().validation_errors == 0);
        for (std::size_t i = 0; i < 2; ++i)
        {
            const auto& result = ComputeCapture::results[i];
            CHECK(result.ready == 1 && result.frames.size() == 2);
            std::array<std::uint32_t, 2> words{};
            std::memcpy(words.data(), result.frames[i ? last.slot_index : read.slot_index].data(), sizeof(words));
            CHECK(words[0] == (i ? 11u : 7u) && words[1] == words[0] + 1);
        }
        std::uint32_t last_word = 0;
        std::memcpy(&last_word, ComputeCapture::results[1].frames[subrange.slot_index].data(), sizeof(last_word));
        CHECK(last_word == 12);
        std::printf(
            "FINAL BUFFER VERSION: slot=%u old=7 next=11 subrange=12 stale-rejected, delayed-reader=%u\n",
            first.slot_index, unsigned(delay)
        );
        return delay ? 0 : 77;
    }
    const auto mutable_resource = call<GetOrCreateResource>(
        *client, commands, {session, ViewScope{scene, view}, MutableReplicaDescriptor::descriptor.type}
    ).resource_id;
    const auto created = create_capture({});
    JobBinding binding;
    constexpr std::uint32_t marker = 0x4d340123;
    if (argc > 2 && std::string_view{argv[2]} == "unknown")
    {
        const auto failed = call<InitializeReplica>(
            *client, commands, {mutable_resource, marker, InitializeReplica::EProbe::UNKNOWN}, false
        );
        CHECK(failed.error.type == kOutcomeUnknown && MutableReplica::injected == 1);
        worker.join();
        CHECK(!ran && ran.error().type == kOutcomeUnknown && server->stats().validation_errors == 0);
        server.reset();
        CHECK(MutableReplica::retired_candidates == 1);
        std::puts("RESOURCE UPLOAD PASS: UNKNOWN stopped device admission and retired the candidate");
        return 0;
    }
    auto other = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(other);
    auto cancelled = std::async(std::launch::async, [&]
    {
        return call<InitializeReplica>(
            *other, commands, {mutable_resource, marker, InitializeReplica::EProbe::CANCEL}, false
        );
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!MutableReplica::held.load())
    {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    const auto busy = call<InitializeReplica>(*client, commands, {mutable_resource, marker}, false);
    MutableReplica::resume.store(true);
    MutableReplica::resume.notify_all();
    CHECK(busy.error.type == kDeviceBusy && cancelled.get().error.type == kCommandCancelled);
    call<InitializeReplica>(*client, commands, {mutable_resource, marker});
    binding.resources = {{"capture", readback}, {"a_mutable", mutable_resource}};
    CHECK(MutableReplica::notified == 0);
    const auto job = startRenderJob(*client, commands, session, scene, view, target, created, binding);
    const auto peer_target = call<CreateTarget>(*client, commands, {session, {width, height, format}}).target;
    const auto peer = call<CreateRenderJob>(*client, commands, {scene, view, created, EJobCadence::REQUESTED}).job;
    CHECK(call<StartRenderJob>(*client, commands, {session, peer, peer_target, binding}, false).error.type == kJobBusy);
    const auto peer_view = call<CreateView>(*client, commands, {scene}).view;
    const auto peer_capture = call<GetOrCreateResource>(
        *client, commands, {session, ViewScope{scene, peer_view}, ComputeCaptureDescriptor::descriptor.type}
    ).resource_id;
    auto peer_binding = binding;
    peer_binding.resources.front().resource = peer_capture;
    const auto compiled_before_peer = call<QueryRenderStats>(*client, commands, {}).compiled_graphs;
    call<StartRenderJob>(*client, commands, {session, peer, peer_target, peer_binding});
    CHECK(call<QueryRenderStats>(*client, commands, {}).compiled_graphs == compiled_before_peer);
    MutableReplica::omit_upper_range = true;
    const auto missing_bytes = call<RebindRenderJobEffect>(
        *client, commands, {session, peer, created, peer_target, peer_binding}, false
    );
    CHECK(missing_bytes.error.type == kStorageMismatch);
    MutableReplica::omit_upper_range = false;
    CHECK(call<QueryRenderStats>(*client, commands, {}).compiled_graphs == compiled_before_peer);
    CHECK(call<RebindRenderJobEffect>(
        *client, commands, {session, peer, created, peer_target, binding}, false
    ).error.type == kJobBusy);
    const auto replace = [&](std::uint32_t value, std::size_t expected)
    {
        call<InitializeReplica>(*client, commands, {mutable_resource, value, InitializeReplica::EProbe::REPLACE});
        CHECK(MutableReplica::notified == expected);
    };
    replace(marker + 1, 2);
    const auto before_rollback = MutableReplica::resolutions.load();
    ComputeCapture::reject_resolve = true;
    const auto rollback = call<RebindRenderJobEffect>(*client, commands, {session, job, created, target, binding}, false);
    CHECK(rollback.error.type == kStorageMismatch && !ComputeCapture::reject_resolve);
    CHECK(MutableReplica::resolutions == before_rollback + 1);
    replace(marker + 2, 2);
    call<RebindRenderJobEffect>(*client, commands, {session, job, created, target, binding});
    replace(marker + 3, 2);
    auto late_failure = binding;
    late_failure.inputs.conditions.push_back(1);
    const auto invalid = call<RebindRenderJobEffect>(
        *client, commands, {session, job, created, target, std::move(late_failure)}, false
    );
    CHECK(invalid.error.type == kDeviceArgument);
    replace(marker + 4, 2);
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
    stopAndRemoveJob(*client, commands, job);

    const auto successor_view = call<CreateView>(*client, commands, {scene}).view;
    const auto successor_capture = call<GetOrCreateResource>(*client, commands, {
        session, ViewScope{scene, successor_view}, ComputeCaptureDescriptor::descriptor.type
    }).resource_id;
    const auto successor_source = call<GetOrCreateResource>(*client, commands, {
        session, ViewScope{scene, successor_view}, MutableReplicaDescriptor::descriptor.type
    }).resource_id;
    constexpr std::uint32_t successor_marker = 0x7286cc31u;
    call<InitializeReplica>(*client, commands, {successor_source, 0});
    auto successor_binding = binding;
    successor_binding.resources = {{"capture", successor_capture}, {"a_mutable", successor_source}};
    const auto successor = startRenderJob(
        *client, commands, session, scene, successor_view, target, create_capture("computed"), successor_binding
    );
    CHECK(successor.index == job.index && successor.gen != job.gen);
    call<InitializeReplica>(*client, commands, {successor_source, successor_marker, InitializeReplica::EProbe::REPLACE});
    CHECK(MutableReplica::notified == 1);
    replace(marker + 5, 1);
    renderFrame(*client, commands, successor);
    stopAndRemoveJob(*client, commands, successor);
    renderFrame(*client, commands, peer);
    stopAndRemoveJob(*client, commands, peer);
    replace(marker + 6, 0);
    JobBinding range_binding;
    range_binding.resources = {{"capture", peer_capture}};
    const auto lower = startRenderJob(
        *client, commands, session, scene, view, target, create_capture("lower"), range_binding
    );
    const auto candidate = [&](std::string mode)
    {
        const auto effect = create_capture(std::move(mode));
        return call<CreateRenderJob>(*client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
    };
    const auto spill = call<CreateEffect>(*client, commands, capture_request("spill")).effect;
    CHECK(call<PrepareEffect>(*client, commands, {spill, session}, false).error.type == kGraphInvalidRange);
    call<RemoveEffect>(*client, commands, {spill});
    const auto rival = candidate("overlap");
    const StartRenderJob::Request conflict{session, rival, peer_target, range_binding};
    CHECK(call<StartRenderJob>(*client, commands, conflict, false).error.type == kJobBusy);
    call<RemoveRenderJob>(*client, commands, {rival});
    const auto upper = candidate("upper");
    const auto unauthorized = candidate("unauthorized");
    auto shared_binding = range_binding;
    shared_binding.resources.front().resource = mutable_resource;
    CHECK(call<StartRenderJob>(
        *client, commands, {session, unauthorized, peer_target, shared_binding}, false
    ).error.type == kStorageMismatch);
    call<RemoveRenderJob>(*client, commands, {unauthorized});
    call<StartRenderJob>(*client, commands, {session, upper, peer_target, range_binding});
    const auto [a, b] = request_pair(upper, lower);
    CHECK(!a.error.type && !b.error.type && a.frame_index == b.frame_index && a.slot_index == b.slot_index);
    stopAndRemoveJob(*client, commands, upper);
    stopAndRemoveJob(*client, commands, lower);
    const auto product = call<GetOrCreateResource>(
        *client, commands, {session, ViewScope{scene, view}, ProductDescriptor::descriptor.type}
    ).resource_id;
    JobBinding product_binding;
    product_binding.resources = {{"product", product}};
    const auto changing_low = candidate("publish-low");
    const auto changing_high = candidate("publish-7");
    call<StartRenderJob>(*client, commands, {session, changing_low, target, product_binding});
    call<StartRenderJob>(*client, commands, {session, changing_high, peer_target, product_binding});
    const auto [c, d] = request_pair(changing_low, changing_high);
    CHECK(!c.error.type && !d.error.type && c.frame_index != d.frame_index && c.slot_index == d.slot_index);
    stopAndRemoveJob(*client, commands, changing_high);
    stopAndRemoveJob(*client, commands, changing_low);
    std::printf("DISJOINT BUFFER BOUNDARY: compatible=%llu,%llu transitioning=%llu,%llu slot=%u\n",
        static_cast<unsigned long long>(a.frame_index), static_cast<unsigned long long>(b.frame_index),
        static_cast<unsigned long long>(c.frame_index), static_cast<unsigned long long>(d.frame_index), c.slot_index);
    server->stop();
    worker.join();
    CHECK(ran && server->stats().validation_errors == 0);
    const auto& peer_pixels = ComputeCapture::results[1];
    CHECK(peer_pixels.ready == 1 && peer_pixels.frames.size() == 1);
    CHECK(peer_pixels.frames.front().size() >= capture_bytes + 16384u);
    const auto outside_graph = std::span{peer_pixels.frames.front()}.subspan(capture_bytes);
    CHECK(std::ranges::all_of(outside_graph, [](auto byte) { return byte == std::byte{}; }));
    std::uint32_t peer_marker = 0;
    const auto* peer_replica = peer_pixels.frames.front().data() + readback_bytes;
    std::memcpy(&peer_marker, peer_replica + sizeof(peer_marker), sizeof(peer_marker));
    CHECK(peer_marker == marker + 5);
    const auto& successor_pixels = ComputeCapture::results[2];
    CHECK(successor_pixels.ready == 1 && successor_pixels.frames.size() == 1);
    const auto& successor_frame = successor_pixels.frames.front();
    CHECK(successor_frame.size() >= readback_bytes + 2 * sizeof(std::uint32_t));
    std::uint32_t observed_successor = 0;
    std::memcpy(&observed_successor, successor_frame.data() + readback_bytes + sizeof(peer_marker), sizeof(peer_marker));
    CHECK(observed_successor == successor_marker);
    for (std::size_t y = 0; y < height; ++y)
    {
        for (std::size_t x = 0; x < width; ++x)
        {
            const auto red = successor_frame[y * successor_pixels.pitch + x * 4];
            CHECK(red == std::byte{127} || red == std::byte{128});
        }
    }
    std::puts("INLINE RESULTS: CPU reader and GPU inline block agree on computed red across all pixels");
    std::printf("PROJECTION READER PASS: Job slot %u, generations %u/%u, peer=%08x successor=%08x\n",
        job.index, job.gen, successor.gen, peer_marker, observed_successor);
    // The later range writers replace the first 16 bytes. The next pixel still belongs to the peer Job.
    const auto peer_pixel = std::span{peer_pixels.frames.front()}.subspan(16, 4);
    CHECK(peer_pixel[0] == std::byte{64} && (peer_pixel[1] == std::byte{127} || peer_pixel[1] == std::byte{128}));
    CHECK(peer_pixel[2] == std::byte{191} && peer_pixel[3] == std::byte{255});
    const auto claimed = std::span{peer_pixels.frames.front()}.first(16);
    CHECK(std::ranges::all_of(claimed.first(8), [](auto byte) { return byte == std::byte{0x5a}; }));
    CHECK(std::ranges::all_of(claimed.last(8), [](auto byte) { return byte == std::byte{0x6b}; }));
    std::puts("FINAL BUFFER PASS: shared input, failed writer Rebind, exclusive byte ranges and 0x5a/0x6b readback");
    const auto& readback_pixels = ComputeCapture::results[0];
    CHECK(readback_pixels.ready == 1 && readback_pixels.frames.size() == 1);
    constexpr std::size_t pixel_size = 4;
    const auto data = std::span(readback_pixels.frames.front());
    CHECK(data.size() >= capture_bytes);
    std::uint32_t observed_marker = 0;
    std::memcpy(&observed_marker, data.data() + readback_bytes + sizeof(std::uint32_t), sizeof(observed_marker));
    CHECK(observed_marker == marker + 4);
    std::printf("SDK REBIND ROLLBACK: two installed readers after early and late failures, marker=%08x\n",
        observed_marker);
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
