#include "GraphFill.hpp"
#include "RasterProgramFixture.hpp"
#include "SceneCapture.hpp"
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/extensions/effect/EffectBuildContext.hpp>
#include <lux/engine/render/extensions/effect/EffectImplementation.hpp>
#include <lux/engine/render/extensions/effect/EffectInitializationContext.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/resources/view/Resource.hpp>
#include <cstring>
#include <future>

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

namespace
{
    struct OutputState final
    {
        OutputConstants previous{0.75f}; // A wrong projection produces a different red channel.
        OutputConstants constants{0.25f};
    };
    namespace graph = lux::graph;

    // The deferred Editor consumer initializes the first slot positionally.
    static_assert(requires(StorageBindingOps::Resolve resolver) { StorageBindingOps{resolver}; });

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
        using Ops = void;
        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.sdk.compute_readback"}, 0, 0, {}, "Compute output readback.", EResourceDomain::VIEW
        };
    };

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
            live = nullptr;
            const_cast<nri::CoreInterface&>(device.nriCore()).QueueSubmit = submit;
            device.deferRelease([core = &device.nriCore(), owned = std::move(buffers), fence = gate]() noexcept
            {
                for (auto* buffer : owned) core->DestroyBuffer(buffer);
                core->DestroyFence(fence);
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

    struct MutableReplica;
    MutableReplica* selected_replica = nullptr;
    void checkSelectedBacking(ResourceInstanceId other, const ResourceInput& backing) noexcept;

    struct ComputeCapture final
    {
        using Descriptor = ComputeCaptureDescriptor;
        static inline std::atomic_bool reject_resolve{};
        static inline std::array<CapturedPixels, 2> results;
        static inline std::size_t created = 0;
        static inline std::atomic_uint accepted{}, rejected{};
        static constexpr std::uint64_t preparation_revision = 1;
        CaptureBuffer buffer;
        CapturedPixels& result;
        std::uint32_t capacity;
        std::uint64_t epoch = 0;

        ResourceInstanceId identity;

        ComputeCapture(ResourceCreateContext& context, CapturedPixels& pixels, ResourceInstanceId id) noexcept
            : buffer(context), result(pixels), capacity(capture_bytes + (&pixels == &results[1] ? 16384u : 0u)), identity(id)
        {
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
                    -> RenderResult<StorageGraphImport>
                {
                    if (projection != 0) return lux::cxx::unexpected(RenderError{kStorageContract});
                    const auto capacity = static_cast<const ComputeCapture*>(owner)->capacity;
                    return StorageGraphImport::bufferOutput(capacity, EGraphUse::TRANSFER_DESTINATION);
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
            return makeResourceRegistration<Descriptor, ComputeCapture, &create>(nullptr, &binding);
        }

        static RenderResult<std::unique_ptr<ComputeCapture>>
        create(ResourceCreateContext& context, ResourceInstanceId id) noexcept
        {
            CHECK(created < results.size());
            return std::make_unique<ComputeCapture>(context, results[created++], id);
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
        using Ops = void;
        using Commands = std::tuple<SetReplicaValue, InitializeReplica>;
        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.sdk.mutable_replica"}, 0, 0, {}, "Mutable replica.", EResourceDomain::VIEW,
            resourceCommandDescriptors(static_cast<Commands*>(nullptr))
        };
    };

    struct MutableReplica final
    {
        static inline std::atomic_bool omit_upper_range{};
        using Descriptor = MutableReplicaDescriptor;
        static inline std::atomic_uint accepted_copies{}, accepted_no_copy{}, rejected{};
        DeviceSession& device;
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
            : device(context.device), registry(context.resources), identity(id) {}

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
        initialize(MutableReplica& storage, ReplicaMutation& mutation, InitializeReplica::Request&& request) noexcept
        {
            if (storage.buffer && request.probe != InitializeReplica::EProbe::REPLACE) co_return AckResponse{RenderError{kStorageContract}};
            const auto premature = co_await mutation.writeBuffers({});
            CHECK(!premature && premature.error().type == kFrameContract);
            const auto selected = co_await mutation.select();
            if (!selected) co_return AckResponse{selected.error()};
            CHECK(selected->slot == 0 && selected->epoch);
            const auto repeated = co_await mutation.select();
            CHECK(repeated && repeated->slot == selected->slot && repeated->epoch == selected->epoch);
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
            const auto result = co_await mutation.writeBuffers({&write, 1});
            if (request.probe == InitializeReplica::EProbe::UNKNOWN) hooks.QueueSubmit = submit;
            if (!result)
            {
                storage.device.deferRelease([destroy = core.DestroyBuffer, candidate]() noexcept
                {
                    destroy(candidate);
                    ++retired_candidates;
                });
                co_return AckResponse{result.error()};
            }
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
                    StorageGraphImport schema{
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
            static const std::array commands{
                CommandBinding::declare<SetReplicaValue, &set>(),
                CommandBinding::declare<InitializeReplica, &initialize>()
            };
            return makeResourceRegistration<Descriptor, MutableReplica, &create>(nullptr, &bindings, commands);
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

    struct NonExclusiveReplicaCommand final
    {
        static constexpr std::string_view name = "test.sdk.nonexclusive_replica";
        using Request = InitializeReplica::Request;
        using Data = Request;
        using Response = AckResponse;
        [[nodiscard]] static ResourceInstanceId resourceId(const Request& request) noexcept { return request.resource; }
    };

    static_assert(CommandMethodFor<InitializeReplica, &MutableReplica::initialize>);
    static_assert(!CommandMethodFor<NonExclusiveReplicaCommand, &MutableReplica::initialize>);

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
        work.interface.parameters.setInlineConstant(OutputConstants{0});
        work.interface.resources = {{"generated", {{generated, 0}, {}, EGraphAccess::WRITE}}};
        work.dispatch = ImageDispatchArguments{generated};
        const auto dispatch = take(builder.addComputePass("generate compute color", std::move(work)));
        if (auto linked = context.copyInlineBlock<OutputConstants, OutputState, &OutputState::constants>(dispatch.pass);
            !linked) return linked;
        const auto copied = take(builder.addTransferPass(
            "copy compute color", TransferWork{{CopyImage{{dispatch.written.front()}, {{output, 0}}}}}
        ));
        return context.publish(color, {{color, copied.written.front()}});
    }

    RenderResult<void> buildResidentRaster(EffectBuildContext& context, std::string_view mode) noexcept
    {
        using namespace sdk_programs;
        const auto type = ResourceName{CodeStorage::descriptor.type.name()};
        const auto code = take(context.importStorageSnapshot<CodeSnapshot>("shader_code", {type}));
        const auto alias = take(context.importStorageSnapshot<CodeSnapshot>("shader_alias", {type}));
        auto& builder = context.builder();
        auto image = take(builder.declareResource({"raster output", GraphImageDesc{width, height}}));
        RasterPassTemplate background_stage;
        background_stage.purpose = "independent clear";
        background_stage.runtime_stream_programs = true;
        RasterWork background;
        background.program = std::move(background_stage);
        background.colors.push_back({{{image, 0}}, EAttachmentLoad::CLEAR, EAttachmentStore::STORE, {0, 0, 1, 1}});
        const auto base = take(builder.addRasterPass("clear background", std::move(background)));
        auto clear = context.addPreparation({
            ProducerFactory::make<ClearBackground>(), take(ConfigValue::make(base.pass)), {}, {}, {}, {base.pass},
            {}, {}, {}, RasterProgramBuilder::make<ClearBackground, &ClearBackground::buildPrograms>()
        });
        if (!clear) return clear;
        RasterPassTemplate stage;
        stage.purpose = "installed programs";
        stage.runtime_stream_programs = true;
        RasterWork raster;
        raster.program = std::move(stage);
        const auto camera = take(context.importStorageSnapshot<ViewGpuParameters>(
            ViewParameterResource::snapshot_binding, {ResourceName{kViewParameterResourceName.name()}},
            ESnapshotScope::JOB_LOCAL
        ));
        raster.colors.push_back({{base.written.front()}, EAttachmentLoad::LOAD});
        auto draw = take(builder.addRasterPass("two programs", std::move(raster)));
        auto configuration = take(ConfigValue::make(ProgramDraws::Configuration{
            draw.pass, alias, camera, context.shaderTarget(), mode == "stale-code"
        }));
        auto program_builder = RasterProgramBuilder::make<ProgramDraws, &ProgramDraws::buildPrograms>();
        if (mode == "no-build") program_builder = {};
        if (mode == "wrong-inputs")
            program_builder = RasterProgramBuilder::make<WrongProgramInputs, &WrongProgramInputs::build>();
        if (mode == "missing-map")
            program_builder = RasterProgramBuilder::make<MissingDrawMapping, &MissingDrawMapping::build>();
        auto added = context.addPreparation({
            ProducerFactory::make<ProgramDraws>(), std::move(configuration), {}, {}, {},
            {draw.pass}, {}, {code, alias, camera}, {code, alias}, program_builder
        });
        if (!added) return added;
        return context.publish(color, {{color, draw.written.front()}});
    }

    RenderResult<void> buildCaptureRange(EffectBuildContext& context, std::string_view mode) noexcept
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
            "capture", {ResourceName{type.name()}}, std::move(declaration)
        );
        if (!output) return lux::cxx::unexpected(output.error());
        auto& builder = context.builder();
        const auto scratch = take(builder.declareResource({"pattern", GraphBufferDesc{claim.size}}));
        const auto pattern = offset == 0 ? 0x5a5a5a5au : 0x6b6b6b6bu;
        const auto fill = take(builder.addTransferPass("fill", TransferWork{{FillBuffer{{{scratch, 0}}, pattern}}}));
        const GraphBufferRange destination{mode == "spill" ? 4u : offset, claim.size};
        const auto copy = builder.addTransferPass("copy", TransferWork{{
            CopyBuffer{{fill.written.front()}, {*output, destination}}
        }});
        if (!copy) return lux::cxx::unexpected(copy.error());
        return context.publish(pixels, {{pixels, copy->written.front()}});
    }

    RenderResult<void> buildCapture(EffectBuildContext& context, std::string_view mode) noexcept
    {
        auto& builder = context.builder();
        if (mode == "publish-7" || mode == "publish-11")
        {
            constexpr GraphBufferRange range{8, 8};
            const auto dest = take(context.importFinalBuffer(
                "product", {ResourceName{ProductDescriptor::descriptor.type.name()}},
                {
                    "", GraphBufferDesc{ProductBuffer::bytes}, EGraphOrigin::IMPORTED, {},
                    EGraphUse::TRANSFER_SOURCE, range
                }
            ));
            const auto tmp = take(builder.declareResource({"sample", GraphBufferDesc{range.size}}));
            const auto word = mode == "publish-7" ? 7u : 11u;
            const auto fill = take(builder.addTransferPass("sample", TransferWork{{
                FillBuffer{{{tmp, 0}, GraphBufferRange{0, 4}}, word},
                FillBuffer{{{tmp, 0}, GraphBufferRange{4, 4}}, word + 1}
            }}));
            const auto copy = take(builder.addTransferPass(
                "publish", TransferWork{{CopyBuffer{{fill.written.front()}, {dest, range}}}}
            ));
            return context.publish(pixels, {{pixels, copy.written.front()}});
        }
        if (mode == "consume-version" || mode == "consume-word")
        {
            const GraphBufferRange bytes{0, mode == "consume-word" ? 4u : 8u};
            const auto source = context.importResource(
                "version", {ResourceName{ProductDescriptor::descriptor.type.name()}},
                {"", GraphBufferDesc{bytes.size}, EGraphOrigin::IMPORTED, {bytes}, EGraphUse::TRANSFER_SOURCE}
            );
            if (!source) return lux::cxx::unexpected(source.error());
            const auto dest = take(context.importFinalBuffer(
                "capture", {ResourceName{ComputeCaptureDescriptor::descriptor.type.name()}},
                {"", GraphBufferDesc{capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION, bytes}
            ));
            const auto copy = take(builder.addTransferPass(
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
            "capture", {ResourceName{ComputeCaptureDescriptor::descriptor.type.name()}},
            {"", GraphBufferDesc{capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION, claim}
        ));
        const ImageCopyRegion region{0, 0, width, height};
        const auto copied = take(context.builder().addTransferPass("copy to readback", TransferWork{{
            CopyImageBuffer{{input.front().front().version}, {output}, row_pitch, readback_bytes, region}
        }}));
        if (raster_only) return context.publish(pixels, {{pixels, copied.written.front()}});
        const auto mutable_input = take(context.importResource(
            "a_mutable", {ResourceName{MutableReplicaDescriptor::descriptor.type.name()}},
            {
                "", GraphBufferDesc{replica_bytes}, EGraphOrigin::IMPORTED,
                {GraphBufferRange{0, replica_bytes}}, EGraphUse::TRANSFER_SOURCE
            }
        ));
        const auto complete = take(context.builder().addTransferPass("copy mutable replica", TransferWork{{
            CopyBuffer{{mutable_input}, {copied.written.front(), GraphBufferRange{readback_bytes, replica_bytes}}}
        }}));
        return context.publish(pixels, {{pixels, complete.written.front()}});
    }

    constexpr std::string_view fault_node = "test.sdk.partial_output";

    JobSubmitted reuseAfterReader(ipc::IpcRequester& client, const CommandList& commands, RenderJobId writer)
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

    CapabilityDefinition faultDefinition() noexcept
    {
        return {
            .identity = {graph::nodeTypeId(fault_node), std::string{fault_node}, 1},
            .ports = {
                {color, "color", {"test.sdk.compute_rgba8", 1}, graph::EPinDirection::OUTPUT},
                {pixels, "pixels", {"test.sdk.readback_bytes", 1}, graph::EPinDirection::OUTPUT}
            }
        };
    }

    RenderResult<void> buildPartialOutput(EffectBuildContext& context, std::string_view) noexcept
    {
        auto& builder = context.builder();
        constexpr GraphBufferRange range{8, 8};
        const auto dest = take(context.importFinalBuffer(
            "product", {ResourceName{ProductDescriptor::descriptor.type.name()}},
            {"", GraphBufferDesc{ProductBuffer::bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_SOURCE, range}
        ));
        const auto write = take(builder.addTransferPass(
            "write product", TransferWork{{FillBuffer{{dest, range}, 11}}}, {.estimated_gpu_ns = 1000}
        ));
        const auto image = take(builder.declareResource({"async result", GraphImageDesc{width, height}}));
        ComputeWork work;
        work.allow_async_compute = true;
        work.interface.program = take(builder.addProgram(GraphProgram{kGraphFill.load(context.shaderTarget())}));
        work.interface.parameters.setInlineConstant(OutputConstants{0.25f});
        work.interface.resources = {{"generated", {{image, 0}, {}, EGraphAccess::WRITE}}};
        work.dispatch = ImageDispatchArguments{image};
        const auto async = take(builder.addComputePass(
            "async work", std::move(work), {.side_effect = true, .estimated_gpu_ns = 100000}
        ));
        const auto output = take(builder.declareResource({"graphics color", GraphImageDesc{width, height}}));
        const auto graphics = take(builder.addTransferPass(
            "graphics work", TransferWork{{ClearImage{{{output, 0}}}}}, {.estimated_gpu_ns = 100000}
        ));
        CHECK(builder.dependsOn(write.pass, async.pass));
        CHECK(builder.dependsOn(write.pass, graphics.pass));
        if (auto published = context.publish(color, {{color, graphics.written.front()}}); !published) return published;
        return context.publish(pixels, {{pixels, write.written.front()}});
    }

    template <EffectImplementation::Build Build>
    void registerNode(RenderServer& server, const CapabilityDefinition& capability, graph::PinSemanticId output)
    {
        CHECK(server.registerEffectCapability(capability));
        EffectImplementation implementation;
        implementation.identity = {capability.identity.canonical_name, capability.identity.version};
        implementation.capability = capability.identity.id;
        implementation.supported_outputs = {output};
        if constexpr (Build == buildPartialOutput) implementation.supported_outputs.push_back(color);
        implementation.initialize = [](EffectInitializationContext& context, std::string_view arguments) noexcept
            -> RenderResult<void>
        {
            if constexpr (Build == buildCompute)
            {
                if (!arguments.empty()) return lux::cxx::unexpected(RenderError{kInvalidConfig});
                return context.initializeState<OutputState>();
            }
            if constexpr (Build == buildResidentRaster)
            {
                const bool valid = arguments.empty() || arguments == "stale-code" || arguments == "no-build" ||
                    arguments == "wrong-inputs" || arguments == "missing-map";
                if (valid) return {};
                return lux::cxx::unexpected(RenderError{kInvalidConfig});
            }
            const bool known = arguments.empty() || arguments == "lower" || arguments == "upper" ||
                arguments == "overlap" || arguments == "spill" || arguments == "unauthorized" ||
                arguments == "publish-7" || arguments == "publish-11" ||
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
    const bool resident_programs = mode == "resident-programs";
    const bool receipts = mode == "receipts";
    const bool product_fault = mode == "receipt-partial" || mode == "receipt-unknown";
    config.frames_in_flight = receipts ? 2 : 1;
    config.uploads.emplace();
    DeviceOptions options;
    options.multiple_queues = receipts || product_fault;
    if (product_fault) options.graph_queue_costs = {100, 100, 100};
    if (d3d12) options.backend = EGraphicsBackend::D3D12;
    auto server = take(RenderServer::create(config, **execution));
    if (resident_programs)
    {
        options.cached_unused_programs = 0;
        take(server->registerResource<sdk_programs::CodeStorage>());
        const std::array commands{
            CommandBinding::declare<sdk_programs::ReleaseCode, &sdk_programs::CodeStorage::release>()
        };
        CHECK(server->registerCommands(commands));
        registerNode<buildResidentRaster>(*server, computeDefinition(), color);
    }
    else registerNode<buildCompute>(*server, computeDefinition(), color);
    registerNode<buildCapture>(*server, captureDefinition(), pixels);
    take(server->registerResource<ComputeCapture>());
    take(server->registerResource<MutableReplica>());
    if (receipts || product_fault) take(server->registerResource<ProductBuffer>());
    if (product_fault) registerNode<buildPartialOutput>(*server, faultDefinition(), pixels);
    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    RenderResult<void> ran;
    std::thread worker([&] { ran = server->run(); });
    const auto commands = queryCommands(*client);
    const auto session = call<CreateDeviceSession>(*client, commands, options).device;
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto view = call<CreateView>(*client, commands, {scene}).view;
    const auto format = rdesc::ETextureFormat::RGBA8_UNORM;
    const auto target = call<CreateTarget>(*client, commands, {session, {width, height, format}}).target;
    const auto readback = call<GetOrCreateResource>(
        *client, commands, {session, ViewRequest{scene, view}, ComputeCaptureDescriptor::descriptor.type}
    ).resource_id;
    const auto capture_request = [&](std::string mode)
    {
        CreateEffectRequest effect;
        effect.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*effect.graph};
        std::string source_mode;
        if (resident_programs && mode != "raster-pixels") source_mode = std::exchange(mode, "raster-pixels");
        const auto compute = take(edit.addNode(
            computeDefinition(), {std::string{compute_node}, 1}, std::move(source_mode)
        ));
        const auto capture = take(edit.addNode(captureDefinition(), {std::string{capture_node}, 1}, std::move(mode)));
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
    if (resident_programs)
    {
        const auto code = call<GetOrCreateResource>(*client, commands, {
            session, DeviceScope{}, sdk_programs::CodeStorage::descriptor.type
        }).resource_id;
        const auto camera_for = [&](ViewId owner, unsigned inset)
        {
            const auto camera = call<GetOrCreateResource>(*client, commands, {
                session, ViewRequest{scene, owner}, kViewParameterResourceName
            }).resource_id;
            ViewGpuParameters parameters;
            const auto left = static_cast<float>(inset) / width;
            parameters.viewport = {left, 0, 1 - left, 1};
            call<ViewParameterCommands::Update>(*client, commands, {camera, std::span{&parameters, 1}});
            return camera;
        };
        const auto camera_resource = camera_for(view, 1);
        JobBinding binding;
        binding.resources = {
            {"capture", readback}, {"shader_code", code, 0, EJobResourceUse::CPU_SNAPSHOT},
            {"shader_alias", code, 0, EJobResourceUse::CPU_SNAPSHOT},
            {std::string{ViewParameterResource::snapshot_binding}, camera_resource, 0, EJobResourceUse::CPU_SNAPSHOT}
        };
        for (auto mode : {"no-build", "wrong-inputs", "missing-map"})
        {
            const auto effect = call<CreateEffect>(*client, commands, capture_request(mode)).effect;
            const auto rejected = call<PrepareEffect>(*client, commands, {effect, session}, false);
            CHECK(rejected.error.type == kGraphInvalidDescriptor);
            call<RemoveEffect>(*client, commands, {effect});
        }
        std::puts("SDK COLD LINK: missing builder, wrong linked inputs and wrong draw mapping rejected at Prepare");
        using Store = sdk_programs::CodeStorage;
        using Gate = sdk_programs::EDriverGate;
        auto frames = ipc::IpcCore::instance().createRequester(config.channel_name);
        auto mutations = ipc::IpcCore::instance().createRequester(config.channel_name);
        CHECK(frames && mutations);
        const auto wait = [](auto predicate)
        {
            const auto deadline = Clock::now() + 10s;
            while (!predicate())
            {
                CHECK(Clock::now() < deadline);
                std::this_thread::yield();
            }
        };
        const auto invalid = create_capture("stale-code");
        const auto stale = startRenderJob(*client, commands, session, scene, view, target, invalid, binding);
        CHECK(call<RequestRenderJob>(*client, commands, {stale}, false).error.type == kStorageIdentity);
        const auto failed_builds = Store::builds.load();
        CHECK(call<QueryExecutionProgress>(*client, commands, stale).last_run_error.type == kStorageIdentity);
        auto locked = std::async(std::launch::async, [&]
        {
            return call<sdk_programs::HoldCode>(*mutations, commands, code);
        });
        wait([] { return Store::held.load(); });
        CHECK(!call<QueryExecutionProgress>(*client, commands, stale).last_run_error.type);
        auto retry = std::async(std::launch::async, [&]
        {
            return call<RequestRenderJob>(*frames, commands, {stale}, false);
        });
        CHECK(retry.wait_for(20ms) == std::future_status::timeout);
        call<sdk_programs::ReleaseCode>(*client, commands, {});
        CHECK(!locked.get().error.type && retry.get().error.type == kStorageIdentity);
        CHECK(Store::builds == failed_builds);
        stopAndRemoveJob(*client, commands, stale);
        const auto effect = create_capture("raster-pixels");
        const auto job = startRenderJob(*client, commands, session, scene, view, target, effect, binding);
        Store::gate = Gate::ARMED;
        auto requested = std::async(std::launch::async, [&]
        {
            return call<RequestRenderJob>(*frames, commands, {job});
        });
        wait([] { return Store::gate == Gate::HELD; });
        auto holding = std::async(std::launch::async, [&]
        {
            return call<sdk_programs::HoldCode>(*mutations, commands, code);
        });
        wait([] { return Store::held.load(); });
        const auto busy_layout = call<QueryRenderJobInputLayout>(*client, commands, job).layout;
        auto busy_packet = take(FrameData::create<std::tuple<Store, ViewParameterResource>>(busy_layout));
        CHECK(busy_packet.addData<Store>(sdk_programs::DrawOrder{1}));
        CHECK(busy_packet.seal());
        CHECK(call<UpdateFrameData>(*client, commands, {job, &busy_packet}, false).error.type == kDeviceBusy);
        busy_packet.release();
        Store::gate = Gate::OPEN;
        Store::gate.notify_all();
        wait([] { return Store::created.load() == 2; });
        CHECK(requested.wait_for(20ms) == std::future_status::timeout);
        call<sdk_programs::ReleaseCode>(*client, commands, {});
        CHECK(!holding.get().error.type && !requested.get().error.type);
        stopAndRemoveJob(*client, commands, job);
        const auto restarted = startRenderJob(*client, commands, session, scene, view, target, effect, binding);
        renderFrame(*client, commands, restarted);
        CHECK(Store::created.load() == 2);
        const auto peer_view = call<CreateView>(*client, commands, {scene}).view;
        const auto peer_target = call<CreateTarget>(*client, commands, {session, {width, height, format}}).target;
        auto peer_binding = binding;
        peer_binding.resources.back().resource = camera_for(peer_view, 2);
        peer_binding.resources.front().resource = call<GetOrCreateResource>(*client, commands, {
            session, ViewRequest{scene, peer_view}, ComputeCaptureDescriptor::descriptor.type
        }).resource_id;
        const auto links_before = Store::source_links.load();
        const auto peer = startRenderJob(
            *client, commands, session, scene, peer_view, peer_target, effect, peer_binding
        );
        renderFrame(*client, commands, peer);
        CHECK(Store::source_links == links_before + 1);
        const auto before_rebind = Store::builds.load();
        call<RebindRenderJobEffect>(*client, commands, {session, restarted, effect, target, binding});
        renderFrame(*client, commands, restarted);
        CHECK(Store::source_links == links_before + 2 && Store::builds == before_rebind + 1);

        const auto stable_builds = Store::builds.load();
        const auto stable_clear_builds = Store::clear_builds.load();
        const auto stable_links = Store::source_links.load();
        const auto stable_captures = Store::captures.load();
        call<sdk_programs::SetDrawOrder>(*client, commands, {code, {0}});
        ViewGpuParameters unrelated;
        unrelated.viewport = {1.0f / width, 0, 1.0f - 1.0f / width, 1};
        unrelated.background[0] = 0.5f;
        call<ViewParameterCommands::Update>(*client, commands, {camera_resource, std::span{&unrelated, 1}});
        for (unsigned i = 0; i < 8; ++i)
        {
            call<QueryExecutionProgress>(*client, commands, restarted);
            call<QueryExecutionProgress>(*client, commands, peer);
            renderFrame(*client, commands, restarted);
            renderFrame(*client, commands, peer);
        }
        CHECK(Store::captures == stable_captures + 16 && Store::builds == stable_builds);
        CHECK(Store::source_links == stable_links && Store::created == 2);

        const auto schema = call<QueryRenderJobInputLayout>(*client, commands, restarted).layout;
        auto packet = take(FrameData::create<std::tuple<Store, ViewParameterResource>>(schema));
        CHECK(packet.addData<Store>(sdk_programs::DrawOrder{1}));
        auto invalid_view = unrelated;
        invalid_view.direction_far[3] = 0;
        CHECK(packet.addData<ViewParameterResource>(invalid_view));
        CHECK(packet.seal());
        CHECK(call<UpdateFrameData>(*client, commands, {restarted, &packet}, false).error.type == kFrameContract);
        packet.release();
        renderFrame(*client, commands, restarted);
        renderFrame(*client, commands, peer);
        CHECK(Store::builds == stable_builds + 2 && Store::created == 2);
        stopAndRemoveJob(*client, commands, peer);
        call<RemoveView>(*client, commands, {scene, peer_view});
        call<sdk_programs::SetDrawOrder>(*client, commands, {code, {0}});
        renderFrame(*client, commands, restarted);
        CHECK(Store::builds == stable_builds + 3 && Store::source_links == stable_links);
        CHECK(Store::clear_builds == stable_clear_builds && Store::created == 2);
        stopAndRemoveJob(*client, commands, restarted);
        call<RemoveView>(*client, commands, {scene, view});
        server->stop();
        worker.join();
        CHECK(ran && server->stats().validation_errors == 0);
        server.reset();
        for (std::size_t i = 0; i < ComputeCapture::results.size(); ++i)
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
                    // Green covers the right half. Red covers both halves when drawn last for the peer.
                    const std::array blue{std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255}};
                    const auto& foreground = (i == 1 || x < width / 2) ? red : green;
                    const auto& expected = x < (i == 1 ? 2u : 1u) ? blue : foreground;
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
        std::puts("SDK RESOURCE PROGRAMS: stale item rejected, exclusive publication deferred, two resident PSOs");
        std::puts("PROGRAM INPUTS: 16 stable frames and progress queries, 16 frame captures, zero relinks or builds; "
                  "partial FrameData rebuilt both readers, after Stop only one, "
                  "zero independent clear rebuilds, 4096 RGBA pixels");
        return 0;
    }
    if (receipts || product_fault)
    {
        const auto product = call<GetOrCreateResource>(
            *client, commands, {session, ViewRequest{scene, view}, ProductDescriptor::descriptor.type}
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
            const auto node = take(edit.addNode(faultDefinition(), {std::string{fault_node}, 1}));
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
                    *client, commands, {session, ViewRequest{scene, next_view}, capture_type}
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
        CHECK(call<SetRenderJobDependencies>(
            *client, commands, {writer, {reader}, {}}, false
        ).error.type == kJobCycle);
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
            *client, commands, {session, ViewRequest{scene, peer_view}, ComputeCaptureDescriptor::descriptor.type}
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
        *client, commands, {session, ViewRequest{scene, view}, MutableReplicaDescriptor::descriptor.type}
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
        std::puts("REPLICA COMMAND PASS: UNKNOWN stopped device admission and retired the candidate");
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
        *client, commands, {session, ViewRequest{scene, peer_view}, ComputeCaptureDescriptor::descriptor.type}
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
    replace(marker + 5, 1);
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
    renderFrame(*client, commands, upper);
    renderFrame(*client, commands, lower);
    stopAndRemoveJob(*client, commands, upper);
    stopAndRemoveJob(*client, commands, lower);
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
