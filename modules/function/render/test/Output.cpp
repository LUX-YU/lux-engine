#include "FixtureEffect.hpp"
#include <lux/engine/render/graph/PlanDiagnostics.hpp>
#include <lux/engine/render/resource/instance/Requirement.hpp>
#include <lux/engine/render/runtime/commands/CommandBinding.hpp>
#include <lux/engine/render/device/SubmissionQueue.hpp>
#include <lux/engine/render/device/ResourceUploader.hpp>
#include "FixtureCommands.hpp"
#include <lux/engine/render/runtime/commands/InstanceCommands.hpp>
#include <lux/engine/render/runtime/commands/MaterialCommands.hpp>
#include <lux/engine/render/runtime/commands/MeshCommands.hpp>
#include <lux/engine/render/runtime/commands/TextureCommands.hpp>
#include <lux/engine/render/runtime/commands/TransformCommands.hpp>
#include <lux/engine/render/runtime/commands/ViewCommands.hpp>
#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/engine/render/resource/RasterProgramSet.hpp>
#include <lux/engine/render/device/JobGpuResources.hpp>
#include "NativeDevice.hpp"
#include "SceneCapture.hpp"
#include "TargetDrawList.hpp"
#include "EffectGraphColor.hpp"
#include "SceneViewColor.hpp"
#include <lux/engine/render/effect/capabilities/DepthReduction.hpp>
#include <lux/engine/render/effect/capabilities/DeferredLighting.hpp>
#include <lux/engine/render/runtime/commands/LightingCommands.hpp>
#include <lux/engine/render/effect/capabilities/ToneMapping.hpp>
#include "EffectGraphImport.hpp"
#include "EffectGraphProducer.hpp"
#include "EffectNodeUniform.hpp"
#include "EffectPreparedBuffer.hpp"
#include "EffectStreamIndirect.hpp"
#include "EffectStreamInline.hpp"
#include "EffectStreamTarget.hpp"
#include "EffectStreamLayers.hpp"
#include "EffectStreamMaterial.hpp"
#include "EffectStreamMesh.hpp"
#include "EffectRecordTable.hpp"
#include "EffectRecordOnly.hpp"
#include "EffectStreamSharedTexture.hpp"
#include "EffectStreamUniform.hpp"
#include "EffectStreamVertex.hpp"
#include <lux/engine/render/frame/FrameData.hpp>
#include <lux/engine/render/resource/lighting/Resource.hpp>
#include <lux/engine/render/resource/instance/Resource.hpp>
#include <lux/engine/render/resource/material/Resource.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/render/resource/mesh/Resource.hpp>
#include <lux/engine/render/resource/texture/Resource.hpp>
#include <lux/engine/render/resource/transform/TransformRecords.hpp>
#include <lux/engine/render/resource/transform/Resource.hpp>
#include <lux/engine/render/resource/view/Resource.hpp>
#ifdef _WIN32
#include "RetirementFence.hpp"
#pragma comment(lib, "user32.lib")
#endif
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <future>
#include <limits>
#include <lux/engine/render/target/OffscreenTarget.hpp>
#include <lux/engine/render/graph/execute/RasterDraws.hpp>
#include <lux/engine/render/graph/execute/ResizeResources.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include "FrameHarness.hpp"
#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <lux/engine/render/effect/EffectImplementation.hpp>
#include <lux/engine/render/graph/compile/GraphDefinition.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/graph/compile/RenderGraphCompiler.hpp>
#include <optional>
#include <string>
#include <string_view>
using namespace foundation_test;
using lux::render::runtime_detail::JobExecution;

struct ProducerConstants final
{
    std::uint32_t value;
};

struct alignas(16) TestUniformBlock final
{
    std::array<std::uint32_t, 4> words;
};

struct TestSharedTextureIndices final
{
    std::uint32_t image_index, sampler_index;
};

template <> struct lux::render::ConstantBlockTraits<TestSharedTextureIndices> final
{
    static ConstantBlockLayout layout(lux::rdesc::EShaderTarget target) noexcept
    {
        constexpr auto scalar = lux::rdesc::EDataScalar::UINT;
        static constexpr std::array members{
            ConstantMemberLayout{"image_index", 0, scalar},
            ConstantMemberLayout{"sampler_index", 4, scalar}
        };
        const auto alignment = target == lux::rdesc::EShaderTarget::DXIL ? 16u : 4u;
        return {sizeof(TestSharedTextureIndices), alignment, members};
    }
};

template <> struct lux::render::ConstantBlockTraits<lux::rdesc::MaterialGpuReference> final
{
    static ConstantBlockLayout layout(lux::rdesc::EShaderTarget target) noexcept
    {
        constexpr auto scalar = lux::rdesc::EDataScalar::UINT;
        static constexpr std::array members{
            ConstantMemberLayout{"buffer_index", 0, scalar},
            ConstantMemberLayout{"record_index", 4, scalar}
        };
        const auto alignment = target == lux::rdesc::EShaderTarget::DXIL ? 16u : 4u;
        return {sizeof(lux::rdesc::MaterialGpuReference), alignment, members};
    }
};

template <> struct lux::render::ConstantBlockTraits<TestUniformBlock> final
{
    static ConstantBlockLayout layout(lux::rdesc::EShaderTarget) noexcept
    {
        static constexpr std::array fields{ConstantMemberLayout{"words", 0, lux::rdesc::EDataScalar::UINT, 4}};
        return {sizeof(TestUniformBlock), alignof(TestUniformBlock), fields};
    }
};

struct ProducerSourceConfig final
{
    std::shared_ptr<const int> lifetime;
};

template <> struct lux::render::ConfigValueTraits<ProducerSourceConfig> final
{
    static RenderResult<std::size_t> retainedBytes(const ProducerSourceConfig&) noexcept
    {
        return sizeof(ProducerSourceConfig) + sizeof(int);
    }
};

template <> struct lux::render::ConstantBlockTraits<ProducerConstants> final
{
    static ConstantBlockLayout layout(lux::rdesc::EShaderTarget target) noexcept
    {
        static constexpr std::array members{ConstantMemberLayout{"value", 0, lux::rdesc::EDataScalar::UINT}};
        const auto alignment = target == lux::rdesc::EShaderTarget::DXIL ? 16u : 4u;
        return {sizeof(ProducerConstants), alignment, members};
    }
};

static void copyBuffer(DeviceSession& session, nri::Buffer& source, nri::Buffer& output, std::uint64_t bytes)
{
    CHECK(submitFixtureCommands(session, session.queue(),
        [&](nri::CommandBuffer& command, std::uint32_t) noexcept -> RenderResult<void>
        {
            const nri::BufferBarrierDesc source_ready{
                &source,
                {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY},
                {nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY}
            };
            session.nriCore().CmdBarrier(command, {nullptr, 0, &source_ready, 1});
            session.nriCore().CmdCopyBuffer(command, output, 0, source, 0, bytes);
            return {};
        }
    ));
}

static void graphOutput(Validation& validation)
{
    TestDevice session({}, diagnostics(validation), nativeValidationEnabled());
    auto frames = std::make_unique<FrameHarness>(*session, 3);
    const TargetDesc target{16, 16, lux::rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    GraphDraftBuilder builder;
    const auto color = take(builder.declareResource({"color", GraphImageDesc{16, 16}}));
    const auto pixels = take(builder.declareResource({"pixels", GraphBufferDesc{4096}}));
    const auto copied = take(builder.declareResource({"copied", GraphBufferDesc{4096}}));
    auto clear = take(builder.addTransferPass("clear", TransferWork{{ClearImage{{{color, 0}}, {1, 0, 1, 1}}}}));
    const auto initialized =
        take(builder.addTransferPass("initialize copy padding", TransferWork{{FillBuffer{{{pixels, 0}}, 0xababababu}}})
        );
    auto copy = take(builder.addTransferPass(
        "copy",
        TransferWork{{CopyImageBuffer{
            {clear.written.front()},
            {initialized.written.front(), {}, EGraphAccess::READ_WRITE},
            256,
            4096
        }}}
    ));
    auto copied_pass =
        take(builder.addTransferPass("copy-buffer", TransferWork{{CopyBuffer{{copy.written.front()}, {{copied, 0}}}}}));
    CHECK(builder.exportOutput({clear.written.front(), {}, GraphOutputRequirements{target.format, target.samples}}));
    CHECK(builder.exportOutput({copy.written.front()}));
    CHECK(builder.exportOutput({copied_pass.written.front()}));
    auto definition = take(std::move(builder).finish());
    auto analysis = take(analyzeRenderGraph(definition));
    const GraphOutputRequirements output_description{target.format, target.samples};
    auto ready_result = stdexec::sync_wait(RenderGraphCompiler::compileNative(
        *session, lux::execution::ExecutionRuntime::instance().cpuScheduler(),
        {definition, analysis}, {.target = output_description}
    ));
    CHECK(ready_result);
    auto graph = take(std::move(std::get<0>(*ready_result)));
    auto physical = std::optional{take(OffscreenTarget::create(*session, target, frames->frameCount()))};
    auto execution_gpu = std::make_unique<JobGpuResources>(*session, frames->frameCount());
    auto execution_job = std::make_unique<JobExecution>(
        *execution_gpu, graph, testInstallation(), physical->output()
    );
    CHECK(execution_job->initialize());
    auto incompatible_output = physical->output();
    incompatible_output.description.format = lux::rdesc::ETextureFormat::BGRA8_UNORM;
    auto rejected_resize = ResizeResources::prepare(
        *execution_job, incompatible_output, {}
    );
    CHECK(!rejected_resize && rejected_resize.error().type == kDeviceArgument);
    auto missing_output = ResizeResources::prepare(*execution_job, {});
    CHECK(!missing_output && missing_output.error().type == kDeviceArgument);
    CHECK(execution_job->targetDescription() == target); // Rejection must not withdraw the adopted output.
    const std::array contexts{execution_job.get()};
    nri::Buffer* output = nullptr;
    const nri::BufferDesc description{4096, 0, nri::BufferUsageBits::NONE};
    CHECK(
        session->nriCore().CreatePlacedBuffer(session->device(), NriHostReadbackHeap, description, output) ==
        nri::Result::SUCCESS
    );
    CHECK(take(runTask(frames->render(contexts, {}))) == 1);
    CHECK(take(runTask(frames->render(contexts, {}))) == 1);
    CHECK(take(runTask(frames->render(contexts, {}))) == 1);
    CHECK(take(runTask(frames->render(contexts, {}))) == 0);
    CHECK(session->drain());
    for (auto id : {pixels, copied})
    {
        copyBuffer(
            *session,
            *execution_job->resourceInput(id, execution_job->progress.last_submitted_slot).buffer,
            *output,
            4096
        );
        CHECK(session->drain());
        auto* mapped = static_cast<const std::byte*>(session->nriCore().MapBuffer(*output, 0, 4096));
        CHECK(mapped);
        CHECK(session->invalidateMappedRange(*output, 0, 4096) == nri::Result::SUCCESS);
        for (std::size_t row = 0; row < 16; ++row)
        {
            for (std::size_t column = 0; column < 16; ++column)
            {
                const auto i = row * 256 + column * 4;
                CHECK(mapped[i] == std::byte{255} && mapped[i + 1] == std::byte{0});
                CHECK(mapped[i + 2] == std::byte{255} && mapped[i + 3] == std::byte{255});
            }
        }
        session->nriCore().UnmapBuffer(*output);
    }
    auto prior_uses = session->makeFrontiers();
    physical->appendUses(prior_uses);
    CHECK(take(runTask(frames->render(contexts, {}))) == 1);
    auto last_uses = session->makeFrontiers();
    physical->appendUses(last_uses);
    CHECK(last_uses != prior_uses);
    execution_job.reset();
    execution_gpu.reset(); // Physical resources outlive the business Context through Session cleanup.
    physical.reset();
    CHECK(session->drain());
    session->nriCore().DestroyBuffer(output);
    std::puts("GPU RETIREMENT PASS: readback pixels and Offscreen release use accepted frontiers");
}

static void importedIdentityReuse(Validation& validation, EGraphicsBackend backend)
{
    std::vector<lux::render::detail::RasterDrawInput> freeze_draws;
    DeviceOptions options;
    options.backend = backend;
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    auto frames = std::make_unique<FrameHarness>(*session, 1);
    GraphDraftBuilder builder;
    const auto color = take(builder.declareResource({"color", GraphImageDesc{16, 16}}));
    RasterWork work;
    work.program = RasterPassTemplate{"identity regression"};
    work.colors = {{{{color, 0}}}};
    const auto pass = take(builder.addRasterPass("clear with dynamic reads", std::move(work)));
    CHECK(builder.exportOutput({pass.written.front()}));
    auto graph = take(std::move(builder).finish());
    auto analysis = take(analyzeRenderGraph(graph));
    GraphCompileDiagnostics report;
    auto ready_result = stdexec::sync_wait(RenderGraphCompiler::compileNative(
        *session, lux::execution::ExecutionRuntime::instance().cpuScheduler(), {graph, analysis}, {.diagnostics = &report}
    ));
    CHECK(ready_result);
    PreparedRenderGraph plan{take(std::move(std::get<0>(*ready_result))), std::move(report)};
    std::string dump;
    const DumpSink sink{&dump, [](void* destination, std::string_view text) noexcept
    {
        static_cast<std::string*>(destination)->append(text);
        return true;
    }};
    const auto number = [&dump](std::string_view key)
    {
        const auto found = dump.find(key);
        CHECK(found != std::string::npos);
        std::uint64_t value = 0;
        CHECK(std::from_chars(dump.data() + found + key.size(), dump.data() + dump.size(), value).ec ==
              std::errc{});
        return value;
    };
    const auto rebuilds = [&]()
    {
        dump.clear();
        CHECK(frames->dumpSubmissions(sink));
        return number("\"dependency_rebuilds\":");
    };
    std::optional<JobGpuResources> instance_gpu;
    std::optional<JobExecution> instance_job;
    const auto create = [&]() -> JobExecution*
    {
        auto& gpu = instance_gpu.emplace(*session, 1);
        return &instance_job.emplace(gpu, plan.graph, testInstallation(), TargetOutput{});
    };
    auto* instance = create();
    CHECK(instance_job->initialize());
    CHECK(session->drain());
    nri::Buffer* buffer = nullptr;
    CHECK(session->nriCore().CreatePlacedBuffer(
        session->device(), NriDeviceHeap, {32, 4, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE}, buffer
    ) == nri::Result::SUCCESS);
    const auto render = [&]()
    {
        const std::array instances{&*instance_job};
        CHECK(take(runTask(frames->render(instances, {}))) == 1);
        CHECK(!frames->submitting());
        CHECK(session->drain());
    };
    // Changing the selected draw ranges preserves the logical dependency cache.
    const auto raster_slot = plan.diagnostics.passSlot(pass.pass);
    CHECK(raster_slot < plan.graph.passes().size());
    const auto& raster_pass = plan.graph.passes()[raster_slot];
    const auto change_reads = [&](std::uint32_t update)
    {
        auto& draws = rasterDraws(*instance, raster_pass, 0);
        draws.reads = {{buffer, GraphBufferRange{16 * (update % 2), 16}, EGraphUse::STORAGE_READ, 1}};
    };
    change_reads(0);
    render();
    const auto initial_rebuilds = rebuilds();
    change_reads(1);
    render();
    CHECK(rebuilds() == initial_rebuilds);
    CHECK(frames->completed(0, *instance_job));
    const auto cached_rebuilds = rebuilds();
    auto stale_resize = take(ResizeResources::prepare(*instance_job, {}));
    CHECK(stale_resize.prepareAdoption(*instance_job));
    const auto* gpu_address = &*instance_gpu;
    const auto* address = &*instance;
    instance_job.reset();
    instance_gpu.reset();
    CHECK(session->drain());
    instance = create();
    CHECK(&*instance == address && &*instance_gpu == gpu_address);
    const auto stale_adoption = stale_resize.prepareAdoption(*instance_job);
    CHECK(!stale_adoption && stale_adoption.error().type == kFrameContract);
    CHECK(instance_job->initialize());
    CHECK(!frames->completed(0, *instance_job));
    change_reads(0);
    CHECK(take(instance_job->freezeFrame(0, freeze_draws)));
    change_reads(1);
    render();
    CHECK(rebuilds() == cached_rebuilds + 1);
    CHECK(frames->completed(0, *instance_job));
    const auto adopted_rebuilds = rebuilds();
    render();
    CHECK(rebuilds() == adopted_rebuilds); // Stable inputs still reuse the dependency cache.
    instance_job.reset();
    instance_gpu.reset();
    CHECK(session->drain());
    session->nriCore().DestroyBuffer(buffer);
    std::printf("RESIZE GENERATION PASS backend=%s: prepared candidate rejected after CPU and GPU row address reuse\n",
        backend == EGraphicsBackend::D3D12 ? "D3D12" : "Vulkan");
    std::printf("EXECUTION IDENTITY PASS backend=%s: old receipt excluded at same address, "
                "draw-range changes reused dependencies, new generation rebuilt once\n",
                backend == EGraphicsBackend::D3D12 ? "D3D12" : "Vulkan");
}

static void deviceLostDelivery(Validation& validation, bool enable_validation = nativeValidationEnabled())
{
    TestDevice session({}, diagnostics(validation), enable_validation);
    auto& core = const_cast<nri::CoreInterface&>(session->nriCore());
    nri::Buffer* buffer = nullptr;
    const nri::BufferDesc description{16, 0, nri::BufferUsageBits::NONE};
    CHECK(core.CreatePlacedBuffer(session->device(), NriHostReadbackHeap, description, buffer) == nri::Result::SUCCESS);
    auto& queue = session->queue();
    const auto record = [](nri::CommandBuffer&, std::uint32_t) noexcept -> RenderResult<void> { return {}; };
    CHECK(submitFixtureCommands(*session, queue, record));
    CHECK(session->drain()); // No live GPU access is exposed to a simulated loss on healthy hardware.
    RenderError delivered;
    SubmissionCallback callback;
    queue.arm(queue.submitted(), callback, [](void* context, RenderResult<void> result) noexcept
    {
        CHECK(!result);
        *static_cast<RenderError*>(context) = result.error();
    }, &delivered);
    const auto submit = core.QueueSubmit;
    core.QueueSubmit = [](nri::Queue&, const nri::QueueSubmitDesc&) noexcept -> nri::Result
    { return nri::Result::DEVICE_LOST; };
    const auto rejected = submitFixtureCommands(*session, queue, record);
    core.QueueSubmit = submit;
    CHECK(!rejected && session->deviceLost());
    CHECK(rejected.error().args[0] == static_cast<std::uint32_t>(nri::Result::DEVICE_LOST));
    bool released = false;
    auto frontier = session->makeFrontiers();
    frontier.accept(session->submissionPoint(session->queueIndex(queue), queue.submitted()));
    session->deferRelease(std::move(frontier), [&]() noexcept
    {
        core.DestroyBuffer(buffer);
        released = true;
    });
    const auto progressed = session->progressCompletions();
    CHECK(!progressed && progressed.error() == rejected.error());
    CHECK(delivered == rejected.error() && released && !callback.armed());
    session.reset();
    std::puts(
        "DEVICE LOST contract PASS: terminal delivered, cleanup settled; injected only after real GPU completion"
    );
}

static void importedExtentContracts(Validation& validation)
{
    TestDevice session({}, diagnostics(validation), nativeValidationEnabled());
    const auto& core = session->nriCore();
    for (const bool resolve : {false, true})
    {
        GraphDraftBuilder builder;
        GraphImageDesc image{0, 0, 1, 1, lux::rdesc::ETextureFormat::RGBA8_UNORM};
        image.extent.source = EImageExtentSource::IMPORTED;
        image.samples = resolve ? 4 : 1;
        const auto source = take(builder.declareResource({
            "source", image, EGraphOrigin::IMPORTED, {{}},
            resolve ? EGraphUse::COLOR_ATTACHMENT : EGraphUse::TRANSFER_SOURCE
        }));
        image.samples = 1;
        const auto destination = take(builder.declareResource({"destination", image, EGraphOrigin::IMPORTED}));
        if (resolve)
        {
            RasterWork work;
            work.program = RasterPassTemplate{"resolve extent"};
            RasterAttachment color{{{source, 0}}};
            color.resolve = ResourceAccess{{destination, 0}};
            work.colors.push_back(color);
            const auto pass = take(builder.addRasterPass("resolve", std::move(work)));
            CHECK(builder.exportOutput({pass.written.back()}));
        }
        else
        {
            const auto pass = take(builder.addTransferPass(
                "copy", TransferWork{{lux::render::CopyImage{{{source, 0}}, {{destination, 0}}}}}
            ));
            CHECK(builder.exportOutput({pass.written.front()}));
        }
        auto graph = take(std::move(builder).finish());
        auto analysis = take(analyzeRenderGraph(graph));
        auto ready_result = stdexec::sync_wait(RenderGraphCompiler::compileNative(
            *session, lux::execution::ExecutionRuntime::instance().cpuScheduler(), {graph, analysis}
        ));
        CHECK(ready_result);
        auto plan = take(std::move(std::get<0>(*ready_result)));
        std::array<nri::Texture*, 3> images{};
        for (std::size_t i = 0; i < images.size(); ++i)
        {
            nri::TextureDesc description{};
            description.type = nri::TextureType::TEXTURE_2D;
            description.format = nri::Format::RGBA8_UNORM;
            description.width = description.height = i == 1 ? 8 : 16;
            description.depth = description.mipNum = description.layerNum = 1;
            description.sampleNum = resolve && i == 0 ? 4 : 1;
            description.usage = nri::TextureUsageBits::COLOR_ATTACHMENT;
            CHECK(core.CreatePlacedTexture(session->device(), NriDeviceHeap, description, images[i]) ==
                  nri::Result::SUCCESS);
        }
        {
            JobGpuResources invalid_gpu(*session, 1);
            JobExecution invalid_job{invalid_gpu, plan, testInstallation(), TargetOutput{}};
            invalid_job.bindImage(source, 0, *images[0]);
            invalid_job.bindImage(destination, 0, *images[1]);
            const auto rejected = invalid_job.initialize();
            CHECK(!rejected && rejected.error().type == kInvalidTargetExtent);
        }
        {
            JobGpuResources instance_gpu(*session, 1);
            JobExecution instance_job{instance_gpu, plan, testInstallation(), TargetOutput{}};
            instance_job.bindImage(source, 0, *images[0]);
            instance_job.bindImage(destination, 0, *images[2]);
            CHECK(instance_job.initialize());
            const std::array<TargetFrame, 1> smaller_frames{{{images[1]}}};
            const ImageReplacement replacement{destination, smaller_frames};
            const auto rejected = ResizeResources::prepare(
                instance_job, {}, {&replacement, 1}
            );
            CHECK(!rejected && rejected.error().type == kInvalidTargetExtent);
            // A graph with only imported resources legitimately accepts an empty output.
            const std::array<TargetFrame, 1> original{{{images[2]}}};
            const ImageReplacement valid{destination, original};
            const std::array repeated{valid, valid};
            const auto duplicate = ResizeResources::prepare(instance_job, {}, repeated);
            CHECK(!duplicate && duplicate.error().type == kFrameContract);
            const ImageReplacement missing_frames{destination, {}};
            const auto malformed = ResizeResources::prepare(instance_job, {}, {&missing_frames, 1});
            CHECK(!malformed && malformed.error().type == kFrameContract);
            CHECK(instance_job.resourceInput(destination, 0).image == images[2]);
            auto unchanged = take(ResizeResources::prepare(
                instance_job, {}, {&valid, 1}
            ));
            CHECK(unchanged.prepareAdoption(instance_job));
            std::move(unchanged).commit(instance_job);
            const auto reused = unchanged.prepareAdoption(instance_job);
            CHECK(!reused && reused.error().type == kFrameContract);
        }
        CHECK(session->drain());
        for (auto* texture : images) core.DestroyTexture(texture);
    }
}

namespace
{
    namespace graph = lux::graph;

    constexpr std::string_view display_clear_name = "test.output.clear";
    constexpr std::string_view hdr_source_name = "test.output.hdr";
    constexpr std::string_view hdr_import_name = "test.output.import_hdr";
    constexpr std::string_view hdr_binding = "author.hdr";

    EffectCapability outputSource(bool hdr)
    {
        const auto name = hdr ? hdr_source_name : display_clear_name;
        static constexpr std::array hdr_ports{outputPort("color", {HdrColorBundle::type, 1})};
        static constexpr std::array display_ports{ToneMappingPorts::color};
        return {.id = graph::nodeTypeId(name), .canonical_name = name, .ports = hdr ? hdr_ports : display_ports};
    }

    template <bool Hdr>
    RenderResult<void> buildOutputClear(EffectCompileContext& context, std::string_view) noexcept
    {
        const auto format = Hdr ? rdesc::ETextureFormat::RGBA16_SFLOAT : context.target().format;
        GraphImageDesc image{0, 0, 1, 1, format};
        image.extent.source = EImageExtentSource::TARGET;
        const auto color = take(context.declareResource({"output clear", image}));
        const auto clear = take(context.addTransferPass(
            "clear output", TransferWork{{ClearImage{{{color, 0}}, {.25f, .5f, .75f, 1}}}}
        ));
        return context.publish(ToneMappingPorts::color.semantic, {{ToneMappingPorts::color.semantic, clear.written.front()}});
    }

    RenderResult<void> buildOutputImport(EffectCompileContext& context, std::string_view) noexcept
    {
        const auto image = context.importTargetImage(hdr_binding, rdesc::ETextureFormat::RGBA16_SFLOAT);
        if (!image) return lux::cxx::unexpected(image.error());
        return context.publish(ToneMappingPorts::color.semantic, {{ToneMappingPorts::color.semantic, *image}});
    }

    RenderResult<void> buildViewColor(EffectCompileContext& context, std::string_view) noexcept
    {
        const auto row = take(context.importResource(
            ViewParameterResource::gpu_binding, kViewParameterResourceName, ViewParameterResource::projection()
        ));
        GraphImageDesc image;
        image.extent.source = EImageExtentSource::TARGET;
        const auto source = take(context.declareResource({"view color", image}));
        const auto color = take(context.declareResource({"output", image}));
        ComputeWork work;
        work.interface.program = take(context.addProgram(GraphProgram{kSceneViewColor.load(context.shaderTarget())}));
        work.interface.resources = {{"scene_view", {row}}, {"generated", {{source, 0}, {}, EGraphAccess::WRITE}}};
        work.dispatch = ImageDispatchArguments{source};
        const auto drawn = take(context.addComputePass("read view row", std::move(work)));
        const auto copied = take(context.addTransferPass(
            "copy view color", TransferWork{{lux::render::CopyImage{{drawn.written.front()}, {{color, 0}}}}}
        ));
        return context.publish(ToneMappingPorts::color.semantic, {{ToneMappingPorts::color.semantic, copied.written.front()}});
    }

    void registerOutputNodes(RenderServer& server)
    {
        const auto add = [&server](
            const EffectCapability& definition, std::string_view name, graph::PinSemanticId output,
            ImplementationEntry::Create create
        )
        {
            ImplementationEntry implementation;
            implementation.capability = definition.id;
            implementation.identity = {std::string{name}, 1};
            implementation.supported_outputs = {output};
            implementation.create = create;
            CHECK(server.registerEffectImplementation(std::move(implementation)));
        };
        const auto display = outputSource(false);
        const auto hdr = outputSource(true);
        CHECK(server.registerEffectCapability(display));
        CHECK(server.registerEffectCapability(hdr));
        add(display, display_clear_name, ToneMappingPorts::color.semantic, fixture::Effect<buildOutputClear<false>>::create);
        add(display, "test.output.view", ToneMappingPorts::color.semantic, fixture::Effect<buildViewColor>::create);
        add(hdr, hdr_source_name, ToneMappingPorts::color.semantic, fixture::Effect<buildOutputClear<true>>::create);
        add(hdr, hdr_import_name, ToneMappingPorts::color.semantic, fixture::Effect<buildOutputImport>::create);
        ColorCapture::registerWith(server);
    }

    CreateEffectRequest clearOutputRequest(bool hdr = false)
    {
        CreateEffectRequest request;
        request.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*request.graph};
        const auto source = outputSource(hdr);
        const auto node = take(edit.addNode(source, {std::string{source.canonical_name}, 1}));
        CHECK(edit.setOutput("color", edit.pinId(node, ToneMappingPorts::color.semantic), true));
        CHECK(edit.commit());
        return request;
    }

    enum class EExpectedEncoding
    {
        LINEAR,
        SRGB,
        HDR_LINEAR
    };

    graph::NodeId addOutputTone(EffectGraphEdit& edit, bool imported, EExpectedEncoding encoding, std::string exposure)
    {
        const auto hdr = take(edit.addNode(
            outputSource(true), {std::string{imported ? hdr_import_name : hdr_source_name}, 1}
        ));
        const char* implementation = "lux.render.tone_mapping.linear";
        if (encoding == EExpectedEncoding::SRGB) implementation = "lux.render.tone_mapping.srgb";
        if (encoding == EExpectedEncoding::HDR_LINEAR) implementation = "lux.render.tone_mapping.hdr_linear";
        const auto tone = take(edit.addNode(capabilities::ToneMapping, {std::string{implementation}, 1},
            "exposure=" + exposure));
        const auto capture = take(edit.addNode(ColorCapture::definition(), {std::string{ColorCapture::name}, 1}));
        CHECK(edit.connect(edit.pinId(hdr, ToneMappingPorts::color.semantic), edit.pinId(tone, ToneMappingPorts::hdr_color.semantic)));
        CHECK(edit.connect(edit.pinId(tone, ToneMappingPorts::color.semantic), edit.pinId(capture, ColorCapture::color)));
        CHECK(edit.setOutput("color", edit.pinId(tone, ToneMappingPorts::color.semantic), true));
        CHECK(edit.setOutput("pixels", edit.pinId(capture, ColorCapture::pixels)));
        return tone;
    }
}

static void verifyTonemapOutput(
    CaptureDevice device, SceneId scene,
    EffectOutputDesc output,
    EExpectedEncoding encoding
)
{
    const auto& [client, commands, session] = device;
    CapturedPixels pixels;
    CreateEffectRequest request;
    request.graph = std::make_unique<EffectGraph>();
    EffectGraphEdit edit{*request.graph};
    request.output = output;
    const auto tone = addOutputTone(edit, false, encoding, "2");
    CHECK(edit.commit());
    const auto effect = createPreparedEffect(client, commands, session, std::move(request));
    const auto view = call<CreateView>(client, commands, {scene}).view;
    const auto target = call<CreateTarget>(client, commands, {session, {64, 64, output.format}}).target;
    const auto attached = call<CreateRenderJob>(client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
    StartRenderJob::Request invalid_start{session, attached, target};
    const auto resource = GraphCaptureBuffer::bind({client, commands, session}, {scene, view}, {&pixels, output.format});
    JobBinding binding;
    binding.resources.push_back({std::string{ColorCapture::name}, resource});
    invalid_start.binding = binding;
    invalid_start.binding.inputs.conditions.push_back(1);
    CHECK(call<StartRenderJob>(client, commands, std::move(invalid_start), false).error.type == kDeviceArgument);
    call<StartRenderJob>(client, commands, {session, attached, target, binding});
    const auto other_scene = call<CreateScene>(client, commands, {}).scene;
    const auto other_view = call<CreateView>(client, commands, {other_scene}).view;
    const auto other_target = call<CreateTarget>(client, commands, {session, {64, 64, output.format}}).target;
    const auto other_resource = GraphCaptureBuffer::bind(
        {client, commands, session}, {other_scene, other_view}, {&pixels, output.format}
    );
    binding.resources.front().resource = other_resource;
    const auto other_attachment =
        startRenderJob(client, commands, session, other_scene, other_view, other_target, effect, binding);
    CHECK(resource != other_resource);
    CHECK(call<RemoveEffect>(client, commands, {effect}, false).error.type == kDeviceBusy);
    const auto compiled = call<QueryRenderStats>(client, commands, {}).compiled_graphs;
    const auto pipeline_layouts = call_capture_counts[1].load();
    const auto pipelines = call_capture_counts[2].load();
    CHECK(!call_capture_core || pipelines != 0);
    renderFrame(client, commands, attached);
    renderFrame(client, commands, other_attachment);
    call<SetToneMappingExposure>(client, commands, {effect, tone, -1.F});
    call<SetToneMappingExposure>(client, commands, {effect, tone, -1.F});
    const auto invalid = std::numeric_limits<float>::quiet_NaN();
    CHECK(call<SetToneMappingExposure>(client, commands, {effect, tone, invalid}, false).error.type == kFrameContract);
    renderFrame(client, commands, other_attachment);
    renderFrame(client, commands, attached);
    CHECK(call<QueryRenderStats>(client, commands, {}).compiled_graphs == compiled);
    CHECK(call_capture_counts[1].load() == pipeline_layouts && call_capture_counts[2].load() == pipelines);
    stopAndRemoveJob(client, commands, attached);
    CHECK(call<RemoveEffect>(client, commands, {effect}, false).error.type == kDeviceBusy);
    stopAndRemoveJob(client, commands, other_attachment);
    call<RemoveEffect>(client, commands, {effect});
    const auto readback_type = GraphCaptureDescriptor::descriptor.type;
    CHECK(call<QueryResource>(client, commands, {session, ViewScope{scene, view}, readback_type}).resource_id == resource);
    CHECK(call<QueryResource>(
        client, commands, {session, ViewScope{other_scene, other_view}, readback_type}
    ).resource_id == other_resource);
    const auto deadline = Clock::now() + 10s;
    const auto retire_view = [&](ViewScope location, unsigned count)
    {
        call<RemoveView>(client, commands, location);
        while (pixels.ready.load(std::memory_order_acquire) != count)
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
    };
    retire_view({scene, view}, 1);
    retire_view({other_scene, other_view}, 2);
    call<RemoveTarget>(client, commands, target);
    call<RemoveTarget>(client, commands, other_target);
    call<RemoveScene>(client, commands, {other_scene});
    CHECK(pixels.frames.size() == 4);
    constexpr std::array hdr{.25, .5, .75};
    const auto matches = [&](const auto& frame, double gain)
    {
        if (encoding == EExpectedEncoding::HDR_LINEAR)
        {
            // Binary16 encodes the exposed RGB exactly: (1, 2, 3) or (1/8, 1/4, 3/8).
            const std::array<std::uint16_t, 4> bright{0x3c00, 0x4000, 0x4200, 0x3c00};
            const std::array<std::uint16_t, 4> dim{0x3000, 0x3400, 0x3600, 0x3c00};
            const auto& expected = gain == 4 ? bright : dim;
            return frame.size() >= sizeof(expected) && std::memcmp(frame.data(), expected.data(), sizeof(expected)) == 0;
        }
        if (frame[3] != std::byte{255}) return false;
        for (std::size_t channel = 0; channel < hdr.size(); ++channel)
        {
            const auto x = hdr[channel] * gain;
            auto value = (x * (2.51 * x + .03)) / (x * (2.43 * x + .59) + .14);
            if (encoding == EExpectedEncoding::SRGB)
                value = 1.055 * std::pow(value, 1 / 2.4) - .055;
            const auto expected = std::lround(value * 255);
            if (std::abs(std::to_integer<long>(frame[channel]) - expected) > 2) return false;
        }
        return true;
    };
    for (std::size_t index : {0, 2})
    {
        const auto frames = std::span(pixels.frames).subspan(index, 2);
        CHECK(std::ranges::count_if(frames, [&](const auto& frame) { return matches(frame, 4); }) == 1);
        CHECK(std::ranges::count_if(frames, [&](const auto& frame) { return matches(frame, .5); }) == 1);
    }
    std::printf("SHARED EFFECT PASS two scenes rendered +2 EV and -1 EV, pipelines=%llu unchanged, resources retained\n",
        static_cast<unsigned long long>(pipelines));
}

static void verifyTargetInput(
    ipc::IpcRequester& client, const CommandList& commands, DeviceSessionId session, SceneId scene,
    DeviceSessionId foreign_device = {},
    EffectId existing_effect = {}, JobBinding writer_binding = {},
    std::array<double, 3> expected_hdr = {.25, .5, .75}
)
{
    const auto hdr_format = rdesc::ETextureFormat::RGBA16_SFLOAT;
    const auto ldr_format = rdesc::ETextureFormat::RGBA8_UNORM;
    const auto producer_view = call<CreateView>(client, commands, {scene}).view;
    const auto consumer_view = call<CreateView>(client, commands, {scene}).view;
    const auto source = call<CreateTarget>(
        client, commands, {session, {64, 64, hdr_format, 1, ETargetKind::OFFSCREEN, true}}
    ).target;
    const auto target = call<CreateTarget>(client, commands, {session, {64, 64, ldr_format}}).target;
    auto producer = existing_effect;
    if (producer.isNull())
    {
        auto request = clearOutputRequest(true);
        request.output = {rdesc::ETextureFormat::RGBA16_SFLOAT, 1, ETargetKind::OFFSCREEN, true};
        producer = createPreparedEffect(client, commands, session, std::move(request));
    }

    CapturedPixels pixels;
    CreateEffectRequest consume;
    consume.graph = std::make_unique<EffectGraph>();
    EffectGraphEdit edit{*consume.graph};
    addOutputTone(edit, true, EExpectedEncoding::LINEAR, "0");
    CHECK(edit.commit());
    const auto consumer = createPreparedEffect(client, commands, session, std::move(consume));
    const auto writer =
        call<CreateRenderJob>(client, commands, {scene, producer_view, producer, EJobCadence::REQUESTED}).job;
    call<StartRenderJob>(client, commands, {session, writer, source, std::move(writer_binding)});
    const auto reader = call<CreateRenderJob>(client, commands, {
        scene, consumer_view, consumer, EJobCadence::REQUESTED
    }).job;
    StartRenderJob::Request start{session, reader, target};
    const auto readback = GraphCaptureBuffer::bind(
        {client, commands, session}, {scene, consumer_view}, {&pixels, ldr_format}
    );
    start.binding.resources.push_back({std::string{ColorCapture::name}, readback});
    start.binding.target_images.push_back({source, "unknown", EProductRead::LATEST_PUBLISHED});
    CHECK(call<StartRenderJob>(client, commands, start, false).error.type == kRuntimeIdentity);
    start.binding.target_images.front().binding = hdr_binding;
    if (!foreign_device.isNull())
    {
        const auto foreign = call<CreateTarget>(client, commands, {
            foreign_device, {64, 64, hdr_format, 1, ETargetKind::OFFSCREEN, true}
        }).target;
        start.binding.target_images.front().target = foreign;
        CHECK(call<StartRenderJob>(client, commands, start, false).error.type == kJobState);
        call<RemoveTarget>(client, commands, foreign);
        start.binding.target_images.front().target = source;
        std::puts("DEVICE IMPORT PASS: rejected a sampled Target from the peer physical adapter");
    }
    call<StartRenderJob>(client, commands, std::move(start));
    CHECK(call<RequestRenderJob>(client, commands, {reader}, false).error.type == kRuntimeState);
    const auto published = call<RequestRenderJob>(client, commands, {writer});
    const auto submitted = call<RequestRenderJob>(client, commands, {reader});
    CHECK(published.slot_index != submitted.slot_index);
    stopAndRemoveJob(client, commands, reader);
    call<RemoveView>(client, commands, {scene, consumer_view});
    const auto deadline = Clock::now() + 10s;
    while (!pixels.ready.load(std::memory_order_acquire)) CHECK(Clock::now() < deadline);
    CHECK(submitted.job == reader && submitted.slot_index < pixels.frames.size());
    const auto& color = pixels.frames[submitted.slot_index];
    const bool matches = color[0] > std::byte{0} && color[1] > color[0] && color[2] > color[1] &&
                         color[3] == std::byte{255};
    if (!matches)
    {
        std::fprintf(stderr, "Sampled Target slot %u pixel %u %u %u %u\n", submitted.slot_index,
            std::to_integer<unsigned>(color[0]), std::to_integer<unsigned>(color[1]),
            std::to_integer<unsigned>(color[2]), std::to_integer<unsigned>(color[3]));
    }
    CHECK(matches);
    if (!existing_effect.isNull())
    {
        for (std::size_t channel = 0; channel < expected_hdr.size(); ++channel)
        {
            const auto x = expected_hdr[channel];
            const auto mapped = (x * (2.51 * x + .03)) / (x * (2.43 * x + .59) + .14);
            CHECK(std::abs(std::to_integer<long>(color[channel]) - std::lround(mapped * 255)) <= 2);
        }
    }
    stopAndRemoveJob(client, commands, writer);
    call<RemoveView>(client, commands, {scene, producer_view});
    call<RemoveTarget>(client, commands, target);
    call<RemoveTarget>(client, commands, source);
    call<RemoveEffect>(client, commands, {consumer});
    if (existing_effect.isNull()) call<RemoveEffect>(client, commands, {producer});
    std::puts("TARGET INPUT PASS: named EffectGraph import resolved to a sampled producer output");
}

namespace
{
    constexpr auto node_color = effectPortSemantic("color");
    constexpr auto node_input = effectPortSemantic("source");
    enum class ENodeBuildGate : std::uint8_t { PASS, PAUSE, FAIL };
    std::atomic<ENodeBuildGate> node_build_gate{ENodeBuildGate::PASS};
    std::atomic_uint node_builds{};
    rdesc::EShaderTarget expected_node_target;

    struct SetNodeTestValue final
    {
        static constexpr std::string_view name{"render.test.effect.node_value"};

        struct Request final
        {
            EffectId effect;
            graph::NodeId node;
            int expected;
            int next;
        };

        using Response = AckResponse;
        static EffectId effectId(const Request& request) noexcept { return request.effect; }
        static graph::NodeId nodeId(const Request& request) noexcept { return request.node; }
    };

    void setNodeTestValue(int& state, const SetNodeTestValue::Request& request, AckResponse& result) noexcept
    {
        if (state != request.expected)
        {
            result.error = {kFrameContract};
            return;
        }
        state = request.next;
        result = {};
    }

    template <auto Build> EffectCreateResult createNodeTest(graph::NodeId, std::string_view args) noexcept
    {
        if (args != "producer" && args != "forward" && args != "dim" && args != "tone input")
            return lux::cxx::unexpected(RenderError{kFrameContract});
        return std::make_unique<fixture::StatefulEffect<Build, int>>(args, args == "producer" ? 5 : 10);
    }

    std::atomic_uint target_graph_builds{};

    RenderResult<void> buildImportedTargetNode(EffectCompileContext& context, std::string_view) noexcept
    {
        constexpr auto format = rdesc::ETextureFormat::RGBA16_SFLOAT;
        auto first = context.importTargetImage("first", format);
        if (!first) return lux::cxx::unexpected(first.error());
        auto second = context.importTargetImage("second", format);
        if (!second) return lux::cxx::unexpected(second.error());
        CHECK(first->resource != second->resource);
        ++target_graph_builds;

        GraphImageDesc image{0, 0, 1, 1, format};
        image.extent.source = EImageExtentSource::TARGET;
        const auto output = take(context.declareResource({"import.output", image}));
        const auto intermediate = take(context.declareResource({"import.intermediate", image}));
        const auto program = take(context.addProgram(GraphProgram{kEffectGraphImport.load(context.shaderTarget())}));
        const auto predicate = take(context.sameBinding(first->resource, second->resource));
        const auto sample = [&](ResourceVersion left, ResourceVersion right, GraphResourceId destination)
        {
            ComputeWork work;
            work.interface.program = program;
            work.interface.resources = {
                {"first_image", {left}}, {"second_image", {right}},
                {"generated", {{destination, 0}, {}, EGraphAccess::WRITE}}
            };
            work.dispatch = ImageDispatchArguments{destination};
            return work;
        };
        const auto result = context.choose(
            predicate,
            [&](EffectCompileContext&) noexcept -> RenderResult<ResourceVersion> { return *first; },
            [&](EffectCompileContext& branch) noexcept -> RenderResult<ResourceVersion>
            {
                auto pass = branch.addComputePass("combine targets", sample(*first, *second, intermediate));
                if (!pass) return lux::cxx::unexpected(pass.error());
                return pass->written.front();
            }
        );
        if (!result) return lux::cxx::unexpected(result.error());
        const auto color = take(context.declareResource({"sampled color", image}));
        const auto generated = take(context.addComputePass("sample selected color", sample(*result, *result, color)));
        const auto copied = take(context.addTransferPass(
            "publish sampled inputs", TransferWork{{lux::render::CopyImage{{generated.written.front()}, {{output, 0}}}}}
        ));
        return context.publish(node_color, {{node_color, copied.written.front()}});
    }

    constexpr std::uint64_t effect_capture_bytes = 64 * 256;

#ifdef _WIN32
static int transformRetirementBacklog(EGraphicsBackend backend)
{
    Validation validation;
    DeviceOptions options;
    options.backend = backend;
    options.multiple_queues = true;
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    auto& graphics = session->queue();
    auto& compute = session->queue(EQueueRole::COMPUTE);
    if (&graphics == &compute) return 77;

    RetirementFence graphics_gate(*session), compute_gate(*session);
    const auto accept = [&](SubmissionQueue& queue, RetirementFence& gate)
    {
        const nri::FenceSubmitDesc wait{gate.fence, 1, nri::StageBits::ALL};
        const auto serial = take(runTask(queue.submit({}, std::span{&wait, 1})));
        return session->submissionPoint(session->queueIndex(queue), serial);
    };
    std::array submitted{session->makeFrontiers(), session->makeFrontiers(), session->makeFrontiers()};
    auto completed = session->makeFrontiers();
    constexpr unsigned width = 16'384;
    TransformRecords records{{1, 1}};
    using Identity = extension_detail::ResourceIdentity;
    const auto collect = [&](bool active)
    {
        session->readCompleted(completed);
        records.collect(active, submitted, completed);
    };
    for (unsigned round = 0; round < submitted.size(); ++round)
    {
        submitted[round].accept(accept(graphics, graphics_gate));
        for (unsigned slot = 0; slot < width; ++slot)
        {
            if (round && slot == (round == 1 ? width / 2 : 0)) submitted[round].accept(accept(compute, compute_gate));
            const bool active = round == 1 && slot < width / 2;
            const auto id = records.insert(kIdentityTransform);
            const auto key = Identity::local(id);
            CHECK(key.index == round * width + slot && key.gen == 1);
            if (slot == 0)
            {
                records.retainInstance(id);
                const auto busy = records.release(id);
                CHECK(!busy && busy.error().type == kDeviceBusy && records.contains(id));
                records.releaseInstance(id);
            }
            CHECK(records.release(id) && !records.contains(id));
            auto future = key;
            ++future.gen;
            CHECK(!records.contains(Identity::make<TransformRef>(records.owner(), future)));
            collect(active);
        }
    }
    CHECK(graphics.poll() && compute.poll());
    collect(false);
    CHECK(Identity::local(records.insert(kIdentityTransform)).index == width * 3);

    const auto verify_reuse = [&](unsigned first, unsigned count)
    {
        std::vector<bool> seen(count);
        for (unsigned i = 0; i < count; ++i)
        {
            const auto id = records.insert(kIdentityTransform);
            const auto key = Identity::local(id);
            CHECK(key.index >= first && key.index - first < count && key.gen == 2);
            CHECK(!seen[key.index - first] && records.contains(id));
            seen[key.index - first] = true;
        }
    };
    graphics_gate.signal();
    CHECK(take(graphics.wait(graphics.submitted(), 5'000'000'000)));
    collect(false);
    CHECK(records.hasRetired());
    verify_reuse(0, width);
    CHECK(Identity::local(records.insert(kIdentityTransform)).index == width * 3 + 1);

    compute_gate.signal();
    CHECK(take(compute.wait(compute.submitted(), 5'000'000'000)));
    collect(false);
    CHECK(!records.hasRetired());
    verify_reuse(width, width * 2);
    CHECK(session->drain() && validation.errors == 0);
    std::puts("TRANSFORM RETIREMENT PASS: 49152 logical IDs invalidated; graphics released 16384 indices, "
              "compute released 32768; all reused once with generation 2");
    return 0;
}

    struct UploadAcquireProbe final
    {
        enum class EPhase { IDLE, ARMED, COPY_ACCEPTED, ACQUIRE_REJECTED };

        static inline bool enabled = false;
        static inline std::atomic_bool qualified{};
        static inline std::atomic<EPhase> phase{EPhase::IDLE};
        static inline std::atomic_uint destructions{}, retries{};
        static inline std::atomic<const nri::CommandBuffer*> copy_commands{};
        static inline std::atomic<nri::Texture*> candidate{};

        static void attach(DeviceSession& session, nri::Fence& gate) noexcept
        {
            auto& transfer = session.queue(EQueueRole::TRANSFER);
            const auto granularity = transfer.properties().image_transfer_granularity;
            qualified = &transfer != &session.queue() && granularity[0] == 1 && granularity[1] == 1 &&
                granularity[2] == 1;
            if (!qualified) return;
            transfer_queue_ = &transfer.native();
            graphics_queue_ = &session.queue().native();
            gate_ = &gate;
            auto& core = const_cast<nri::CoreInterface&>(session.nriCore());
            submit_ = core.QueueSubmit;
            create_ = core.CreatePlacedTexture;
            copy_ = core.CmdUploadBufferToTexture;
            destroy_ = core.DestroyTexture;
            fence_value_ = core.GetFenceValue;
            core.CreatePlacedTexture = [](
                nri::Device& device, nri::Memory* memory, std::uint64_t offset,
                const nri::TextureDesc& description, nri::Texture*& texture
            ) noexcept
            {
                const auto result = create_(device, memory, offset, description, texture);
                if (phase == EPhase::ACQUIRE_REJECTED && result == nri::Result::SUCCESS) ++retries;
                return result;
            };
            core.CmdUploadBufferToTexture = [](
                nri::CommandBuffer& command, nri::Texture& target, const nri::TextureRegionDesc& region,
                const nri::Buffer& source, const nri::TextureDataLayoutDesc& layout
            ) noexcept
            {
                if (phase == EPhase::ARMED)
                {
                    CHECK(!candidate.exchange(&target));
                    copy_commands = &command;
                }
                if (phase == EPhase::ACQUIRE_REJECTED)
                {
                    // The first Copy used the entire staging page. Any later recording needs its completion.
                    CHECK(fence_value_(*copy_receipt_.fence) >= copy_receipt_.value);
                }
                copy_(command, target, region, source, layout);
            };
            core.QueueSubmit = [](nri::Queue& queue, const nri::QueueSubmitDesc& desc) noexcept
            {
                if (phase == EPhase::COPY_ACCEPTED && &queue == graphics_queue_)
                {
                    const auto waits = std::span{desc.waitFences, desc.waitFenceNum};
                    const bool waits_for_copy = std::ranges::any_of(waits, [](const nri::FenceSubmitDesc& wait) noexcept
                    {
                        return wait.fence == copy_receipt_.fence && wait.value == copy_receipt_.value;
                    });
                    if (waits_for_copy)
                    {
                        phase = EPhase::ACQUIRE_REJECTED;
                        return nri::Result::INVALID_ARGUMENT;
                    }
                }
                if (phase == EPhase::ARMED && &queue == transfer_queue_)
                {
                    const auto commands = std::span{desc.commandBuffers, desc.commandBufferNum};
                    const auto* recorded = copy_commands.load();
                    if (recorded && std::ranges::find(commands, recorded) != commands.end())
                    {
                        CHECK(desc.waitFenceNum == 0 && desc.signalFenceNum == 1);
                        const nri::FenceSubmitDesc gate{gate_, 1, nri::StageBits::ALL};
                        auto gated = desc;
                        gated.waitFences = &gate;
                        gated.waitFenceNum = 1;
                        const auto accepted = submit_(queue, gated);
                        CHECK(accepted == nri::Result::SUCCESS);
                        copy_receipt_ = desc.signalFences[0];
                        phase = EPhase::COPY_ACCEPTED;
                        return accepted;
                    }
                }
                return submit_(queue, desc);
            };
            core.DestroyTexture = [](nri::Texture* texture) noexcept
            {
                if (texture && texture == candidate.load())
                {
                    CHECK(phase == EPhase::ACQUIRE_REJECTED);
                    CHECK(fence_value_(*copy_receipt_.fence) >= copy_receipt_.value);
                    candidate = nullptr;
                    ++destructions;
                }
                destroy_(texture);
            };
        }

    private:
        static inline nri::Queue* transfer_queue_ = nullptr;
        static inline nri::Queue* graphics_queue_ = nullptr;
        static inline nri::Fence* gate_ = nullptr;
        static inline nri::FenceSubmitDesc copy_receipt_{};
        static inline decltype(nri::CoreInterface::QueueSubmit) submit_{};
        static inline decltype(nri::CoreInterface::CreatePlacedTexture) create_{};
        static inline decltype(nri::CoreInterface::CmdUploadBufferToTexture) copy_{};
        static inline decltype(nri::CoreInterface::DestroyTexture) destroy_{};
        static inline decltype(nri::CoreInterface::GetFenceValue) fence_value_{};
    };
#endif

    struct UniformUploadProbe final
    {
        enum class EFault : std::uint8_t { NONE, REJECT_UPLOAD, HOLD_UPLOAD, REJECT_SUFFIX, UNKNOWN_UPLOAD };
        enum class EPipelineGate : std::uint8_t { OPEN, ARMED, HELD };

        static void attach(DeviceSession& session) noexcept
        {
            if (original_copy) return;
            auto& core = const_cast<nri::CoreInterface&>(session.nriCore());
            describe = core.GetBufferDesc;
            original_copy = core.CmdCopyBuffer;
            original_create = core.CreatePlacedBuffer;
            original_submit = core.QueueSubmit;
            original_idle = core.QueueWaitIdle;
            original_pipeline = core.CreateGraphicsPipeline;
            original_destroy_pipeline = core.DestroyPipeline;
            original_create_pool = core.CreateDescriptorPool;
            original_allocate_sets = core.AllocateDescriptorSets;
            original_set_descriptor_set = core.CmdSetDescriptorSet;
            original_destroy_pool = core.DestroyDescriptorPool;
            original_indexed = core.CmdDrawIndexed;
            original_indexed_indirect = core.CmdDrawIndexedIndirect;
            can_grow_texture_table = session.enabledFeatures().variable_descriptor_count;
            can_batch_indexed = session.enabledFeatures().multi_draw_indirect &&
                session.description().other.drawIndirectMaxNum >= 2;
            auto& async_queue = session.queue(EQueueRole::COMPUTE);
            if (&async_queue != &session.queue()) compute_queue = &async_queue.native();
            core.CmdCopyBuffer = [](
                nri::CommandBuffer& command, nri::Buffer& target, std::uint64_t target_offset,
                const nri::Buffer& source, std::uint64_t source_offset, std::uint64_t bytes
            ) noexcept
            {
                if (isUniform(describe(target)))
                {
                    ++copies;
                    upload_command.store(&command);
                }
                original_copy(command, target, target_offset, source, source_offset, bytes);
            };
            core.CreatePlacedBuffer = [](
                nri::Device& device, nri::Memory* memory, std::uint64_t offset,
                const nri::BufferDesc& desc, nri::Buffer*& buffer
            ) noexcept
            {
                const auto result = original_create(device, memory, offset, desc, buffer);
                if (result == nri::Result::SUCCESS && isUniform(desc)) ++backings;
                return result;
            };
            core.QueueSubmit = [](nri::Queue& queue, const nri::QueueSubmitDesc& desc) noexcept
            {
                auto expected = EPipelineGate::ARMED;
                const bool held = submission_gate.compare_exchange_strong(expected, EPipelineGate::HELD);
                if (held) submission_gate.wait(EPipelineGate::HELD);
                auto rejection = reject_submission_countdown.load();
                while (rejection != 0)
                {
                    if (!reject_submission_countdown.compare_exchange_weak(rejection, rejection - 1)) continue;
                    if (rejection == 1) return nri::Result::INVALID_ARGUMENT;
                    break;
                }
                const auto active_fault = fault.load();
                const auto commands = std::span(desc.commandBuffers, desc.commandBufferNum);
                const auto* upload = upload_command.load();
                const bool is_upload = upload && std::ranges::find(commands, upload) != commands.end();
                if (is_upload)
                {
                    upload_command.store(nullptr);
                    if (active_fault == EFault::REJECT_UPLOAD) return nri::Result::INVALID_ARGUMENT;
                    if (active_fault == EFault::HOLD_UPLOAD)
                    {
                        upload_held.store(true);
                        release_upload.wait(false);
                    }
                }
                if (active_fault == EFault::REJECT_SUFFIX && &queue != compute_queue && suffix_ready.load())
                {
                    suffix_rejected.store(true);
                    return nri::Result::INVALID_ARGUMENT;
                }
                auto submitted = desc;
#ifdef _WIN32
                std::vector<nri::FenceSubmitDesc> waits;
                auto* gpu_gate = commands.empty() ? nullptr : next_product_wait.exchange(nullptr);
                if (gpu_gate)
                {
                    CHECK(desc.signalFenceNum == 1);
                    product_signal = desc.signalFences[0];
                }
                if (held && gate_texture_gpu.exchange(false))
                {
                    CHECK(!commands.empty());
                    if (watch_texture_pool)
                    {
                        const auto* recorded = old_pool_command.load();
                        CHECK(recorded && std::ranges::find(commands, recorded) != commands.end());
                    }
                    gpu_gate = texture_retirement_fence;
                }
                if (gpu_gate)
                {
                    if (desc.waitFenceNum) waits.assign(desc.waitFences, desc.waitFences + desc.waitFenceNum);
                    waits.push_back({gpu_gate, 1, nri::StageBits::ALL});
                    submitted.waitFences = waits.data();
                    submitted.waitFenceNum = static_cast<std::uint32_t>(waits.size());
                }
#endif
                const auto result = original_submit(queue, submitted);
                if (result != nri::Result::SUCCESS) return result;
                if (&queue == compute_queue)
                {
                    ++compute_submissions;
                    if (active_fault == EFault::REJECT_SUFFIX) suffix_ready.store(true);
                    if (active_fault == EFault::HOLD_UPLOAD && held_upload_signal.fence)
                    {
                        const auto waits = std::span{desc.waitFences, desc.waitFenceNum};
                        CHECK(std::ranges::count_if(waits, [](const auto& wait)
                        {
                            return wait.fence == held_upload_signal.fence && wait.value >= held_upload_signal.value &&
                                wait.stages == nri::StageBits::ALL;
                        }) == 1);
                        checked_upload_wait = true;
                    }
                }
                if (is_upload)
                {
                    if (active_fault == EFault::HOLD_UPLOAD)
                    {
                        CHECK(desc.signalFenceNum == 1);
                        held_upload_signal = desc.signalFences[0];
                    }
                    if (active_fault == EFault::UNKNOWN_UPLOAD)
                    {
                        unknown_submitted.store(true);
                        return nri::Result::FAILURE;
                    }
                    ++accepted_uploads;
                }
                return result;
            };
            core.QueueWaitIdle = [](nri::Queue* queue) noexcept
            {
                const auto result = original_idle(queue);
                if (unknown_submitted.load() && result == nri::Result::SUCCESS) unknown_retired.store(true);
                return result;
            };
            core.CreateGraphicsPipeline = [](nri::Device& device, const nri::GraphicsPipelineDesc& desc,
                                             nri::Pipeline*& pipeline) noexcept
            {
                auto expected = EPipelineGate::ARMED;
                const bool held = pipeline_gate.compare_exchange_strong(expected, EPipelineGate::HELD);
                if (held) pipeline_gate.wait(EPipelineGate::HELD);
                const auto result = original_pipeline(device, desc, pipeline);
                if (held && result == nri::Result::SUCCESS) held_pipeline.store(pipeline);
                if (result == nri::Result::SUCCESS && watch_cold_prefix.exchange(false)) cold_prefix = pipeline;
                return result;
            };
            core.DestroyPipeline = [](nri::Pipeline* pipeline) noexcept
            {
                auto* expected = pipeline;
                const bool held = pipeline && held_pipeline.compare_exchange_strong(expected, nullptr);
                expected = pipeline;
                const bool prefix = pipeline && cold_prefix.compare_exchange_strong(expected, nullptr);
                original_destroy_pipeline(pipeline);
                if (held) held_pipeline_destroyed = true;
                if (prefix) cold_prefix_destroyed = true;
            };
            core.CreateDescriptorPool = [](
                nri::Device& device, const nri::DescriptorPoolDesc& desc, nri::DescriptorPool*& pool
            ) noexcept
            {
                ++pool_creations;
                if (reject_raster_pool.exchange(false)) return nri::Result::INVALID_ARGUMENT;
                const auto result = original_create_pool(device, desc, pool);
                if (!watch_texture_pool || result != nri::Result::SUCCESS) return result;
                if ((desc.flags & nri::DescriptorPoolBits::ALLOW_UPDATE_AFTER_SET) == 0) return result;
                for (std::size_t i = 0; i < texture_pools.size(); ++i)
                {
                    if (desc.textureMaxNum != 2u << i) continue;
                    auto& generation = texture_pools[i];
                    CHECK(!generation.pool && generation.creates == 0);
                    generation.pool = pool;
                    ++generation.creates;
                }
                return result;
            };
            core.DestroyDescriptorPool = [](nri::DescriptorPool* pool) noexcept
            {
                for (auto& generation : texture_pools)
                {
                    if (!generation.pool || generation.pool != pool) continue;
                    generation.pool = nullptr;
                    original_destroy_pool(pool);
                    ++generation.destroys;
                    return;
                }
                original_destroy_pool(pool);
            };
            core.AllocateDescriptorSets = [](
                nri::DescriptorPool& pool, const nri::PipelineLayout& layout, std::uint32_t index,
                nri::DescriptorSet** sets, std::uint32_t count, std::uint32_t variables
            ) noexcept
            {
                set_allocations += count;
                if (reject_raster_set_countdown.load() && reject_raster_set_countdown.fetch_sub(1) == 1)
                    return nri::Result::INVALID_ARGUMENT;
                const auto result = original_allocate_sets(pool, layout, index, sets, count, variables);
                if (result == nri::Result::SUCCESS && &pool == texture_pools.front().pool)
                {
                    CHECK(count == 1);
                    old_texture_set = sets[0];
                }
                return result;
            };
            core.CmdSetDescriptorSet = [](nri::CommandBuffer& command, const nri::SetDescriptorSetDesc& desc) noexcept
            {
                if (desc.descriptorSet && desc.descriptorSet == old_texture_set.load()) old_pool_command = &command;
                original_set_descriptor_set(command, desc);
            };
            core.CmdDrawIndexed = [](nri::CommandBuffer& command, const nri::DrawIndexedDesc& draw) noexcept
            {
                ++indexed_calls;
                original_indexed(command, draw);
            };
            core.CmdDrawIndexedIndirect = [](
                nri::CommandBuffer& command, const nri::Buffer& arguments, std::uint64_t offset,
                std::uint32_t count, std::uint32_t stride, const nri::Buffer* count_buffer,
                std::uint64_t count_offset
            ) noexcept
            {
                ++indexed_indirect_calls;
                indexed_indirect_draws += count;
                original_indexed_indirect(command, arguments, offset, count, stride, count_buffer, count_offset);
            };
        }

        static inline std::atomic_uint copies{};
        static inline std::atomic_uint backings{};
        static inline std::atomic_uint compute_submissions{};
        static inline nri::FenceSubmitDesc held_upload_signal{};
        static inline std::atomic_bool checked_upload_wait{};
        static inline std::atomic_uint accepted_uploads{};
        static inline std::atomic_uint reject_submission_countdown{};
        static inline std::atomic<EFault> fault{EFault::NONE};
        static inline std::atomic pipeline_gate{EPipelineGate::OPEN};
        static inline std::atomic_bool held_pipeline_destroyed{};
        static inline std::atomic_bool watch_cold_prefix{}, cold_prefix_destroyed{};
        static inline std::atomic<nri::Pipeline*> cold_prefix{};
        static inline std::atomic submission_gate{EPipelineGate::OPEN};
        static inline std::atomic_bool upload_held{}, release_upload{};
        static inline std::atomic_bool suffix_rejected{}, unknown_retired{};
        static inline std::atomic_uint pool_creations{}, set_allocations{};
        static inline std::atomic_bool reject_raster_pool{};
        static inline std::atomic_uint reject_raster_set_countdown{};
        static inline std::atomic_bool can_grow_texture_table{}, watch_texture_pool{};
        static inline std::atomic_bool can_batch_indexed{};
        static inline std::atomic_uint indexed_calls{}, indexed_indirect_calls{}, indexed_indirect_draws{};
        struct TexturePool final
        {
            nri::DescriptorPool* pool;
            std::atomic_uint creates, destroys;
        };

        static inline std::array<TexturePool, 4> texture_pools{};
        static inline std::atomic<const nri::CommandBuffer*> old_pool_command{};
#ifdef _WIN32
        static inline bool test_unpublished_retirement = false;
        static inline nri::Fence* texture_retirement_fence = nullptr;
        static inline std::atomic_bool gate_texture_gpu{};
        static inline std::atomic<nri::Fence*> next_product_wait{};
        static inline nri::FenceSubmitDesc product_signal{};
#endif
        static bool hasSeparateComputeQueue() noexcept { return compute_queue != nullptr; }

    private:
        static bool isUniform(const nri::BufferDesc& desc) noexcept
        {
            return (desc.usage & nri::BufferUsageBits::CONSTANT) != 0;
        }

        static inline decltype(nri::CoreInterface::GetBufferDesc) describe{};
        static inline decltype(nri::CoreInterface::CmdCopyBuffer) original_copy{};
        static inline decltype(nri::CoreInterface::CreatePlacedBuffer) original_create{};
        static inline decltype(nri::CoreInterface::QueueSubmit) original_submit{};
        static inline decltype(nri::CoreInterface::QueueWaitIdle) original_idle{};
        static inline decltype(nri::CoreInterface::CreateGraphicsPipeline) original_pipeline{};
        static inline decltype(nri::CoreInterface::DestroyPipeline) original_destroy_pipeline{};
        static inline std::atomic<nri::Pipeline*> held_pipeline{};
        static inline decltype(nri::CoreInterface::CreateDescriptorPool) original_create_pool{};
        static inline decltype(nri::CoreInterface::DestroyDescriptorPool) original_destroy_pool{};
        static inline decltype(nri::CoreInterface::AllocateDescriptorSets) original_allocate_sets{};
        static inline decltype(nri::CoreInterface::CmdSetDescriptorSet) original_set_descriptor_set{};
        static inline std::atomic<const nri::DescriptorSet*> old_texture_set{};
        static inline decltype(nri::CoreInterface::CmdDrawIndexed) original_indexed{};
        static inline decltype(nri::CoreInterface::CmdDrawIndexedIndirect) original_indexed_indirect{};
        static inline std::atomic<const nri::CommandBuffer*> upload_command{};
        static inline std::atomic_bool suffix_ready{}, unknown_submitted{};
        static inline const nri::Queue* compute_queue = nullptr;
    };

    struct EffectCaptureDescriptor final
    {
        using Config = void;

        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.effect.readback"}, {}, {}, {},
            "View-owned EffectGraph buffer readback.", EResourceDomain::VIEW
        };
    };

    class EffectCaptureBuffer final
    {
    public:
        using Descriptor = EffectCaptureDescriptor;

        static ResourceRegistration factory() noexcept
        {
            static const StorageBindingOps bindings{
                .describeGraphImport = describe,
                .resolveGraphImport = resolve
            };
            return makeResourceRegistration<Descriptor, EffectCaptureBuffer, &EffectCaptureBuffer::create>(
                &bindings
            );
        }

        EffectCaptureBuffer(ResourceCreateContext& context, CapturedPixels& pixels) noexcept
            : device_(context.device), capture_(context), config_{.result = &pixels, .bytes = effect_capture_bytes}
        {
#ifdef _WIN32
            const bool descriptor_probe = UniformUploadProbe::watch_texture_pool &&
                context.device.enabledFeatures().variable_descriptor_count;
            const bool graph_gate = descriptor_probe || UniformUploadProbe::test_unpublished_retirement;
            if (graph_gate || UploadAcquireProbe::enabled)
            {
                texture_retirement_ = std::make_unique<RetirementFence>(context.device);
                test_texture_retirement = texture_retirement_.get();
                if (graph_gate) UniformUploadProbe::texture_retirement_fence = texture_retirement_->fence;
                if (UploadAcquireProbe::enabled) UploadAcquireProbe::attach(context.device, *texture_retirement_->fence);
            }
#endif
        }

        ~EffectCaptureBuffer() noexcept
        {
            if (product_)
                device_.deferRelease([&core = device_.nriCore(), buffer = product_]() noexcept { core.DestroyBuffer(buffer); });
#ifdef _WIN32
            if (texture_retirement_)
            {
                device_.deferRelease([retirement = std::move(texture_retirement_)]() noexcept {});
            }
#endif
        }

        static RenderResult<std::unique_ptr<EffectCaptureBuffer>>
        create(ResourceCreateContext& context, ResourceInstanceId) noexcept
        {
            CHECK(created_ < pixels.size());
            auto capture = std::make_unique<EffectCaptureBuffer>(context, pixels[created_++]);
            UniformUploadProbe::attach(context.device);
            return capture;
        }

#ifdef _WIN32
        static inline const RetirementFence* test_texture_retirement = nullptr;
        static inline DeviceSession* product_device = nullptr;
#endif
        static inline std::array<CapturedPixels, 14> pixels;

    private:
        static RenderResult<ResourceProjection> describe(const void*, std::uint32_t projection) noexcept
        {
            if (projection == 1) return ResourceProjection::bufferOutput(16, EGraphUse::TRANSFER_SOURCE);
            if (projection != 0) return lux::cxx::unexpected(RenderError{kStorageContract});
            return ResourceProjection::bufferOutput(effect_capture_bytes, EGraphUse::TRANSFER_DESTINATION);
        }

        static RenderResult<void> resolve(
            void* object, std::uint32_t projection, const VGraphResourceDesc& description,
            std::span<ResourceInput> output
        ) noexcept
        {
            auto& owner = *static_cast<EffectCaptureBuffer*>(object);
            if (projection == 1)
            {
                if (!owner.product_)
                {
                    const nri::BufferDesc desc{16, 4, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE};
                    auto& device = owner.device_;
                    CHECK(device.nriCore().CreatePlacedBuffer(device.device(), NriDeviceHeap, desc, owner.product_) ==
                        nri::Result::SUCCESS);
#ifdef _WIN32
                    product_device = &device;
#endif
                }
                std::ranges::fill(output, ResourceInput{owner.product_, 0, 16});
                return {};
            }
            CHECK(projection == 0);
            return CaptureBuffer::resolve(&owner.capture_, description, &owner.config_, output);
        }

        static inline std::size_t created_ = 0;
#ifdef _WIN32
        std::unique_ptr<RetirementFence> texture_retirement_;
#endif
        DeviceSession& device_;
        nri::Buffer* product_ = nullptr;
        CaptureBuffer capture_;
        CaptureConfig config_;
    };

    constexpr auto node_bytes = effectPortSemantic("bytes");
    std::atomic_uint storage_node_builds{};

    RenderResult<void> buildImportedStorageNode(EffectCompileContext& context, std::string_view args) noexcept
    {
        if (args == "publish" || args == "consume")
        {
            const auto type = EffectCaptureDescriptor::descriptor.type;
            constexpr GraphBufferRange range{0, 16};
            TransferWork work;
            if (args == "publish")
            {
                const auto output = take(context.importFinalBuffer("words", type, {
                    "", GraphBufferDesc{16}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_SOURCE, range
                }, 1));
                work = TransferWork{{FillBuffer{{output, range}, 0xc3c3c3c3}}};
            }
            else
            {
                const auto input = take(context.importResource("words", type, {
                    GraphBufferDesc{16}, {range}, EGraphUse::TRANSFER_SOURCE
                }, 1));
                const auto output = take(context.importFinalBuffer("capture", GraphCaptureDescriptor::descriptor.type, {
                    "", GraphBufferDesc{16}, EGraphOrigin::IMPORTED, {},
                    EGraphUse::TRANSFER_DESTINATION, range
                }));
                work = TransferWork{{CopyBuffer{{input, range}, {output, range}}}};
            }
            const auto pass = take(context.addTransferPass("product words", std::move(work)));
            return context.publish(node_bytes, {{node_bytes, pass.written.front()}});
        }
        const auto type = args == "wrong-type" ?
            CaptureDescriptor::descriptor.type.name() : EffectCaptureDescriptor::descriptor.type.name();
        const auto bytes = args == "wrong-shape" ? effect_capture_bytes + sizeof(std::uint32_t) : effect_capture_bytes;
        const ResourceNameView requirement{type};
        const GraphResource declaration{
            "", GraphBufferDesc{bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION
        };
        auto first = args == "shared-write" ? context.importResource(
            "first", requirement, {declaration.description, {}, declaration.initial_use}
        ) :
            context.importFinalBuffer("first", requirement, declaration);
        if (!first) return lux::cxx::unexpected(first.error());
        auto second = context.importFinalBuffer("second", requirement, declaration);
        if (!second) return lux::cxx::unexpected(second.error());
        ++storage_node_builds;

        const auto transient = take(context.declareResource({"staging", GraphBufferDesc{bytes}}));
        const auto fill = [&](EffectCompileContext& arm, std::uint32_t pattern) noexcept -> RenderResult<ResourceVersion>
        {
            auto pass = arm.addTransferPass(
                "fill " + std::to_string(pattern), TransferWork{{FillBuffer{{{transient, 0}}, pattern}}}
            );
            if (!pass) return lux::cxx::unexpected(pass.error());
            return pass->written.front();
        };
        const auto selected = take(context.choose(
            take(context.sameBinding(first->resource, second->resource)),
            [&](EffectCompileContext& arm) noexcept { return fill(arm, 0x5a5a5a5au); },
            [&](EffectCompileContext& arm) noexcept -> RenderResult<ResourceVersion>
            {
                auto value = fill(arm, 0x6b6b6b6bu);
                if (!value) return lux::cxx::unexpected(value.error());
                auto copy = arm.addTransferPass("copy to first output", TransferWork{{CopyBuffer{{*value}, {*first}}}});
                if (!copy) return lux::cxx::unexpected(copy.error());
                if (args == "mixed-boundary")
                {
                    const auto temporary = take(arm.declareResource({
                        "copy back", GraphBufferDesc{effect_capture_bytes}
                    }));
                    auto read = arm.addTransferPass("read first output", TransferWork{{
                        CopyBuffer{{copy->written.front()}, {{temporary, 0}}}
                    }});
                    if (!read) return lux::cxx::unexpected(read.error());
                    return read->written.front();
                }
                return *value;
            }
        ));
        if (args == "ambiguous-write")
        {
            CHECK(context.addTransferPass("also write first", TransferWork{{CopyBuffer{{selected}, {*first}}}}));
        }
        const auto copied = take(context.addTransferPass("copy to storage", TransferWork{{
            CopyBuffer{{selected}, {*second}}
        }}));
        return context.publish(node_bytes, {{node_bytes, copied.written.front()}});
    }

    std::atomic_bool reject_value_preparation{}, reject_staged_constants{};
    std::atomic_uint source_links{}, source_instances{}, view_preparations{};
    std::atomic_uint source_workspaces_destroyed{};
    std::atomic<const void*> source_linked_inputs{};
    std::atomic_bool reject_view_workspace{};
    std::weak_ptr<const int> producer_source_lifetime;
    std::atomic_bool reject_view_link{};

    class TestValueProducer final
    {
    public:
        using Configuration = ProducerSourceConfig;
        using LinkedData = std::size_t;

        static RenderResult<LinkedData>
        resolve(const Configuration& config, std::span<const RasterOperands, 0>) noexcept
        {
            ++source_links;
            if (!config.lifetime || reject_view_link.exchange(false))
                return lux::cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            return static_cast<LinkedData>(*config.lifetime);
        }

        struct Workspace final
        {
            std::uint64_t view_revision = 0;

            ~Workspace() noexcept { ++source_workspaces_destroyed; }
        };

        static RenderResult<std::unique_ptr<Workspace>>
        createWorkspace(const LinkedData& linked, const TargetDesc&, const InstalledDrawSources&) noexcept
        {
            const auto* previous = source_linked_inputs.exchange(&linked);
            CHECK(!previous || previous == &linked);
            CHECK(producer_source_lifetime.expired());
            ++source_instances;
            auto workspace = std::make_unique<Workspace>();
            if (reject_view_workspace.exchange(false)) return lux::cxx::unexpected(RenderError{kDeviceCapacity});
            return workspace;
        }

        static RenderResult<std::tuple<RevisionedValue<ProducerConstants>>>
        onFrame(
            const LinkedData& channel, const EffectFrameInputs<ViewGpuParameters>& inputs, EffectFrameWriter&, Workspace& workspace
        ) noexcept
        {
            if (reject_value_preparation.exchange(false)) return lux::cxx::unexpected(RenderError{kFrameContract});
            const auto& view = std::get<0>(inputs.values);
            CHECK(view.revision >= workspace.view_revision && view.revision != 0);
            workspace.view_revision = view.revision;
            ++view_preparations;
            if (reject_staged_constants.exchange(false)) return lux::cxx::unexpected(RenderError{kFrameContract});
            const ProducerConstants block{std::bit_cast<std::uint32_t>(view.value.get().background[channel])};
            return std::tuple{RevisionedValue{block, view.revision}};
        }
    };

    struct UniformNodeState final
    {
        TestUniformBlock block{{0x31313131, 0x31313131, 0x31313131, 0x31313131}};
        std::uint64_t revision = 1;
    };

    struct SetTestUniformBlock final
    {
        static constexpr std::string_view name{"render.test.effect.set_uniform"};

        struct Request final
        {
            EffectId effect;
            graph::NodeId node;
            std::uint32_t value;
        };

        using Response = AckResponse;
        static EffectId effectId(const Request& request) noexcept { return request.effect; }
        static graph::NodeId nodeId(const Request& request) noexcept { return request.node; }
    };

    void setTestUniformBlock(UniformNodeState& state, const SetTestUniformBlock::Request& request, AckResponse& out) noexcept
    {
        const auto v = request.value;
        if (state.revision == UINT64_MAX)
        {
            out.error = {kRuntimeCapacity};
            return;
        }
        state.block = {{v, v, v, v}};
        ++state.revision;
        out.error = {};
    }

    RenderResult<void> buildUniformOutput(EffectCompileContext& context, std::string_view arguments) noexcept
    {
        const ResourceNameView requirement{EffectCaptureDescriptor::descriptor.type.name()};
        auto external = context.importFinalBuffer("readback", requirement, {
            "", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION
        });
        if (!external) return lux::cxx::unexpected(external.error());
        const auto uniform = context.constantBuffer<TestUniformBlock, UniformNodeState,
            &UniformNodeState::block, &UniformNodeState::revision>();
        if (!uniform) return lux::cxx::unexpected(uniform.error());

        const auto intermediate = take(context.declareResource({"generated values", GraphBufferDesc{effect_capture_bytes}}));
        const bool async = arguments == "async";
        constexpr std::uint64_t branch_gpu_ns = 80'000;
        if (async)
        {
            const auto scratch = take(context.declareResource({"independent graphics", GraphBufferDesc{16}}));
            CHECK(context.addTransferPass(
                "independent graphics", TransferWork{{FillBuffer{{{scratch, 0}}, 1}}},
                {.side_effect = true, .estimated_gpu_ns = branch_gpu_ns}
            ));
        }
        ComputeWork compute;
        compute.allow_async_compute = async;
        compute.interface.program = take(context.addProgram(GraphProgram{kEffectNodeUniform.load(context.shaderTarget())}));
        compute.interface.resources = {
            {"uniform_block", {*uniform}},
            {"generated", {{intermediate, 0}, {}, EGraphAccess::WRITE}}
        };
        compute.interface.resources.front().setUniformConstant<TestUniformBlock>();
        compute.dispatch = DispatchArguments{{effect_capture_bytes / (sizeof(std::uint32_t) * 64), 1, 1}};
        const auto produced = take(context.addComputePass(
            "compute uniform values", std::move(compute), {.estimated_gpu_ns = async ? branch_gpu_ns : 0}
        ));
        const auto copied = take(context.addTransferPass(
            "read uniform values", TransferWork{{CopyBuffer{{produced.written.front()}, {*external}}}},
            {.estimated_gpu_ns = async ? 100u : 0u}
        ));
        return context.publish(node_bytes, {{node_bytes, copied.written.front()}});
    }

    RenderResult<void> buildPreparedOutput(EffectCompileContext& context, std::string_view args) noexcept
    {
        const ResourceNameView requirement{EffectCaptureDescriptor::descriptor.type.name()};
        auto imported = context.importFinalBuffer("readback", requirement, {
            "", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION
        });
        if (!imported) return lux::cxx::unexpected(imported.error());

        const auto gpu = take(context.declareResource({"prepared values", GraphBufferDesc{effect_capture_bytes}}));
        ComputeWork compute;
        compute.interface.program = take(context.addProgram(GraphProgram{kEffectGraphProducer.load(context.shaderTarget())}));
        compute.interface.resources = {{"generated", {{gpu, 0}, {}, EGraphAccess::WRITE}}};
        compute.interface.parameters.setInlineConstant(ProducerConstants{0});
        compute.dispatch = DispatchArguments{{effect_capture_bytes / (sizeof(std::uint32_t) * 64), 1, 1}};
        const auto produced = take(context.addComputePass("compute prepared values", std::move(compute)));
        const auto copy = take(context.addTransferPass(
            "read prepared values", TransferWork{{CopyBuffer{{produced.written.front()}, {*imported}}}}
        ));
        struct BlockSource final
        {
            ProducerConstants constants;
            TestSharedTextureIndices texture_indices;
        };
        if (args == "copy-type")
            CHECK((context.copyInlineBlock<TestSharedTextureIndices, BlockSource, &BlockSource::texture_indices>(
                produced.pass
            )));
        if (args == "copy-source" || args == "copy-and-producer")
            CHECK((context.copyInlineBlock<ProducerConstants, BlockSource, &BlockSource::constants>(produced.pass)));
        if (args == "copy-type" || args == "copy-source")
            return context.publish(node_bytes, {{node_bytes, copy.written.front()}});

        const auto seed = take(context.declareResource({"companion seed", GraphBufferDesc{4}}));
        const auto scratch = take(context.declareResource({"companion scratch", GraphBufferDesc{4}}));
        const auto undefined = take(context.declareResource({"uninitialized companion input", GraphBufferDesc{4}}));
        const auto unused = take(context.declareResource({"unused companion target", GraphBufferDesc{4}}));
        const auto seeded = take(context.addTransferPass("seed companion", TransferWork{{FillBuffer{{{seed, 0}}, 7}}}));
        const auto companion = take(context.addTransferPass(
            "keep companion", TransferWork{{CopyBuffer{{seeded.written.front()}, {{scratch, 0}}}}}
        ));
        const auto dormant = take(context.addTransferPass(
            "dormant undefined read", TransferWork{{CopyBuffer{{{undefined, 0}}, {{unused, 0}}}}}
        ));

        const auto camera = take(context.readResourceView<ViewGpuParameters>(
            kViewParameters, kViewParameters.binding
        ));
        auto lifetime = std::make_shared<const int>(0);
        if (args.empty()) producer_source_lifetime = lifetime;
        FrameAlgorithmUses uses{
            .inline_blocks = {produced.pass}, .passes = {companion.pass}, .resource_views = {camera}
        };
        if (args == "output-count") uses.inline_blocks.clear();
        if (args == "input-count") uses.resource_views.clear();
        // Keep the compute pass live when testing a missing inline destination.
        uses.passes.push_back(produced.pass);
        CHECK((context.onFrame<&TestValueProducer::onFrame, &TestValueProducer::resolve, &TestValueProducer::createWorkspace>(
            ProducerSourceConfig{std::move(lifetime)}, std::move(uses)
        )));
        const auto dormant_passes = args == "joint-closure" ?
            std::vector{seeded.pass, dormant.pass} : std::vector{dormant.pass};
        CHECK((context.onFrame<&TestValueProducer::onFrame, &TestValueProducer::resolve, &TestValueProducer::createWorkspace>(
            ProducerSourceConfig{}, {.passes = dormant_passes}
        )));
        return context.publish(node_bytes, {{node_bytes, copy.written.front()}});
    }

    enum class ETestStreamMode : std::uint8_t
    {
        VALID, BAD_INDEX, BAD_CONSTANTS, BAD_BINDING, BAD_IMAGE, BAD_SAMPLER, DUPLICATE_ELEMENT,
        FOREIGN_BUFFER, FOREIGN_IMAGE, PARTIAL_IMAGE_GROUP,
        DUPLICATE_LAYER, MISSING_LAYER, UNCOVERED_DRAW, EMPTY, SWAP_TILES, SHARED_BINDINGS, DISTINCT_BINDINGS,
        BAD_INDIRECT_STRIDE, SHORT_INDIRECT_ARGS, BAD_COUNT, WRONG_INDIRECT_USE, NO_COUNT, OLD_PROGRAM_SLOT
    };
    std::atomic<ETestStreamMode> stream_mode{ETestStreamMode::VALID};
    enum class ETestStreamPrograms : std::uint8_t
    {
        INITIAL, INVALID, BAD_GEOMETRY, PENDING, REVERSED, EQUIVALENT
    };
    struct SetStreamPrograms final
    {
        static constexpr std::string_view name = "test.effect.set_stream_programs";
        struct Request final
        {
            ResourceInstanceId resource;
            ETestStreamPrograms mode;
        };
        using Data = Request;
        using Response = AckResponse;
        static ResourceInstanceId resourceId(const Request& request) noexcept { return request.resource; }
    };

    rdesc::ShaderProgramArtifact streamShader(
        rdesc::EShaderTarget target, const rdesc::EmbeddedShader& fragment,
        const rdesc::EmbeddedShader& vertex_shader = kEffectStreamVertex
    ) noexcept;

    std::vector<RasterPassTemplate::Input> streamArguments()
    {
        return {
            {"tint", rdesc::EDescriptorType::UNIFORM_BUFFER, EGraphUse::STORAGE_READ, 2},
            {"tiles", rdesc::EDescriptorType::SAMPLED_IMAGE, EGraphUse::SAMPLED, 2},
            {"sampler_state", rdesc::EDescriptorType::SAMPLER, EGraphUse::SAMPLED, 2}
        };
    }

    class StreamProgramStorage final
    {
    public:
        using Descriptor = StreamProgramStorage;
        using Config = void;
        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.effect.stream_programs"}, {}, {}, {}, "Raster stream programs.", EResourceDomain::VIEW
        };

        static constexpr ResourceRequirement requirement{
            "stream_programs", ResourceNameView{"test.effect.stream_programs"}, EResourceDomain::VIEW,
            EResourceContentLifetime::PERSISTENT, true
        };

        explicit StreamProgramStorage(ResourceCreateContext& context) noexcept
            : programs(context), target_(context.device.enabledFeatures().shader_target) {}

        static std::span<const CommandBinding> commandBindings() noexcept
        {
            static const std::array commands{CommandBinding::declare<SetStreamPrograms, &set>()};
            return commands;
        }

        static ResourceRegistration factory() noexcept
        {
            static const StorageBindingOps bindings{
                .cpu_view = ResourceViewOps::make<StreamProgramStorage, ETestStreamPrograms, &selectedMode>(),
                .rasterPrograms = [](const void* owner) noexcept -> const RasterProgramSet&
                {
                    return static_cast<const StreamProgramStorage*>(owner)->programs;
                }
            };
            return makeResourceRegistration<Descriptor, StreamProgramStorage, &create>(&bindings);
        }

        RasterProgramSet programs;
        ETestStreamPrograms mode = ETestStreamPrograms::INITIAL;

    private:
        std::uint64_t revision_ = 0;
        RenderResult<ResourceViewValue<ETestStreamPrograms>> selectedMode() noexcept
        {
            return ResourceViewValue<ETestStreamPrograms>{std::cref(mode), revision_};
        }

        static RenderResult<std::unique_ptr<StreamProgramStorage>>
        create(ResourceCreateContext& context, ResourceInstanceId) noexcept
        {
            return std::make_unique<StreamProgramStorage>(context);
        }

        static exec::task<AckResponse> set(StreamProgramStorage& storage, SetStreamPrograms::Request&& request) noexcept
        {
            const auto mode = request.mode;
            RasterProgramSet::Description description;
            description.arguments = streamArguments();
            description.colors = {rdesc::ETextureFormat::RGBA8_UNORM};
            description.shaders.push_back(streamShader(storage.target_, kEffectStreamInline));
            description.shaders.push_back(streamShader(storage.target_, kEffectStreamUniform));
            if (mode == ETestStreamPrograms::INVALID)
                description.shaders.back().target = storage.target_ == rdesc::EShaderTarget::SPIRV ?
                    rdesc::EShaderTarget::DXIL : rdesc::EShaderTarget::SPIRV;
            ParameterBinding parameters;
            parameters.setInlineConstant(ProducerConstants{0});
            description.programs = {{0, {}, std::move(parameters)}, {1, {}, {}}};
            if (mode == ETestStreamPrograms::BAD_GEOMETRY) description.programs.front().geometry_locations = {0};
            if (mode == ETestStreamPrograms::PENDING) description.programs.front().graphics.blend.enabled = true;
            if (mode == ETestStreamPrograms::REVERSED || mode == ETestStreamPrograms::EQUIVALENT)
            {
                for (auto& program : description.programs) program.graphics.front_counter_clockwise = false;
                std::swap(description.programs[0], description.programs[1]);
            }
            const auto prepared = co_await storage.programs.replace(std::move(description));
            if (!prepared) co_return AckResponse{prepared.error()};
            storage.mode = mode;
            ++storage.revision_;
            co_return AckResponse{};
        }

        rdesc::EShaderTarget target_;
    };

    constexpr GraphBufferRange stream_linear_arguments{4, 32};
    constexpr GraphBufferRange stream_indexed_arguments{40, 44};
    constexpr GraphBufferRange stream_count_word{8, 4};
    constexpr std::uint32_t stream_indexed_stride = 24;

    class TestRasterProducer final
    {
    public:
        struct Configuration final
        {
            GraphResourceId indices, uniform, image, arguments, count;
            std::uint32_t sampler, comparison_sampler;
        };

        struct LinkedData final
        {
            DrawStreamSlot stream;
            RasterBufferSlot indices, uniform, arguments, count;
            RasterImageSlot image;
            RasterSamplerSlot sampler, comparison_sampler;
            std::uint32_t tint_argument, tiles_argument, sampler_argument;
        };

        static RenderResult<LinkedData>
        resolve(const Configuration& input, std::span<const RasterOperands, 1> streams) noexcept
        {
            auto index = streams.front().buffer(input.indices);
            auto uniform = streams.front().buffer(input.uniform);
            auto arguments = streams.front().buffer(input.arguments);
            auto count = streams.front().buffer(input.count);
            auto image = streams.front().image(input.image);
            auto sampler = streams.front().sampler(input.sampler);
            auto comparison = streams.front().sampler(input.comparison_sampler);
            auto tint_argument = streams.front().argument("tint");
            auto tiles_argument = streams.front().argument("tiles");
            auto sampler_argument = streams.front().argument("sampler_state");
            const bool missing_buffer = !index || !uniform || !arguments || !count;
            const bool missing_texture_binding = !image || !sampler || !comparison;
            const bool missing_argument = !tint_argument || !tiles_argument || !sampler_argument;
            if (missing_buffer || missing_texture_binding || missing_argument)
                return lux::cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            return LinkedData{
                streams.front().stream(), *index, *uniform, *arguments, *count, *image,
                *sampler, *comparison, *tint_argument, *tiles_argument, *sampler_argument
            };
        }

        struct Workspace final
        {
            std::optional<RasterProgramSlot> first_slot;
            std::uint64_t reversed_revision = 0;
        };

        static RenderResult<void> onFrame(
            const LinkedData& linked, const EffectFrameInputs<ETestStreamPrograms>& inputs,
            EffectFrameWriter& frame, Workspace& workspace
        ) noexcept
        {
            const auto program_mode = std::get<0>(inputs.values).value.get();
            const auto mode = stream_mode.exchange(ETestStreamMode::VALID);
            if (mode == ETestStreamMode::EMPTY) return frame.setRasterStream(linked.stream, {});
            const bool reversed = program_mode == ETestStreamPrograms::REVERSED || program_mode == ETestStreamPrograms::EQUIVALENT;
            auto first_program = frame.rasterProgram(linked.stream, reversed ? 1 : 0);
            auto second_program = frame.rasterProgram(linked.stream, reversed ? 0 : 1);
            if (!first_program || !second_program) return lux::cxx::unexpected(RenderError{kFrameContract});
            if (program_mode == ETestStreamPrograms::REVERSED) workspace.reversed_revision = first_program->revision;
            if (program_mode == ETestStreamPrograms::EQUIVALENT)
                CHECK(first_program->revision == workspace.reversed_revision);
            if (!workspace.first_slot) workspace.first_slot = *first_program;
            if (mode == ETestStreamMode::OLD_PROGRAM_SLOT) first_program = *workspace.first_slot;
            const std::array values{ProducerConstants{241}, ProducerConstants{83}};
            constexpr GraphImageRange first_tile{1, 0, 1, 0, 1};
            constexpr GraphImageRange second_tile{1, 1, 1, 1, 1};
            constexpr GraphImageRange outside_envelope{1, 1, 1, 0, 1};
            const bool swap_tiles = mode == ETestStreamMode::SWAP_TILES;
            const auto first_range = mode == ETestStreamMode::BAD_IMAGE ? outside_envelope :
                swap_tiles ? second_tile : first_tile;
            const auto second_range = swap_tiles ? first_tile : second_tile;
            const auto sampler = mode == ETestStreamMode::BAD_SAMPLER ? linked.comparison_sampler :
                linked.sampler;
            auto tile = linked.image;
            if (mode == ETestStreamMode::FOREIGN_IMAGE) ++tile.stream;
            if (mode == ETestStreamMode::PARTIAL_IMAGE_GROUP) ++tile.index;
            auto uniform = linked.uniform;
            if (mode == ETestStreamMode::FOREIGN_BUFFER) ++uniform.stream;
            // Physical descriptor order is compiler-owned. Publish in a different order.
            const std::array bindings{
                RasterStreamBinding{linked.tiles_argument, RasterImageSlice{tile, second_range},
                    mode == ETestStreamMode::DUPLICATE_ELEMENT ? 0u : 1u},
                RasterStreamBinding{linked.sampler_argument, sampler},
                RasterStreamBinding{linked.tint_argument,
                    RasterBufferSlice{uniform, 0, mode == ETestStreamMode::BAD_BINDING ? 257u : 256u}},
                RasterStreamBinding{linked.tiles_argument, RasterImageSlice{linked.image, first_range}, 0}
            };
            const auto offset = mode == ETestStreamMode::BAD_INDEX ? 1u : 0u;
            std::array draws{
                RasterStreamDraw{
                    *first_program, {}, {}, {sizeof(ProducerConstants), sizeof(ProducerConstants)}, DrawVertices{6}
                },
                RasterStreamDraw{
                    *first_program, {}, {},
                    {0, mode == ETestStreamMode::BAD_CONSTANTS ? 0u : std::uint32_t{sizeof(ProducerConstants)}},
                    DrawVertices{6}
                },
                RasterStreamDraw{
                    *second_program, {0, static_cast<std::uint32_t>(bindings.size())}, {}, {},
                    RasterStreamIndexed{{linked.indices, offset, 24 - offset}, EIndexType::UINT16, 6}
                }
            };
            const bool use_indirect = swap_tiles || mode == ETestStreamMode::NO_COUNT;
            const bool bad_indirect = mode == ETestStreamMode::BAD_INDIRECT_STRIDE ||
                mode == ETestStreamMode::SHORT_INDIRECT_ARGS || mode == ETestStreamMode::BAD_COUNT ||
                mode == ETestStreamMode::WRONG_INDIRECT_USE;
            if (use_indirect || bad_indirect)
            {
                std::optional<RasterBufferSlice> count;
                if (mode != ETestStreamMode::NO_COUNT)
                {
                    const auto skip = mode == ETestStreamMode::BAD_COUNT ? 1u : 0u;
                    count = RasterBufferSlice{
                        linked.count, stream_count_word.offset + skip, stream_count_word.size - skip
                    };
                }
                draws[0].draw = RasterStreamIndirect{
                    {linked.arguments, stream_linear_arguments.offset, stream_linear_arguments.size},
                    count,
                    {},
                    2
                };
                auto arguments = linked.arguments;
                if (mode == ETestStreamMode::WRONG_INDIRECT_USE) arguments = linked.uniform;
                const auto shortfall = mode == ETestStreamMode::SHORT_INDIRECT_ARGS ? 4u : 0u;
                draws[2].draw = RasterStreamIndirect{
                    {arguments, stream_indexed_arguments.offset, stream_indexed_arguments.size - shortfall},
                    count,
                    RasterStreamIndexBinding{{linked.indices, 0, 24}, EIndexType::UINT16},
                    2,
                    mode == ETestStreamMode::BAD_INDIRECT_STRIDE ? 18u : stream_indexed_stride
                };
            }
            const float viewport_width = 64.0f / inputs.target.width;
            std::array sections{
                RasterStreamSection{{0, 1}, 1, RasterViewport{viewport_width / 2, 0, viewport_width / 2, 0.5f}},
                RasterStreamSection{
                    {1, mode == ETestStreamMode::UNCOVERED_DRAW ? 1u : 2u},
                    mode == ETestStreamMode::DUPLICATE_LAYER ? 1u : 0u,
                    RasterViewport{0, 0, viewport_width, 1}
                }
            };
            if (mode == ETestStreamMode::SHARED_BINDINGS || mode == ETestStreamMode::DISTINCT_BINDINGS)
            {
                std::array<RasterStreamDraw, 67> repeated;
                std::ranges::copy(draws, repeated.begin());
                std::ranges::fill(std::span{repeated}.subspan(draws.size()), draws.back());
                sections.back().draws.count = static_cast<std::uint32_t>(repeated.size() - 1);
                if (mode == ETestStreamMode::DISTINCT_BINDINGS)
                {
                    std::vector<RasterStreamBinding> slices;
                    for (auto& draw : repeated)
                    {
                        if (!draw.bindings.count) continue;
                        draw.bindings.first = static_cast<std::uint32_t>(slices.size());
                        slices.insert(slices.end(), bindings.begin(), bindings.end());
                    }
                    return frame.setRasterStream(linked.stream, {
                        repeated, slices, {}, std::as_bytes(std::span{values}), sections
                    });
                }
                return frame.setRasterStream(linked.stream, {
                    repeated, bindings, {}, std::as_bytes(std::span{values}), sections
                });
            }
            const auto section_count = mode == ETestStreamMode::MISSING_LAYER ? 1u : 2u;
            return frame.setRasterStream(linked.stream, {
                draws, bindings, {}, std::as_bytes(std::span{values}), std::span{sections}.first(section_count)
            });
        }
    };

    rdesc::ShaderProgramArtifact
    prependStreamVertex(rdesc::ShaderProgramArtifact raster, const rdesc::EmbeddedShader& vertex_shader) noexcept
    {
        auto vertex = vertex_shader.load(raster.target);
        raster.interface.vertex_inputs = std::move(vertex.interface.vertex_inputs);
        raster.stages.insert(raster.stages.begin(), std::move(vertex.stages.front()));
        raster.interface.entry_points.insert(
            raster.interface.entry_points.begin(), vertex.interface.entry_points.begin(),
            vertex.interface.entry_points.end()
        );
        return raster;
    }

    rdesc::ShaderProgramArtifact
    streamShader(
        rdesc::EShaderTarget target, const rdesc::EmbeddedShader& fragment,
        const rdesc::EmbeddedShader& vertex_shader
    ) noexcept
    {
        return prependStreamVertex(fragment.load(target), vertex_shader);
    }

    std::uint32_t addEffectRasterProgram(EffectCompileContext& context, const rdesc::EmbeddedShader& fragment) noexcept
    {
        return take(context.addProgram(GraphProgram{streamShader(context.shaderTarget(), fragment)}));
    }

    RenderResult<void> buildTargetLists(EffectCompileContext& context, std::string_view) noexcept
    {
        const auto output = take(context.importFinalBuffer("capture", GraphCaptureDescriptor::descriptor.type, {
            "", GraphBufferDesc{256}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION
        }));
        const auto color = take(context.declareResource({"sampled row", GraphImageDesc{64, 1}}));
        const auto program = addEffectRasterProgram(context, kEffectStreamTarget);
        const auto raster = [&](ResourceVersion image, bool first)
        {
            RasterPassTemplate stream;
            stream.purpose = "sample Target";
            stream.draw_list = first ? "samples" : "overlay";
            stream.inputs = {{
                .name = "source_color", .kind = rdesc::EDescriptorType::SAMPLED_IMAGE, .usage = EGraphUse::SAMPLED,
                .stages = 2, .target_format = rdesc::ETextureFormat::RGBA16_SFLOAT
            }};
            stream.programs = std::vector{RasterPassTemplate::StreamProgram{program}};
            RasterWork work;
            work.program = std::move(stream);
            work.colors.push_back({{image}, first ? EAttachmentLoad::CLEAR : EAttachmentLoad::LOAD});
            return take(context.addRasterPass(first ? "first sample" : "second sample", std::move(work),
                {.estimated_gpu_ns = 80000}));
        };
        const auto first = raster({color, 0}, true);
        const auto scratch = take(context.declareResource({"async scratch",
            GraphImageDesc{8, 8, 1, 1, rdesc::ETextureFormat::RGBA16_SFLOAT}}));
        ComputeWork compute;
        compute.allow_async_compute = true;
        auto shader = kEffectGraphColor.load(context.shaderTarget());
        compute.interface.program = take(context.addProgram(GraphProgram{std::move(shader)}));
        compute.interface.resources = {{"generated", {{scratch, 0}, {}, EGraphAccess::WRITE}}};
        compute.dispatch = DispatchArguments{{1, 1, 1}};
        const auto middle = take(context.addComputePass("independent compute", std::move(compute),
            {.side_effect = true, .estimated_gpu_ns = 80000}));
        const auto second = raster(first.written.front(), false);
        CHECK(context.dependsOn(middle.pass, second.pass));
        const auto copied = take(context.addTransferPass("capture sample", TransferWork{{CopyImageBuffer{
            {second.written.front()}, {output}, 256, 256, ImageCopyRegion{0, 0, 64, 1}
        }}}, {.estimated_gpu_ns = 100}));
        return context.publish(node_bytes, {{node_bytes, copied.written.front()}});
    }

    RenderResult<void> buildRasterStreamOutput(EffectCompileContext& context, std::string_view) noexcept
    {
        auto imported = context.importFinalBuffer(
            "readback",
            EffectCaptureDescriptor::descriptor.type,
            {"", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION}
        );
        if (!imported) return lux::cxx::unexpected(imported.error());
        GraphImageDesc color_image{0, 0, 1, 2};
        color_image.extent.source = EImageExtentSource::TARGET;
        const auto color = take(context.declareResource({"raster stream color", color_image}));
        const auto indices = take(context.declareResource({"raster stream indices", GraphBufferDesc{24}}));
        const auto arguments = take(context.declareResource({"raster stream arguments", GraphBufferDesc{84}}));
        const auto count = take(context.declareResource({"raster stream count", GraphBufferDesc{16}}));
        auto indirect_shader = kEffectStreamIndirect.load(context.shaderTarget());
        ComputeWork generate_args;
        generate_args.interface.program = take(context.addProgram(GraphProgram{std::move(indirect_shader)}));
        generate_args.interface.resources = {
            {"arguments", {{arguments, 0}, {}, EGraphAccess::WRITE}},
            {"count", {{count, 0}, {}, EGraphAccess::WRITE}}
        };
        generate_args.dispatch = DispatchArguments{{1, 1, 1}};
        const auto generated = take(context.addComputePass("generate indirect commands", std::move(generate_args)));
        CHECK(generated.written.size() == 2);
        const auto arguments_ready = generated.written[0];
        const auto count_ready = generated.written[1];
        CHECK(arguments_ready.resource == arguments && count_ready.resource == count);
        const auto uniform = take(context.declareResource({"raster stream uniform", GraphBufferDesc{256}}));
        const auto shade = take(context.addTransferPass("fill uniform", TransferWork{{FillBuffer{{{uniform, 0}}, 197}}}));
        GraphImageDesc sampled_image{32, 32, 2, 2};
        sampled_image.extent.source = EImageExtentSource::TARGET;
        const auto tiles = take(context.declareResource({"raster stream tiles", sampled_image}));
        constexpr GraphImageRange first_tile{1, 0, 1, 0, 1};
        constexpr GraphImageRange second_tile{1, 1, 1, 1, 1};
        const auto tiled = take(context.addTransferPass("fill first tile", TransferWork{{
            ClearImage{{{tiles, 0}, first_tile}, {65.0f / 255, 0, 97.0f / 255, 1}}
        }}));
        const auto mipped = take(context.addTransferPass("fill second tile", TransferWork{{
            ClearImage{{tiled.written.front(), second_tile}, {129.0f / 255, 0, 161.0f / 255, 1}}
        }}));
        const auto sampler = context.addSampler({ESamplerFilter::NEAREST});
        const auto comparison_sampler = context.addSampler({
            ESamplerFilter::NEAREST, ESamplerAddress::CLAMP_TO_EDGE, EDepthCompare::LESS
        });
        ResourceVersion index_data{indices, 0};
        for (std::uint32_t i = 0; i < 6; ++i)
        {
            const auto start = (i < 3 ? 6u : 0u) + (i % 3) * 2;
            const auto pair = start | ((start + 1) << 16);
            const auto init = take(context.addTransferPass(
                "fill index word " + std::to_string(i),
                TransferWork{{FillBuffer{{index_data, GraphBufferRange{i * 4, 4}}, pair}}}
            ));
            index_data = init.written.front();
        }

        RasterPassTemplate stage;
        stage.purpose = "effect-stream";
        stage.inputs = streamArguments();
        stage.resources = {
            {.version = index_data, .usage = EGraphUse::INDEX},
            {.version = arguments_ready, .range = stream_linear_arguments, .usage = EGraphUse::INDIRECT},
            {.version = arguments_ready, .range = stream_indexed_arguments, .usage = EGraphUse::INDIRECT},
            {.version = count_ready, .range = stream_count_word, .usage = EGraphUse::INDIRECT},
            {.version = shade.written.front(), .usage = EGraphUse::STORAGE_READ},
            {.version = mipped.written.front(), .range = first_tile, .usage = EGraphUse::SAMPLED},
            {.version = mipped.written.front(), .range = second_tile, .usage = EGraphUse::SAMPLED}
        };
        stage.stream_samplers = {sampler, comparison_sampler};
        const auto mode = take(context.readResourceView<ETestStreamPrograms>(StreamProgramStorage::requirement, "program_mode"));
        stage.programs = RasterPassTemplate::ReadyPrograms{"stream_programs"};
        RasterWork raster;
        raster.colors.push_back({{{color, 0}}});
        raster.program = std::move(stage);
        const auto drawn = take(context.addRasterPass("two-program raster stream", std::move(raster)));
        constexpr std::uint32_t layer_bytes = effect_capture_bytes / 2;
        const auto first_copy = take(context.addTransferPass(
            "read raster stream layer 0",
            TransferWork{{CopyImageBuffer{
                {drawn.written.front(), GraphImageRange{1, 0, 1, 0, 1}},
                {*imported, GraphBufferRange{0, layer_bytes}}, 256, layer_bytes, ImageCopyRegion{0, 0, 64, 32}
            }}}
        ));
        const auto copied = take(context.addTransferPass(
            "read raster stream layer 1",
            TransferWork{{CopyImageBuffer{
                {drawn.written.front(), GraphImageRange{1, 0, 1, 1, 1}},
                {first_copy.written.front(), GraphBufferRange{layer_bytes, layer_bytes}},
                256, layer_bytes, ImageCopyRegion{0, 0, 64, 32}
            }}}
        ));
        CHECK((context.onFrame<&TestRasterProducer::onFrame, &TestRasterProducer::resolve>({
            indices, uniform, tiles, arguments, count, sampler,
            comparison_sampler
        }, {
            .resource_views = {mode}, .draw_streams = {drawn.pass}
        })));
        return context.publish(node_bytes, {{node_bytes, copied.written.front()}});
    }

    std::atomic_uint32_t runtime_layer_count{2};
    std::atomic_bool runtime_layer_conflict{false};
    enum class ELayerBias { VALID, MISSING, NONFINITE, UNDECLARED };
    std::atomic runtime_layer_bias{ELayerBias::VALID};

    class RuntimeLayerProducer final
    {
    public:
        using Configuration = bool;

        struct LinkedData final
        {
            DrawStreamSlot stream;
            RasterProgramSlot program;
            bool preserve;
        };

        static RenderResult<LinkedData>
        resolve(const Configuration& input, std::span<const RasterOperands, 1> streams) noexcept
        {
            auto program = streams.front().programAt(0);
            if (!program) return lux::cxx::unexpected(program.error());
            return LinkedData{streams.front().stream(), *program, input};
        }

        static RenderResult<void> onFrame(const LinkedData& linked, const EffectFrameInputs<>& inputs, EffectFrameWriter& frame) noexcept
        {
            const auto requested = runtime_layer_count.load();
            const auto count = linked.preserve && runtime_layer_conflict.load() ? requested % 3 + 1 : requested;
            constexpr std::array values{ProducerConstants{40}, ProducerConstants{80}, ProducerConstants{120}};
            constexpr std::array biases{RasterDepthBias{1048576}, RasterDepthBias{}, RasterDepthBias{-1048576}};
            std::array<RasterStreamSection, values.size()> sections{};
            std::array<RasterStreamDraw, values.size()> draws{};
            auto layer_sections = std::span{sections}.first(count);
            for (std::uint32_t i = 0; i < layer_sections.size(); ++i)
            {
                layer_sections[i] = {linked.preserve ? RasterStreamRange{} : RasterStreamRange{i, 1}, i};
                const auto selected = runtime_layer_bias.load();
                if (linked.preserve)
                {
                    if (selected == ELayerBias::UNDECLARED) layer_sections[i].depth_bias = RasterDepthBias{};
                }
                else if (selected != ELayerBias::MISSING)
                {
                    layer_sections[i].depth_bias = biases[i];
                    if (selected == ELayerBias::NONFINITE)
                        layer_sections[i].depth_bias->slope = std::numeric_limits<float>::infinity();
                }
                draws[i] = {
                    linked.program, {}, {}, {i * std::uint32_t{sizeof(ProducerConstants)}, sizeof(ProducerConstants)},
                    DrawVertices{6}
                };
            }
            return frame.setRasterStream(linked.stream, {
                std::span{draws}.first(linked.preserve ? 0 : count),
                {}, {}, std::as_bytes(std::span{values}), layer_sections
            });
        }
    };

    RenderResult<void> buildRuntimeLayersOutput(EffectCompileContext& context, std::string_view) noexcept
    {
        auto imported = context.importFinalBuffer(
            "readback",
            EffectCaptureDescriptor::descriptor.type,
            {"", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION}
        );
        if (!imported) return lux::cxx::unexpected(imported.error());
        GraphImageDesc image{64, 32, 1, 2};
        image.runtime_layers = true;
        const auto layers = take(context.declareResource({"runtime raster layers", image}));
        image.format = rdesc::ETextureFormat::D32_SFLOAT;
        const auto depth = take(context.declareResource({"runtime depth layers", image}));
        const auto graphics = addEffectRasterProgram(context, kEffectStreamInline);
        const auto raster = [&](ResourceVersion color_input, ResourceVersion depth_input, bool preserve)
        {
            RasterPassTemplate stage;
            stage.purpose = "runtime raster layers";
            ParameterBinding parameters;
            parameters.setInlineConstant(ProducerConstants{0});
            GraphicsState state;
            state.depth_test = state.depth_write = true;
            stage.programs = std::vector{RasterPassTemplate::StreamProgram{graphics, std::move(state), std::move(parameters)}};
            stage.section_depth_bias = !preserve;
            RasterWork work;
            const auto load = preserve ? EAttachmentLoad::LOAD : EAttachmentLoad::CLEAR;
            work.colors.push_back({{color_input}, load});
            work.depth = RasterAttachment{{depth_input}, load};
            work.program = std::move(stage);
            const auto pass = take(context.addRasterPass(preserve ? "load dynamic layers" : "draw dynamic layers",
                std::move(work)));
            CHECK((context.onFrame<&RuntimeLayerProducer::onFrame, &RuntimeLayerProducer::resolve>(
                preserve,
                {.draw_streams = {pass.pass}}
            )));
            return std::array{pass.written[0], pass.written[1]};
        };
        const auto drawn = raster({layers, 0}, {depth, 0}, false);
        const auto preserved = raster(drawn[0], drawn[1], true);
        const auto words = take(
            context.declareResource({"runtime layer capture", GraphBufferDesc{effect_capture_bytes}})
        );
        ComputeWork read;
        auto shader = kEffectStreamLayers.load(context.shaderTarget());
        read.interface.program = take(context.addProgram(GraphProgram{std::move(shader)}));
        read.interface.resources = {
            {"layers", {preserved[0]}},
            {"depth_layers", {preserved[1]}},
            {"generated", {{words, 0}, {}, EGraphAccess::WRITE}}
        };
        read.dispatch = DispatchArguments{{effect_capture_bytes / (sizeof(std::uint32_t) * 64), 1, 1}};
        const auto values = take(context.addComputePass("read dynamic layers", std::move(read)));
        const auto copied = take(context.addTransferPass(
            "publish dynamic layers readback", TransferWork{{CopyBuffer{{values.written.front()}, {*imported}}}}
        ));
        return context.publish(node_bytes, {{node_bytes, copied.written.front()}});
    }

    constexpr auto tone_left = effectPortSemantic("tone_left");
    constexpr auto tone_right = effectPortSemantic("tone_right");

    RenderResult<void> buildToneCapture(EffectCompileContext& context, std::string_view) noexcept
    {
        const auto left = context.inputs(tone_left);
        const auto right = context.inputs(tone_right);
        CHECK(left.size() == 1 && left.front().size() == 1);
        CHECK(right.size() == 1 && right.front().size() == 1);
        auto imported = context.importFinalBuffer(
            "readback",
            EffectCaptureDescriptor::descriptor.type,
            {"", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION}
        );
        if (!imported) return lux::cxx::unexpected(imported.error());
        constexpr std::uint32_t bytes = effect_capture_bytes / 2;
        const auto first = take(context.addTransferPass("copy left tone", TransferWork{{CopyImageBuffer{
            {left.front().front().version}, {*imported, GraphBufferRange{0, bytes}}, 256, bytes, ImageCopyRegion{0, 0, 64, 32}
        }}}));
        const auto second = take(context.addTransferPass("copy right tone", TransferWork{{CopyImageBuffer{
            {right.front().front().version}, {first.written.front(), GraphBufferRange{bytes, bytes}},
            256, bytes, ImageCopyRegion{0, 0, 64, 32}
        }}}));
        return context.publish(node_bytes, {{node_bytes, second.written.front()}});
    }

    RenderResult<void> buildNodeTest(EffectCompileContext& context, std::string_view args) noexcept
    {
        ++node_builds;
        if (args == "producer")
        {
            node_build_gate.wait(ENodeBuildGate::PAUSE, std::memory_order_acquire);
            if (node_build_gate.load() == ENodeBuildGate::FAIL)
                return lux::cxx::unexpected(RenderError{kFrameContract});
        }
        if (context.shaderTarget() != expected_node_target)
            return lux::cxx::unexpected(RenderError{kCapabilityDeclaration});
        if (args == "forward")
        {
            const auto input = context.inputs(node_input);
            if (input.size() != 1) return lux::cxx::unexpected(RenderError{kCapabilityDeclaration});
            return context.publish(node_color, input.front());
        }
        const auto& target = context.target();
        const auto format = args == "tone input" ? rdesc::ETextureFormat::RGBA16_SFLOAT : target.format;
        GraphImageDesc image{0, 0, 1, 1, format, target.samples};
        image.extent.source = EImageExtentSource::TARGET;
        auto color = context.declareResource({"node.color", image});
        if (!color) return lux::cxx::unexpected(color.error());
        if (args == "dim" || args == "tone input")
        {
            auto cleared = context.addTransferPass(
                "dim source", TransferWork{{ClearImage{{{*color, 0}}, {.125f, .375f, .625f, 1}}}}
            );
            if (!cleared) return lux::cxx::unexpected(cleared.error());
            return context.publish(node_color, {{node_color, cleared->written.front()}});
        }
        auto intermediate = context.declareResource({"node.intermediate", image});
        if (!intermediate) return lux::cxx::unexpected(intermediate.error());
        auto program = context.addProgram(GraphProgram{kEffectGraphColor.load(context.shaderTarget())});
        if (!program) return lux::cxx::unexpected(program.error());
        ComputeWork compute;
        compute.interface.program = *program;
        compute.interface.resources = {{"generated", {{*intermediate, 0}, {}, EGraphAccess::WRITE}}};
        compute.dispatch = ImageDispatchArguments{*intermediate};
        auto generated = context.addComputePass("node generate", std::move(compute));
        if (!generated) return lux::cxx::unexpected(generated.error());
        auto copied = context.addTransferPass(
            "node publish", TransferWork{{lux::render::CopyImage{{generated->written.front()}, {{*color, 0}}}}}
        );
        if (!copied) return lux::cxx::unexpected(copied.error());
        return context.publish(node_color, {{node_color, copied->written.front()}});
    }
}

static void effectGraphOutput(bool d3d12, bool async_uniform)
{
    RenderServerConfig config;
    config.channel_name = "lux.render.test.effect-graph";
    config.frames_in_flight = 2;
    config.uploads.emplace();
    enum class EResponseGate { OPEN, ARMED, BLOCKED };
    std::atomic<EResponseGate> response_gate{EResponseGate::OPEN};
    config.response_context = &response_gate;
    config.responses_available = [](void* value) noexcept
    {
        auto& gate = *static_cast<std::atomic<EResponseGate>*>(value);
        auto expected = EResponseGate::ARMED;
        if (!gate.compare_exchange_strong(expected, EResponseGate::BLOCKED)) return;
        gate.wait(EResponseGate::BLOCKED);
    };
    config.diagnostics.report = [](void*, const RenderDiagnostic& diagnostic) noexcept
    {
        if (diagnostic.severity == ERenderDiagnosticSeverity::ERROR_MESSAGE)
            std::fprintf(stderr, "EffectGraph diagnostic: %.*s\n",
                static_cast<int>(diagnostic.message.size()), diagnostic.message.data());
    };
    expected_node_target = d3d12 ? rdesc::EShaderTarget::DXIL : rdesc::EShaderTarget::SPIRV;
    config.validation = nativeValidationEnabled();
    auto& execution = lux::execution::ExecutionRuntime::instance();
    auto server = take(RenderServer::create(config, execution));
    take(server->registerResource<CaptureBuffer>());
    registerOutputNodes(*server);
    take(server->registerResource<StreamProgramStorage>(StreamProgramStorage::commandBindings()));
    take(server->registerResource<EffectCaptureBuffer>());
    take(server->registerResource<TargetDrawList>());
    constexpr std::string_view storage_schema = "test.render.effect.storage";
    EffectCapability storage_capability;
    storage_capability.id = graph::nodeTypeId(storage_schema);
    storage_capability.canonical_name = storage_schema;
    const EffectPortDefinition storage_capability_ports[]{{node_bytes, "bytes", {"test.readback_bytes", 1}, graph::EPinDirection::OUTPUT}};
    storage_capability.ports = storage_capability_ports;
    const ResourceRequirement storage_requirements[]{StreamProgramStorage::requirement,
        {"samples", TargetDrawList::descriptor.type, EResourceDomain::JOB, EResourceContentLifetime::PER_RUN},
        {"overlay", TargetDrawList::descriptor.type, EResourceDomain::JOB, EResourceContentLifetime::PER_RUN}};
    storage_capability.resources = storage_requirements;
    CHECK(server->registerEffectCapability(storage_capability));
    ImplementationEntry storage_implementation;
    storage_implementation.capability = storage_capability.id;
    storage_implementation.identity = {"test.node.storage_import", 1};
    storage_implementation.supported_outputs = {node_bytes};
    storage_implementation.create = fixture::Effect<buildImportedStorageNode>::create;
    const auto storage_selection = storage_implementation.identity;
    auto lists = storage_implementation;
    lists.identity = {"test.dynamic.target_lists", 1};
    lists.create = fixture::Effect<buildTargetLists>::create;
    const auto list_selection = lists.identity;
    CHECK(server->registerEffectImplementation(std::move(lists)));
    auto producer_implementation = storage_implementation;
    producer_implementation.identity = {"test.node.preparation", 1};
    producer_implementation.create = fixture::Effect<buildPreparedOutput>::create;
    const auto producer_selection = producer_implementation.identity;
    auto stream_implementation = storage_implementation;
    stream_implementation.identity = {"test.node.raster_stream", 1};
    stream_implementation.create = fixture::Effect<buildRasterStreamOutput>::create;
    const auto stream_selection = stream_implementation.identity;
    CHECK(server->registerEffectImplementation(std::move(stream_implementation)));
    auto runtime_layers = storage_implementation;
    runtime_layers.identity = {"test.node.runtime_raster_layers", 1};
    runtime_layers.create = fixture::Effect<buildRuntimeLayersOutput>::create;
    const auto runtime_selection = runtime_layers.identity;
    CHECK(server->registerEffectImplementation(std::move(runtime_layers)));
    CHECK(server->registerEffectImplementation(std::move(storage_implementation)));
    CHECK(server->registerEffectImplementation(std::move(producer_implementation)));

    const auto uniform_command = take(server->registerNodeEffectCommand(
        CommandBinding::declareForEffect<SetTestUniformBlock, UniformNodeState, setTestUniformBlock>()
    ));
    auto uniform_capability = storage_capability;
    constexpr std::string_view uniform_schema = "test.render.effect.uniform";
    uniform_capability.id = graph::nodeTypeId(uniform_schema);
    uniform_capability.canonical_name = uniform_schema;
    CHECK(server->registerEffectCapability(uniform_capability, {uniform_command.command}));
    ImplementationEntry uniform_implementation;
    uniform_implementation.capability = uniform_capability.id;
    uniform_implementation.identity = {"test.node.uniform", 1};
    uniform_implementation.supported_outputs = {node_bytes};
    uniform_implementation.create = [](graph::NodeId, std::string_view args) noexcept -> EffectCreateResult
    {
        return std::make_unique<fixture::StatefulEffect<buildUniformOutput, UniformNodeState>>(args, UniformNodeState{});
    };
    const auto uniform_selection = uniform_implementation.identity;
    CHECK(server->registerEffectImplementation(std::move(uniform_implementation), {uniform_command}));

    const auto command = take(server->registerNodeEffectCommand(
        CommandBinding::declareForEffect<SetNodeTestValue, int, setNodeTestValue>()
    ));
    constexpr std::string_view schema = "test.render.effect.color";
    EffectCapability capability;
    capability.id = graph::nodeTypeId(schema);
    capability.canonical_name = schema;
    const EffectPortDefinition capability_ports[]{
        {node_input, "source", {HdrColorBundle::type, HdrColorBundle::version}, graph::EPinDirection::INPUT},
        {node_color, "color", {HdrColorBundle::type, HdrColorBundle::version}, graph::EPinDirection::OUTPUT, 2}
    };
    capability.ports = capability_ports;
    CHECK(server->registerEffectCapability(capability, {command.command}));

    ImplementationEntry implementation;
    implementation.capability = capability.id;
    implementation.identity = {"test.node.clear_or_forward", 1};
    implementation.supported_outputs = {node_color};
    implementation.create = createNodeTest<buildNodeTest>;
    auto selected = implementation.identity;
    auto import_implementation = implementation;
    import_implementation.identity = {"test.node.target_import", 1};
    import_implementation.create = createNodeTest<buildImportedTargetNode>;
    const auto imported_selection = import_implementation.identity;
    CHECK(server->registerEffectImplementation(std::move(implementation), {command}));
    CHECK(server->registerEffectImplementation(std::move(import_implementation), {command}));

    const auto tone_node = capabilities::ToneMapping;
    const EffectValueType display_type{kDisplayColorType, 1};
    EffectCapability tone_capture;
    constexpr std::string_view tone_capture_name = "test.render.effect.tone_capture";
    tone_capture.id = graph::nodeTypeId(tone_capture_name);
    tone_capture.canonical_name = tone_capture_name;
    const EffectPortDefinition tone_capture_ports[]{
        {tone_left, "tone_left", display_type, graph::EPinDirection::INPUT, 1, true},
        {tone_right, "tone_right", display_type, graph::EPinDirection::INPUT, 1, true},
        {node_bytes, "bytes", {"test.readback_bytes", 1}, graph::EPinDirection::OUTPUT}
    };
    tone_capture.ports = tone_capture_ports;
    CHECK(server->registerEffectCapability(tone_capture));
    ImplementationEntry tone_copy;
    tone_copy.capability = tone_capture.id;
    tone_copy.identity = {std::string{tone_capture_name}, 1};
    tone_copy.supported_outputs = {node_bytes};
    tone_copy.create = fixture::Effect<buildToneCapture>::create;
    const auto tone_capture_selection = tone_copy.identity;
    CHECK(server->registerEffectImplementation(std::move(tone_copy)));
    graph::NodeId tone_left_node, tone_right_node;
    CreateEffectRequest tone_request;
    tone_request.graph = std::make_unique<EffectGraph>();
    {
        EffectGraphEdit tone_edit{*tone_request.graph};
        const auto hdr_input = take(tone_edit.addNode(capability, selected, "tone input"));
        const auto left_tone = take(tone_edit.addNode(tone_node, {std::string{"lux.render.tone_mapping.linear"}, 1}, "exposure=-1"));
        const auto right_tone = take(tone_edit.addNode(tone_node, {std::string{"lux.render.tone_mapping.srgb"}, 1}, "exposure=1"));
        tone_left_node = left_tone;
        tone_right_node = right_tone;
        const auto sink = take(tone_edit.addNode(tone_capture, tone_capture_selection, ""));
        for (const auto [node, port] : {std::pair{left_tone, tone_left}, std::pair{right_tone, tone_right}})
        {
            CHECK(tone_edit.connect(tone_edit.pinId(hdr_input, node_color), tone_edit.pinId(node, ToneMappingPorts::hdr_color.semantic)));
            CHECK(tone_edit.connect(tone_edit.pinId(node, ToneMappingPorts::color.semantic), tone_edit.pinId(sink, port)));
        }
        CHECK(tone_edit.setOutput("bytes", tone_edit.pinId(sink, node_bytes)));
        CHECK(tone_edit.commit());
    }

    CreateEffectRequest request;
    request.graph = std::make_unique<EffectGraph>();
    EffectGraphEdit edit{*request.graph};
    const auto producer = take(edit.addNode(capability, selected, "producer"));
    const auto forward = take(edit.addNode(capability, selected, "forward"));
    const auto dead = take(edit.addNode(capability, selected, "bad config"));
    CHECK(edit.connect(edit.pinId(producer, node_color), edit.pinId(forward, node_input)));
    CHECK(edit.setOutput("display", edit.pinId(forward, node_color), true));
    CHECK(edit.commit());

    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    RenderResult<void> server_result;
    std::thread render([&] { server_result = server->run(); });
    const auto commands = queryCommands(*client);
    enum class ERoundRequest { SEPARATE, WINDOW, WINDOW_THEN_STOP };
    const auto requestRound = [&](
        std::span<const RenderJobId> jobs, ERoundRequest mode = ERoundRequest::SEPARATE,
        WindowUpdates updates = {}, lux::error::ErrorId expected_error = {}
    )
    {
        const auto stats_command = std::ranges::find(commands.commands, QueryRenderStats::name, &CommandInfo::name);
        CHECK(stats_command != commands.commands.end());
        const auto stats_id = nextSceneRequest();
        ipc::IpcMessage wake;
        wake.emplace<QueryRenderStats::Request>();
        // A prior reply can already be visible while its notification is still entering this gate.
        response_gate.store(EResponseGate::ARMED);
        CHECK(client->trySend(commandTag(stats_command->id), stats_id, wake));
        const auto pair_deadline = Clock::now() + 10s;
        while (response_gate.load() != EResponseGate::BLOCKED) CHECK(Clock::now() < pair_deadline);
        const auto request_command = std::ranges::find(commands.commands, RequestRenderJob::name, &CommandInfo::name);
        CHECK(request_command != commands.commands.end());
        std::vector<std::uint64_t> request_ids;
        std::optional<std::uint64_t> stop_reply;
        if (mode != ERoundRequest::SEPARATE)
        {
            RequestRenderJob::Request window{jobs.front(), std::move(updates)};
            for (auto job : jobs.subspan(1)) window.additional_runs.push_back({job, {nextSceneRequest()}});
            const auto lead = nextSceneRequest();
            window.forEachRun({lead}, [&](const auto& run) { request_ids.push_back(run.reply.value); });
            ipc::IpcMessage message;
            message.emplace<RequestRenderJob::Request>(std::move(window));
            CHECK(client->trySend(commandTag(request_command->id), lead, message));
            if (mode == ERoundRequest::WINDOW_THEN_STOP) stop_reply = sendSceneRequest<Stop>(*client, commands, {});
        }
        else
        {
            for (auto job : jobs)
            {
                ipc::IpcMessage message;
                RequestRenderJob::Request request{job};
                if (job == jobs.front()) request.updates = std::move(updates);
                message.emplace<RequestRenderJob::Request>(std::move(request));
                request_ids.push_back(nextSceneRequest());
                CHECK(client->trySend(commandTag(request_command->id), request_ids.back(), message));
            }
        }
        response_gate.store(EResponseGate::OPEN);
        response_gate.notify_all();
        std::vector<JobSubmitted> paired(jobs.size());
        std::optional<std::uint64_t> paired_before;
        for (std::size_t received = 0; received < paired.size() || !paired_before || stop_reply;)
        {
            ipc::IpcMessage message;
            std::uint64_t correlation = 0;
            CHECK(Clock::now() < pair_deadline);
            if (client->tryReceive(correlation, message) != ipc::EIpcReceive::RECEIVED)
            {
                std::this_thread::yield();
                continue;
            }
            if (correlation == stats_id)
            {
                const auto& stats = message.get<QueryRenderStats::Response>();
                CHECK(!paired_before && !stats.error.type);
                paired_before = stats.submissions;
                continue;
            }
            if (stop_reply == correlation)
            {
                CHECK(!message.get<Stop::Response>().error.type);
                stop_reply.reset();
                continue;
            }
            auto& reply = message.get<JobSubmitted>();
            CHECK(reply.error.type == expected_error);
            const auto job = std::ranges::find(jobs, reply.job);
            CHECK(job != jobs.end());
            const auto index = static_cast<std::size_t>(job - jobs.begin());
            CHECK(correlation == request_ids[index] && paired[index].job.isNull());
            paired[index] = std::move(reply);
            ++received;
        }
        for (std::size_t i = 0; i < jobs.size(); ++i)
        {
            CHECK(paired[i].job == jobs[i] && paired[i].frame_index == paired.front().frame_index);
            CHECK(paired[i].slot_index == paired.front().slot_index);
        }
        return std::pair{*paired_before, std::move(paired)};
    };
    request.output = {rdesc::ETextureFormat::RGBA16_SFLOAT, 1, ETargetKind::OFFSCREEN, true};
    tone_request.output.sampled = true;
    const auto effect = call<CreateEffect>(*client, commands, std::move(request)).effect;
    const auto tone_effect = call<CreateEffect>(*client, commands, std::move(tone_request)).effect;
    call<SetToneMappingExposure>(*client, commands, {tone_effect, tone_left_node, -2.F});
    call<SetNodeTestValue>(*client, commands, {effect, producer, 5, 7});
    call<SetNodeTestValue>(*client, commands, {effect, forward, 10, 13});
    call<SetNodeTestValue>(*client, commands, {effect, producer, 7, 11});
    CHECK(call<SetNodeTestValue>(*client, commands, {effect, dead, 0, 1}, false).error.type == kCommandContract);

    DeviceOptions options;
    options.texture_descriptors = true;
    if (async_uniform)
    {
        options.multiple_queues = true;
        options.graph_queue_costs = {1'000, 1'000, 1'000};
    }
    if (d3d12) options.backend = EGraphicsBackend::D3D12;
    const auto device = call<CreateDeviceSession>(*client, commands, options).device;
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto view = call<CreateView>(*client, commands, {scene}).view;
    constexpr auto hdr = rdesc::ETextureFormat::RGBA16_SFLOAT;
    const TargetDesc first_desc{96, 64, hdr, 1, ETargetKind::OFFSCREEN, true};
    const TargetDesc peer_desc{64, 64, hdr, 1, ETargetKind::OFFSCREEN, true};
    const auto target = call<CreateTarget>(*client, commands, {device, first_desc}).target;
    const auto peer_target = call<CreateTarget>(*client, commands, {device, peer_desc}).target;
    const auto first = call<CreateRenderJob>(*client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
    const auto peer = call<CreateRenderJob>(*client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
    auto prepare_client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(prepare_client);
    const auto begin_prepare = [&](EffectId preparing, DeviceSessionId selected_device)
    {
        const auto before = node_builds.load();
        node_build_gate.store(ENodeBuildGate::PAUSE);
        auto response = std::async(std::launch::async, [&, preparing, selected_device]
        {
            return call<PrepareEffect>(*prepare_client, commands, {preparing, selected_device}, false);
        });
        const auto deadline = Clock::now() + 10s;
        while (node_builds.load() == before)
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
        CHECK(call<PrepareEffect>(*client, commands, {preparing, selected_device}, false).error.type == kDeviceBusy);
        return response;
    };
    const auto release_prepare = [](ENodeBuildGate outcome)
    {
        node_build_gate.store(outcome, std::memory_order_release);
        node_build_gate.notify_all();
    };
    CHECK(call<StartRenderJob>(*client, commands, {device, first, target}, false).error.type == kJobState);
    auto failed = begin_prepare(effect, device);
    call<ResizeTarget>(*client, commands, {peer_target, peer_desc});
    release_prepare(ENodeBuildGate::FAIL);
    CHECK(failed.get().error.type == kFrameContract && node_builds.load() == 1);
    auto retry = begin_prepare(effect, device);
    call<StopRenderJob>(*client, commands, first);
    release_prepare(ENodeBuildGate::PASS);
    CHECK(!retry.get().error.type && node_builds.load() == 3);
    call<PrepareEffect>(*client, commands, {effect, device});
    call<StartRenderJob>(*client, commands, {device, first, target});
    call<StartRenderJob>(*client, commands, {device, peer, peer_target});
    renderFrame(*client, commands, first);
    renderFrame(*client, commands, peer);
    verifyTargetInput(*client, commands, device, scene, {}, effect);
    CHECK(node_builds.load() == 3);
    const auto other_device = call<CreateDeviceSession>(*client, commands, options).device;
    auto other_prepare = begin_prepare(effect, other_device);
    call<PrepareEffect>(*client, commands, {effect, device});
    CHECK(call<RemoveDeviceSession>(*client, commands, other_device, false).error.type == kDeviceBusy);
    renderFrame(*client, commands, first);
    release_prepare(ENodeBuildGate::FAIL);
    CHECK(other_prepare.get().error.type == kFrameContract);
    node_build_gate.store(ENodeBuildGate::PASS);
    call<PrepareEffect>(*client, commands, {effect, other_device});
    call<RemoveDeviceSession>(*client, commands, other_device);
    renderFrame(*client, commands, peer);

    CreateEffectRequest imported_request;
    imported_request.graph = std::make_unique<EffectGraph>();
    EffectGraphEdit imported_edit{*imported_request.graph};
    const auto imported_node = take(imported_edit.addNode(capability, imported_selection, "producer"));
    CHECK(imported_edit.setOutput("display", imported_edit.pinId(imported_node, node_color), true));
    CHECK(imported_edit.commit());
    imported_request.output = {rdesc::ETextureFormat::RGBA16_SFLOAT, 1, ETargetKind::OFFSCREEN, true};
    const auto imported_effect = createPreparedEffect(*client, commands, device, std::move(imported_request));
    const auto imported_target = call<CreateTarget>(*client, commands, {device, peer_desc}).target;
    const auto imported_job =
        call<CreateRenderJob>(*client, commands, {scene, view, imported_effect, EJobCadence::REQUESTED}).job;
    const auto bound = [](TargetId a, TargetId b, EProductRead read = EProductRead::LATEST_PUBLISHED)
    {
        JobBinding binding;
        // Deliberately out of name order.
        binding.target_images = {{b, "second", read}, {a, "first", read}};
        return binding;
    };
    CHECK(call<StartRenderJob>(*client, commands, {device, imported_job, imported_target}, false).error.type ==
        kRuntimeIdentity);
    auto duplicate = bound(target, peer_target);
    duplicate.target_images.front().binding = "first";
    CHECK(call<StartRenderJob>(*client, commands, {device, imported_job, imported_target, duplicate}, false).error.type ==
        kCommandContract);
    auto wrong_name = bound(target, peer_target);
    wrong_name.target_images.front().binding = "unknown";
    CHECK(call<StartRenderJob>(*client, commands, {device, imported_job, imported_target, wrong_name}, false).error.type ==
        kRuntimeIdentity);
    const auto wrong_format = call<CreateTarget>(*client, commands, {
        device, {64, 64, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN, true}
    }).target;
    CHECK(call<StartRenderJob>(
        *client, commands, {device, imported_job, imported_target, bound(wrong_format, target)}, false
    ).error.type == kDeviceArgument);
    call<RemoveTarget>(*client, commands, wrong_format);

    CHECK(call<StartRenderJob>(
        *client, commands, {device, imported_job, imported_target, bound(imported_target, target, EProductRead::CURRENT_RUN)}, false
    ).error.type == kCommandContract);
    call<StartRenderJob>(*client, commands, {device, imported_job, imported_target, bound(target, target, EProductRead::CURRENT_RUN)});
    const auto imported_builds = target_graph_builds.load();
    CHECK(call<RemoveTarget>(*client, commands, target, false).error.type == kDeviceBusy);
    const TargetDesc resized_desc{128, 64, hdr, 1, ETargetKind::OFFSCREEN, true};
    const auto resized = call<ResizeTarget>(*client, commands, {target, resized_desc});
    call<SetTargetActive>(*client, commands, {target, true, resized.epoch});
    CHECK(call<RequestRenderJob>(*client, commands, {imported_job}, false).error.type == kRuntimeState);
    CapturedPixels fanout_pixels;
    CreateEffectRequest fanout_request;
    fanout_request.graph = std::make_unique<EffectGraph>();
    EffectGraphEdit fanout_edit{*fanout_request.graph};
    addOutputTone(fanout_edit, true, EExpectedEncoding::LINEAR, "0");
    CHECK(fanout_edit.commit());
    const auto fanout_effect = createPreparedEffect(*client, commands, device, std::move(fanout_request));
    const auto fanout_view = call<CreateView>(*client, commands, {scene}).view;
    const auto fanout_target = call<CreateTarget>(*client, commands, {
        device, {64, 64, rdesc::ETextureFormat::RGBA8_UNORM}
    }).target;
    const auto fanout_job = call<CreateRenderJob>(*client, commands, {
        scene, fanout_view, fanout_effect, EJobCadence::REQUESTED
    }).job;
    const auto capture = GraphCaptureBuffer::bind(
        {*client, commands, device}, {scene, fanout_view}, {&fanout_pixels, rdesc::ETextureFormat::RGBA8_UNORM}
    );
    JobBinding fanout_binding;
    fanout_binding.target_images.push_back({target, std::string{hdr_binding}, EProductRead::CURRENT_RUN});
    fanout_binding.resources.push_back({std::string{ColorCapture::name}, capture});
    call<StartRenderJob>(*client, commands, {device, fanout_job, fanout_target, fanout_binding});
    // A -> B -> C: B and C request this round, but A has no demand. C must leave its setter untouched.
    auto chained = fanout_binding;
    chained.target_images.front().target = imported_target;
    call<RebindRenderJobEffect>(*client, commands, {device, fanout_job, fanout_effect, fanout_target, chained});
    WindowUpdates chain_updates;
    const auto chain_setter = std::ranges::find(commands.commands, SetNodeTestValue::name, &CommandInfo::name);
    CHECK(chain_setter != commands.commands.end());
    chain_updates.append<SetNodeTestValue>(chain_setter->id, {effect, forward, 13, 14});
    const auto rejected_chain = requestRound(
        std::array{fanout_job, imported_job}, ERoundRequest::SEPARATE, std::move(chain_updates), kRuntimeState
    ).second;
    CHECK(!rejected_chain.front().updates.empty() && rejected_chain.back().updates.empty());
    WindowUpdates complete_chain;
    complete_chain.append<SetNodeTestValue>(chain_setter->id, {effect, forward, 13, 14});
    requestRound(std::array{fanout_job, imported_job, first}, ERoundRequest::WINDOW, std::move(complete_chain));
    call<SetNodeTestValue>(*client, commands, {effect, forward, 14, 13});
    call<RebindRenderJobEffect>(*client, commands, {device, fanout_job, fanout_effect, fanout_target, fanout_binding});
    // A resized Target has no publication. Both readers must select this writer's new FIF slot.
    const auto fanout = requestRound(std::array{imported_job, fanout_job, first}).second;
    CHECK(call<RequestRenderJob>(*client, commands, {fanout_job}, false).error.type == kRuntimeState);
    fanout_binding.target_images.front().read = EProductRead::LATEST_PUBLISHED;
    call<RebindRenderJobEffect>(*client, commands, {device, fanout_job, fanout_effect, fanout_target, fanout_binding});
    const auto later = call<RequestRenderJob>(*client, commands, {fanout_job});
    CHECK(later.slot_index != fanout[1].slot_index);
    if (const auto after = server->developmentStats())
    {
        const auto stable = call<RequestRenderJob>(*client, commands, {fanout_job});
        CHECK(stable.frame_index > later.frame_index);
        const auto counters = server->developmentStats();
        CHECK(counters->input_rebuilds == after->input_rebuilds);
        CHECK(counters->job_selection_growths == after->job_selection_growths);
        CHECK(counters->job_topology_growths == after->job_topology_growths);
    }
    stopAndRemoveJob(*client, commands, fanout_job);
    call<RemoveView>(*client, commands, {scene, fanout_view});
    const auto fanout_deadline = Clock::now() + 10s;
    while (!fanout_pixels.ready.load(std::memory_order_acquire)) CHECK(Clock::now() < fanout_deadline);
    CHECK(fanout[1].slot_index < fanout_pixels.frames.size() && later.slot_index < fanout_pixels.frames.size());
    const auto& current_pixels = fanout_pixels.frames[fanout[1].slot_index];
    const auto& published_pixels = fanout_pixels.frames[later.slot_index];
    constexpr std::size_t row_bytes = 64 * 4;
    for (std::uint32_t row = 0; row < 64; ++row)
    {
        const auto offset = std::size_t(row) * fanout_pixels.pitch;
        CHECK(offset + row_bytes <= current_pixels.size() && offset + row_bytes <= published_pixels.size());
        CHECK(std::ranges::equal(
            std::span{current_pixels}.subspan(offset, row_bytes), std::span{published_pixels}.subspan(offset, row_bytes)
        ));
    }
    CHECK(current_pixels[0] != std::byte{} && current_pixels[1] != std::byte{} && current_pixels[3] == std::byte{255});
    call<RemoveTarget>(*client, commands, fanout_target);
    call<RemoveEffect>(*client, commands, {fanout_effect});
    std::puts("ROUND OUTPUT PASS: resized writer plus two readers, same-round and later published pixels match");
    CreateEffectRequest dim_request;
    dim_request.graph = std::make_unique<EffectGraph>();
    EffectGraphEdit dim_edit{*dim_request.graph};
    const auto dim_node = take(dim_edit.addNode(capability, selected, "dim"));
    CHECK(dim_edit.setOutput("display", dim_edit.pinId(dim_node, node_color), true));
    CHECK(dim_edit.commit());
    dim_request.output = {rdesc::ETextureFormat::RGBA16_SFLOAT, 1, ETargetKind::OFFSCREEN, true};
    const auto dim_effect = createPreparedEffect(*client, commands, device, std::move(dim_request));
    auto mixed = bound(target, target);
    mixed.target_images.front().read = EProductRead::CURRENT_RUN;
    call<RebindRenderJobEffect>(*client, commands, {device, imported_job, imported_effect, imported_target, mixed});
    call<RebindRenderJobEffect>(*client, commands, {device, first, dim_effect, target, {}});
    requestRound(std::array{imported_job, first});
    verifyTargetInput(*client, commands, device, scene, {}, imported_effect,
        bound(imported_target, imported_target), {.1875, .4375, .6875});
    call<RebindRenderJobEffect>(*client, commands, {device, first, effect, target, {}});
    renderFrame(*client, commands, first);
    std::puts("PRODUCT VERSIONS PASS: both bindings of one Target sampled its old and new pixels in one round");

    const auto dim_target = call<CreateTarget>(*client, commands, {device, peer_desc}).target;
    const auto dim_job = startRenderJob(*client, commands, device, scene, view, dim_target, dim_effect);
    renderFrame(*client, commands, dim_job);
    stopAndRemoveJob(*client, commands, dim_job);
    call<RebindRenderJobEffect>(*client, commands, {
        device, imported_job, imported_effect, imported_target, bound(dim_target, dim_target)
    });
    auto incompatible = peer_desc;
    incompatible.format = rdesc::ETextureFormat::RGBA8_UNORM;
    const auto rejected = call<ResizeTarget>(*client, commands, {dim_target, incompatible}, false);
    CHECK(rejected.error.type == kDeviceArgument && rejected.description == peer_desc);
    renderFrame(*client, commands, imported_job);
    verifyTargetInput(*client, commands, device, scene, {}, imported_effect,
        bound(imported_target, imported_target), {.125, .375, .625});
    CHECK(target_graph_builds.load() == imported_builds);
    verifyTargetInput(*client, commands, device, scene, {}, imported_effect,
        bound(target, dim_target), {.1875, .4375, .6875});
    CHECK(target_graph_builds.load() == imported_builds);
    stopAndRemoveJob(*client, commands, imported_job);
    call<RemoveTarget>(*client, commands, dim_target);
    call<RemoveEffect>(*client, commands, {dim_effect});
    call<RemoveTarget>(*client, commands, imported_target);
    call<RemoveEffect>(*client, commands, {imported_effect});
    std::puts("NAMED TARGET IMPORT PASS: native readback, alias key, binding checks and source resize");

    const auto make_storage_effect = [&](const ImplementationSelection& selection, std::string_view arguments)
    {
        CreateEffectRequest request;
        request.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*request.graph};
        request.output = {rdesc::ETextureFormat::RGBA16_SFLOAT, 1, ETargetKind::OFFSCREEN, true};
        const auto node = take(edit.addNode(storage_capability, selection, std::string{arguments}));
        // The unconnected node consumes a local View when live. These Jobs bind only their Buffer inputs.
        if (selection == storage_selection && arguments.empty())
            take(edit.addNode(storage_capability, producer_selection));
        CHECK(edit.setOutput("readback", edit.pinId(node, node_bytes), false));
        CHECK(edit.commit());
        return call<CreateEffect>(*client, commands, std::move(request)).effect;
    };
    const auto storage_effect = make_storage_effect(storage_selection, "");
    const auto wrong_shape_effect = make_storage_effect(storage_selection, "wrong-shape");
    const auto wrong_type_effect = make_storage_effect(storage_selection, "wrong-type");
    for (auto selected : {storage_effect, wrong_shape_effect, wrong_type_effect})
        call<PrepareEffect>(*client, commands, {selected, device});
    const auto make_storage_job = [&](EffectId selected_effect)
    {
        return call<CreateRenderJob>(*client, commands, {scene, view, selected_effect, EJobCadence::REQUESTED}).job;
    };
    const auto owner_view = call<CreateView>(*client, commands, {scene}).view;
    const auto other_view = call<CreateView>(*client, commands, {scene}).view;
    const auto make_resource = [&](ViewId owner, ResourceNameView name)
    {
        return call<GetOrCreateResource>(*client, commands, {device, ViewScope{scene, owner}, name}).resource_id;
    };
    const auto owner = make_resource(owner_view, EffectCaptureDescriptor::descriptor.type);
    const auto other_owner = make_resource(other_view, EffectCaptureDescriptor::descriptor.type);
    const auto legacy_owner = make_resource(view, CaptureDescriptor::descriptor.type);
    const auto storage_job = make_storage_job(storage_effect);
    const auto other_storage_job = make_storage_job(storage_effect);
    const auto wrong_shape_job = make_storage_job(wrong_shape_effect);
    const auto wrong_type_job = make_storage_job(wrong_type_effect);
    const auto storage_binding = [](ResourceInstanceId first, ResourceInstanceId second)
    {
        JobBinding binding;
        binding.resources = {{"second", second}, {"first", first}};
        return binding;
    };
    const auto storage_output = call<CreateTarget>(*client, commands, {device, peer_desc}).target;
    CHECK(call<StartRenderJob>(*client, commands, {device, storage_job, storage_output}, false).error.type ==
        kStorageIdentity);
    auto duplicate_storage = storage_binding(owner, owner);
    duplicate_storage.resources.front().binding = "first";
    CHECK(call<StartRenderJob>(
        *client, commands, {device, storage_job, storage_output, duplicate_storage}, false
    ).error.type == kCommandContract);
    CHECK(call<StartRenderJob>(
        *client, commands, {device, storage_job, storage_output, storage_binding(legacy_owner, legacy_owner)}, false
    ).error.type == kStorageMismatch);
    // This Effect names the actual Resource type. Its factory supplies no graph projection.
    CHECK(call<StartRenderJob>(
        *client, commands, {device, wrong_type_job, storage_output, storage_binding(legacy_owner, legacy_owner)}, false
    ).error.type == kStorageContract);
    for (auto rejected : {wrong_shape_job, wrong_type_job})
    {
        CHECK(call<StartRenderJob>(
            *client, commands, {device, rejected, storage_output, storage_binding(owner, owner)}, false
        ).error.type == kStorageMismatch);
        call<RemoveRenderJob>(*client, commands, {rejected});
    }
    const auto before_valid_storage = storage_node_builds.load();
    call<StartRenderJob>(*client, commands, {device, storage_job, storage_output, storage_binding(owner, owner)});
    // A second Job binds another physical owner with the same import contract.
    const auto other_storage_output = call<CreateTarget>(*client, commands, {device, peer_desc}).target;
    CHECK(call<StartRenderJob>(
        *client, commands, {device, other_storage_job, other_storage_output, storage_binding(owner, owner)}, false
    ).error.type == kJobBusy);
    const auto owner_address = call<QueryResource>(*client, commands, {
        device, ViewScope{scene, owner_view}, EffectCaptureDescriptor::descriptor.type
    }).address();
    const auto users = call<ListResourceJobs>(*client, commands, owner_address).jobs;
    CHECK(users.size() == 1 && users.front() == storage_job);
    CHECK(call<RemoveResource>(*client, commands, owner_address, false).error.type == kDeviceBusy);
    call<StartRenderJob>(
        *client, commands, {device, other_storage_job, other_storage_output, storage_binding(other_owner, other_owner)}
    );
    CHECK(storage_node_builds.load() == before_valid_storage);
    CHECK(call<RemoveView>(*client, commands, {scene, owner_view}, false).error.type == kDeviceBusy);
    const auto storage_receipt = call<RequestRenderJob>(*client, commands, {storage_job});
    const auto other_receipt = call<RequestRenderJob>(*client, commands, {other_storage_job});
    stopAndRemoveJob(*client, commands, other_storage_job);
    call<RebindRenderJobEffect>(
        *client, commands, {device, storage_job, storage_effect, storage_output, storage_binding(owner, other_owner)}
    );
    CHECK(storage_node_builds.load() == before_valid_storage);
    const auto distinct_receipt = call<RequestRenderJob>(*client, commands, {storage_job});
    stopAndRemoveJob(*client, commands, storage_job);
#ifdef _WIN32
    if (async_uniform && UniformUploadProbe::hasSeparateComputeQueue())
    {
        const auto list_effect = make_storage_effect(list_selection, "");
        call<PrepareEffect>(*client, commands, {list_effect, device});
        TargetId source{}, suffix_source{};
        for (const bool reject_suffix : {false, true})
        {
            CapturedPixels pixels;
            const auto capture_view = call<CreateView>(*client, commands, {scene}).view;
            const CaptureDevice capture_device{*client, commands, device};
            const auto capture = GraphCaptureBuffer::bind(capture_device, {scene, capture_view}, {&pixels});
            const auto reader = make_storage_job(list_effect);
            JobBinding binding;
            binding.resources.push_back({"capture", capture});
            call<StartRenderJob>(*client, commands, {device, reader, other_storage_output, binding});
            RenderJobId writer{};
            if (!reject_suffix)
            {
                const auto create_source = [&](TargetReadProbe::Images& bank)
                {
                    TargetReadProbe::capturing = &bank;
                    const auto target = call<CreateTarget>(*client, commands, {device, peer_desc}).target;
                    TargetReadProbe::capturing = nullptr;
                    CHECK(bank.created == config.frames_in_flight + 1);
                    return target;
                };
                source = create_source(TargetReadProbe::banks[0]);
                suffix_source = create_source(TargetReadProbe::banks[1]);
                const auto peer_writer = startRenderJob(*client, commands, device, scene, view, suffix_source, effect);
                renderFrame(*client, commands, peer_writer);
                stopAndRemoveJob(*client, commands, peer_writer);
                writer = startRenderJob(*client, commands, device, scene, view, source, effect);
            }
            const auto mode = reject_suffix ? EProductRead::LATEST_PUBLISHED : EProductRead::CURRENT_RUN;
            WindowUpdates updates;
            for (const std::string name : {"samples", "overlay"})
            {
                const auto address = call<QueryResource>(*client, commands, {
                    device, reader, TargetDrawList::descriptor.type, name
                }).address();
                auto packet = take(FrameData::create<std::tuple<TargetDrawList>>({address}));
                CHECK(packet.addData<TargetDrawList>(RasterTargetImage{source, mode}));
                const bool suffix_only = name == "overlay" && reject_suffix;
                CHECK(packet.addData<TargetDrawList>(RasterTargetImage{suffix_only ? suffix_source : source, mode}));
                updates.append(std::move(packet));
            }
            std::uint32_t sampled_slot = kGraphInvalidIndex;
            std::optional<RetirementFence> gate;
            if (!reject_suffix)
            {
                const auto before = server->developmentStats();
                const auto paired = requestRound(std::array{reader, writer}, ERoundRequest::WINDOW, std::move(updates));
                sampled_slot = paired.second.front().slot_index;
                if (before) CHECK(server->developmentStats()->dependency_edges == before->dependency_edges + 1);
                stopAndRemoveJob(*client, commands, writer);
            }
            else
            {
                gate.emplace(*TargetDrawList::session);
                TargetReadProbe::gate = gate->fence;
                const auto reply = call<RequestRenderJob>(*client, commands, {reader, std::move(updates)}, false);
                TargetReadProbe::gate = nullptr;
                CHECK(reply.error.type == kDeviceFailure);
                CHECK(TargetReadProbe::draws == 2 && TargetReadProbe::submissions == 2);
                const auto& point = TargetReadProbe::first_read;
                CHECK(point.fence && gate->core.GetFenceValue(*point.fence) < point.value);
                call<RemoveTarget>(*client, commands, source);
                call<RemoveTarget>(*client, commands, suffix_source);
                const auto deadline = Clock::now() + 10s;
                const auto& suffix = TargetReadProbe::banks[1];
                while (suffix.destroyed != suffix.created) CHECK(Clock::now() < deadline);
                CHECK(!TargetReadProbe::banks[0].destroyed && gate->core.GetFenceValue(*point.fence) < point.value);
                gate->signal();
            }
            stopAndRemoveJob(*client, commands, reader);
            call<RemoveView>(*client, commands, {scene, capture_view});
            const auto deadline = Clock::now() + 10s;
            while (!pixels.ready.load(std::memory_order_acquire)) CHECK(Clock::now() < deadline);
            if (!reject_suffix)
            {
                CHECK(sampled_slot < pixels.frames.size());
                const auto& bytes = pixels.frames[sampled_slot];
                CHECK(bytes[0] != std::byte{} && bytes[1] != std::byte{} && bytes[3] == std::byte{255});
            }
            else
            {
                const auto& prefix = TargetReadProbe::banks[0];
                while (prefix.destroyed != prefix.created) CHECK(Clock::now() < deadline);
            }
        }
        call<RemoveEffect>(*client, commands, {list_effect});
        std::puts("DYNAMIC TARGET PREFIX PASS: one current-run edge; suffix-only images retired before the held read");
    }
    if (!async_uniform)
    {
        const auto writer_effect = make_storage_effect(storage_selection, "publish");
        const auto reader_effect = make_storage_effect(storage_selection, "consume");
        for (const auto id : {writer_effect, reader_effect}) call<PrepareEffect>(*client, commands, {id, device});
        const auto writer = make_storage_job(writer_effect);
        const auto reader = make_storage_job(reader_effect);
        CapturedPixels words;
        const auto capture_view = call<CreateView>(*client, commands, {scene}).view;
        const auto destination = GraphCaptureBuffer::bind({*client, commands, device}, {scene, capture_view}, {&words});
        JobBinding writer_binding;
        writer_binding.resources.push_back({"words", owner, 1});
        call<StartRenderJob>(*client, commands, {device, writer, storage_output, writer_binding});
        RetirementFence gate{*EffectCaptureBuffer::product_device};
        UniformUploadProbe::next_product_wait = gate.fence;
        const auto published = call<RequestRenderJob>(*client, commands, {writer});
        const auto& signal = UniformUploadProbe::product_signal;
        CHECK(!UniformUploadProbe::next_product_wait && signal.fence);
        CHECK(gate.core.GetFenceValue(*signal.fence) < signal.value);
        JobBinding reader_binding;
        reader_binding.resources = {{"words", owner, 1, published.bufferVersion({0, 16})}, {"capture", destination}};
        auto reading = std::async(std::launch::async, [&]
        {
            stopAndRemoveJob(*client, commands, writer);
            call<StartRenderJob>(*client, commands, {device, reader, other_storage_output, reader_binding});
            return call<RequestRenderJob>(*client, commands, {reader});
        });
        const bool admitted = reading.wait_for(5s) == std::future_status::ready;
        const bool producer_pending = gate.core.GetFenceValue(*signal.fence) < signal.value;
        gate.signal();
        const auto consumed = reading.get();
        CHECK(admitted && producer_pending && consumed.slot_index != published.slot_index);
        stopAndRemoveJob(*client, commands, reader);
        call<RemoveView>(*client, commands, {scene, capture_view});
        const auto deadline = Clock::now() + 10s;
        while (!words.ready.load(std::memory_order_acquire)) CHECK(Clock::now() < deadline);
        CHECK(consumed.slot_index < words.frames.size());
        const auto& captured = words.frames[consumed.slot_index];
        CHECK(captured.size() >= 16);
        CHECK(std::ranges::all_of(std::span{captured}.first(16), [](auto b) { return b == std::byte{0xc3}; }));
        for (const auto id : {reader_effect, writer_effect}) call<RemoveEffect>(*client, commands, {id});
        std::puts("IN-FLIGHT BUFFER PASS: writer removed and reader submitted before fence completion, 16 bytes matched");
    }
#endif
    const auto shared_write = make_storage_effect(storage_selection, "shared-write");
    CHECK(call<PrepareEffect>(*client, commands, {shared_write, device}, false).error.type == kGraphInvalidDescriptor);
    call<RemoveEffect>(*client, commands, {shared_write});
    for (const auto mode : {"ambiguous-write", "mixed-boundary"})
    {
        const auto ambiguous = make_storage_effect(storage_selection, mode);
        call<PrepareEffect>(*client, commands, {ambiguous, device});
        const auto ambiguous_job = make_storage_job(ambiguous);
        CHECK(call<StartRenderJob>(
            *client, commands, {device, ambiguous_job, storage_output, storage_binding(owner, owner)}, false
        ).error.type == kGraphConflictingOutput);
        call<RemoveRenderJob>(*client, commands, {ambiguous_job});
        call<RemoveEffect>(*client, commands, {ambiguous});
    }
    CHECK(call<ListResourceJobs>(*client, commands, owner_address).jobs.empty());
    call<RemoveResource>(*client, commands, owner_address);
    call<RemoveView>(*client, commands, {scene, owner_view});
    const auto stale_job = make_storage_job(storage_effect);
    CHECK(call<StartRenderJob>(
        *client, commands, {device, stale_job, storage_output, storage_binding(owner, owner)}, false
    ).error.type == kStorageIdentity);
    call<RemoveRenderJob>(*client, commands, {stale_job});
    call<RemoveView>(*client, commands, {scene, other_view});
    const auto deadline = Clock::now() + 10s;
    const std::array receipt_slots{storage_receipt.slot_index, other_receipt.slot_index};
    for (std::size_t i = 0; i < receipt_slots.size(); ++i)
    {
        auto& capture = EffectCaptureBuffer::pixels[i];
        while (!capture.ready.load(std::memory_order_acquire)) CHECK(Clock::now() < deadline);
        const auto check_capture = [&](std::uint32_t slot, std::byte expected)
        {
            CHECK(slot < capture.frames.size());
            const auto frame = std::span{capture.frames[slot]}.first(effect_capture_bytes);
            CHECK(std::ranges::all_of(frame, [expected](std::byte byte) noexcept { return byte == expected; }));
        };
        if (receipt_slots[i] != distinct_receipt.slot_index)
            check_capture(receipt_slots[i], std::byte{0x5a});
        check_capture(distinct_receipt.slot_index, std::byte{0x6b});
    }
    call<RemoveTarget>(*client, commands, storage_output);
    call<RemoveTarget>(*client, commands, other_storage_output);
    call<RemoveEffect>(*client, commands, {storage_effect});
    call<RemoveEffect>(*client, commands, {wrong_shape_effect});
    call<RemoveEffect>(*client, commands, {wrong_type_effect});
    std::puts("NAMED STORAGE IMPORT PASS: native bytes, binding qualification, variant reuse and owner lifetime");

    const auto producer_effect = make_storage_effect(producer_selection, "");
    const auto producer_view = call<CreateView>(*client, commands, {scene}).view;
    const auto producer_peer_view = call<CreateView>(*client, commands, {scene}).view;
    const auto first_data = make_resource(producer_view, EffectCaptureDescriptor::descriptor.type);
    const auto peer_data = make_resource(producer_peer_view, EffectCaptureDescriptor::descriptor.type);
    const auto producer_target = call<CreateTarget>(*client, commands, {device, peer_desc}).target;
    const auto producer_peer_target = call<CreateTarget>(*client, commands, {device, peer_desc}).target;
    const auto producer_binding = [](ResourceInstanceId resource)
    {
        JobBinding binding;
        binding.resources = {{"readback", resource}};
        return binding;
    };
    const std::pair<std::string_view, lux::error::ErrorId> invalid_preparations[]{
        {"input-count", kGraphInvalidDescriptor},
        {"output-count", kGraphInvalidDescriptor},
        {"copy-type", kGraphInvalidDescriptor},
        {"copy-source", kGraphInvalidDescriptor},
        {"copy-and-producer", kGraphAmbiguousProducer},
        {"joint-closure", kGraphUndefinedRead}
    };
    for (const auto [mode, error] : invalid_preparations)
    {
        const auto invalid = make_storage_effect(producer_selection, mode);
        CHECK(call<PrepareEffect>(*client, commands, {invalid, device}, false).error.type == error);
        call<RemoveEffect>(*client, commands, {invalid});
    }
    const auto source_before = source_links.load();
    const auto prepared_job = call<CreateRenderJob>(
        *client, commands, {scene, producer_view, producer_effect, EJobCadence::REQUESTED}
    ).job;
    const auto prepared_peer = call<CreateRenderJob>(
        *client, commands, {scene, producer_peer_view, producer_effect, EJobCadence::REQUESTED}
    ).job;
    ViewGpuParameters first_view_parameters, peer_view_parameters;
    first_view_parameters.background[0] = std::bit_cast<float>(0x3c3c3c3cu);
    peer_view_parameters.background[0] = std::bit_cast<float>(0x5b5b5b5bu);
    call<UpdateView>(*client, commands, {{scene, producer_view}, first_view_parameters});
    call<UpdateView>(*client, commands, {{scene, producer_peer_view}, peer_view_parameters});
    const auto first_view_data = make_resource(producer_view, kViewParameterResourceName);
    const auto peer_view_data = make_resource(producer_peer_view, kViewParameterResourceName);
    auto first_binding = producer_binding(first_data);
    first_binding.resources.push_back({
        std::string{kViewParameters.binding}, first_view_data, 0
    });
    auto peer_binding = producer_binding(peer_data);
    peer_binding.resources.push_back({
        std::string{kViewParameters.binding}, peer_view_data, 0
    });
    reject_view_link = true;
    const auto failed_link = call<PrepareEffect>(*client, commands, {producer_effect, device}, false);
    CHECK(failed_link.error.type == kGraphInvalidDescriptor && !reject_view_link.load());
    CHECK(source_links.load() == source_before + 1);
    CHECK(!source_instances.load() && producer_source_lifetime.expired());
    call<PrepareEffect>(*client, commands, {producer_effect, device});
    JobBinding missing_readback;
    missing_readback.resources.push_back(first_binding.resources.back());
    CHECK(call<StartRenderJob>(*client, commands, {device, prepared_job, producer_target, missing_readback}, false)
        .error.type == kStorageIdentity);
    first_binding.resources.back().resource = peer_view_data;
    const auto mismatched = call<StartRenderJob>(
        *client, commands, {device, prepared_job, producer_target, first_binding}, false
    );
    CHECK(mismatched.error.type == kStorageMismatch);
    first_binding.resources.back().resource = first_view_data;
    call<StartRenderJob>(*client, commands, {device, prepared_job, producer_target, producer_binding(first_data)});
    reject_view_workspace = true;
    const auto rejected_workspace = call<StartRenderJob>(
        *client, commands, {device, prepared_peer, producer_peer_target, peer_binding}, false
    );
    CHECK(rejected_workspace.error.type == kDeviceCapacity && !reject_view_workspace.load());
    CHECK(source_instances.load() == 2 && source_workspaces_destroyed.load() == 1);
    call<StartRenderJob>(*client, commands, {device, prepared_peer, producer_peer_target, peer_binding});
    CHECK(source_links.load() == source_before + 2);
    CHECK(source_instances.load() == 3 && source_workspaces_destroyed.load() == 1);
    auto packet = framePacket<ViewParameterResource>(*client, commands, device, ViewScope{scene, producer_view});
    auto changed_view = first_view_parameters;
    changed_view.background[0] = std::bit_cast<float>(0x4d4d4d4du);
    auto invalid_view = changed_view;
    invalid_view.direction_far[3] = 0;
    CHECK(packet.addData<ViewParameterResource>(changed_view));
    CHECK(packet.addData<ViewParameterResource>(invalid_view));
    auto rejected_packet = call<UpdateFrameData>(*client, commands, {std::move(packet)}, false);
    CHECK(rejected_packet.error.type == kFrameContract);
    packet = std::move(rejected_packet.data);
    auto wrong_camera = first_binding;
    wrong_camera.resources.back().resource = peer_view_data;
    const auto rejected_camera = call<RebindRenderJobEffect>(
        *client, commands, {device, prepared_job, producer_effect, producer_target, wrong_camera}, false
    );
    CHECK(rejected_camera.error.type == kStorageMismatch);
    using SubmitGate = UniformUploadProbe::EPipelineGate;
    auto frame_client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(frame_client);
    UniformUploadProbe::submission_gate = SubmitGate::ARMED;
    auto frozen_view = std::async(std::launch::async, [&]
    {
        return call<RequestRenderJob>(*frame_client, commands, {prepared_job});
    });
    const auto freeze_deadline = Clock::now() + 10s;
    while (UniformUploadProbe::submission_gate != SubmitGate::HELD)
    {
        CHECK(Clock::now() < freeze_deadline);
        std::this_thread::yield();
    }
    call<UpdateView>(*client, commands, {{scene, producer_view}, changed_view});
    UniformUploadProbe::submission_gate = SubmitGate::OPEN;
    UniformUploadProbe::submission_gate.notify_all();
    const auto prepared_first = frozen_view.get();
    reject_value_preparation = true;
    CHECK(call<RequestRenderJob>(*client, commands, {prepared_job}, false).error.type == kFrameContract);
    CHECK(view_preparations.load() == 1);
    reject_staged_constants = true;
    CHECK(call<RequestRenderJob>(*client, commands, {prepared_job}, false).error.type == kFrameContract);
    CHECK(!reject_staged_constants.load() && view_preparations.load() == 2);
    packet.reset();
    CHECK(packet.addData<ViewParameterResource>(changed_view));
    const auto prepared_recovered = call<RequestRenderJob>(*client, commands, {prepared_job, std::move(packet)});
    const auto prepared_peer_first = call<RequestRenderJob>(*client, commands, {prepared_peer});
    CHECK(view_preparations.load() == 4);
    stopAndRemoveJob(*client, commands, prepared_job);
    stopAndRemoveJob(*client, commands, prepared_peer);
    CHECK(source_workspaces_destroyed.load() == source_instances.load());
    call<RemoveView>(*client, commands, {scene, producer_view});
    call<RemoveView>(*client, commands, {scene, producer_peer_view});
    const auto prepare_deadline = Clock::now() + 10s;
    const auto verify_prepared = [&](std::size_t index, std::uint32_t slot, std::byte expected)
    {
        const auto& capture = EffectCaptureBuffer::pixels[index];
        while (!capture.ready.load(std::memory_order_acquire)) CHECK(Clock::now() < prepare_deadline);
        CHECK(slot < capture.frames.size());
        const auto frame = std::span{capture.frames[slot]}.first(effect_capture_bytes);
        CHECK(std::ranges::all_of(frame, [expected](std::byte byte) noexcept { return byte == expected; }));
    };
    CHECK(prepared_first.slot_index != prepared_recovered.slot_index);
    verify_prepared(2, prepared_first.slot_index, std::byte{0x3c});
    verify_prepared(2, prepared_recovered.slot_index, std::byte{0x4d});
    verify_prepared(3, prepared_peer_first.slot_index, std::byte{0x5b});
    call<RemoveTarget>(*client, commands, producer_target);
    call<RemoveTarget>(*client, commands, producer_peer_target);
    call<RemoveEffect>(*client, commands, {producer_effect});
    std::puts("VIEW SNAPSHOTS: missing live input and foreign rebind rejected; frozen 0x3c, next 0x4d, peer 0x5b");
    std::puts("PRODUCER PASS: joint liveness, shared prototype, failure propagation, and native View parameter readback");

    const auto stream_effect = make_storage_effect(stream_selection, "");
    call<PrepareEffect>(*client, commands, {stream_effect, device});
    const auto stream_view = call<CreateView>(*client, commands, {scene}).view;
    const auto stream_data = make_resource(stream_view, EffectCaptureDescriptor::descriptor.type);
    const auto stream_target = call<CreateTarget>(*client, commands, {device, peer_desc}).target;
    const auto stream_job = call<CreateRenderJob>(*client, commands, {scene, stream_view, stream_effect, EJobCadence::REQUESTED}).job;
    const auto stream_source = make_resource(stream_view, StreamProgramStorage::descriptor.type);
    call<SetStreamPrograms>(*client, commands, {stream_source, ETestStreamPrograms::INITIAL});
    auto stream_binding = producer_binding(stream_data);
    call<StartRenderJob>(*client, commands, {device, stream_job, stream_target, std::move(stream_binding)});
    stream_mode = ETestStreamMode::BAD_CONSTANTS;
    CHECK(call<RequestRenderJob>(*client, commands, {stream_job}, false).error.type == kFrameContract);
    stream_mode = ETestStreamMode::BAD_INDEX;
    CHECK(call<RequestRenderJob>(*client, commands, {stream_job}, false).error.type == kGraphInvalidRange);
    stream_mode = ETestStreamMode::BAD_BINDING;
    CHECK(call<RequestRenderJob>(*client, commands, {stream_job}, false).error.type == kGraphInvalidRange);
    stream_mode = ETestStreamMode::BAD_IMAGE;
    CHECK(call<RequestRenderJob>(*client, commands, {stream_job}, false).error.type == kGraphInvalidRange);
    stream_mode = ETestStreamMode::BAD_SAMPLER;
    CHECK(call<RequestRenderJob>(*client, commands, {stream_job}, false).error.type == kFrameContract);
    stream_mode = ETestStreamMode::DUPLICATE_ELEMENT;
    CHECK(call<RequestRenderJob>(*client, commands, {stream_job}, false).error.type == kFrameContract);
    for (const auto mode : {ETestStreamMode::FOREIGN_BUFFER, ETestStreamMode::FOREIGN_IMAGE,
                            ETestStreamMode::PARTIAL_IMAGE_GROUP, ETestStreamMode::DUPLICATE_LAYER,
                            ETestStreamMode::MISSING_LAYER, ETestStreamMode::UNCOVERED_DRAW})
    {
        stream_mode = mode;
        CHECK(call<RequestRenderJob>(*client, commands, {stream_job}, false).error.type == kGraphInvalidRange);
    }
    for (const auto mode : {ETestStreamMode::BAD_INDIRECT_STRIDE, ETestStreamMode::SHORT_INDIRECT_ARGS,
                            ETestStreamMode::BAD_COUNT, ETestStreamMode::WRONG_INDIRECT_USE})
    {
        stream_mode = mode;
        CHECK(call<RequestRenderJob>(*client, commands, {stream_job}, false).error.type == kGraphInvalidRange);
    }
    stream_mode = ETestStreamMode::EMPTY;
    const auto empty_stream = call<RequestRenderJob>(*client, commands, {stream_job});
    const auto rendered_stream = call<RequestRenderJob>(*client, commands, {stream_job});
    const auto before_bad_program = call<QueryRenderStats>(*client, commands, {}).submissions;
    CHECK(call<SetStreamPrograms>(*client, commands, {stream_source, ETestStreamPrograms::INVALID}, false).error.type == kDeviceUnsupported);
    CHECK(call<SetStreamPrograms>(*client, commands, {stream_source, ETestStreamPrograms::BAD_GEOMETRY}, false).error.type == kGraphInvalidDescriptor);
    CHECK(call<QueryRenderStats>(*client, commands, {}).submissions == before_bad_program);

    using PipelineGate = UniformUploadProbe::EPipelineGate;
    UniformUploadProbe::pipeline_gate = PipelineGate::ARMED;
    auto requester = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(requester);
    auto preparing = std::async(std::launch::async, [&]
    {
        return call<SetStreamPrograms>(*requester, commands, {stream_source, ETestStreamPrograms::PENDING});
    });
    const auto replacement_deadline = Clock::now() + 10s;
    while (UniformUploadProbe::pipeline_gate != PipelineGate::HELD)
    {
        CHECK(Clock::now() < replacement_deadline);
        std::this_thread::yield();
    }
    CHECK(call<SetStreamPrograms>(*client, commands, {stream_source, ETestStreamPrograms::REVERSED}, false).error.type == kDeviceBusy);
    call<RequestRenderJob>(*client, commands, {stream_job});
    UniformUploadProbe::pipeline_gate = PipelineGate::OPEN;
    UniformUploadProbe::pipeline_gate.notify_all();
    preparing.get();
    call<SetStreamPrograms>(*client, commands, {stream_source, ETestStreamPrograms::REVERSED});
    stream_mode = ETestStreamMode::OLD_PROGRAM_SLOT;
    CHECK(call<RequestRenderJob>(*client, commands, {stream_job}, false).error.type == kFrameContract);
    call<SetStreamPrograms>(*client, commands, {stream_source, ETestStreamPrograms::EQUIVALENT});
    auto stream_resize = peer_desc;
    stream_resize.width *= 2;
    const auto stream_resized = call<ResizeTarget>(*client, commands, {stream_target, stream_resize});
    call<SetTargetActive>(*client, commands, {stream_target, true, stream_resized.epoch});
    stream_mode = ETestStreamMode::SWAP_TILES;
    const auto swapped_stream = call<RequestRenderJob>(*client, commands, {stream_job});
    stopAndRemoveJob(*client, commands, stream_job);
    call<RemoveView>(*client, commands, {scene, stream_view});
    const auto stream_deadline = Clock::now() + 10s;
    const auto& stream_capture = EffectCaptureBuffer::pixels[4];
    while (!stream_capture.ready.load(std::memory_order_acquire)) CHECK(Clock::now() < stream_deadline);
    const auto check_capture_pixel = [&](const CapturedPixels& capture,
                                         std::uint32_t slot, std::uint32_t layer, std::uint32_t x,
                                         std::array<unsigned, 4> rgba)
    {
        CHECK(slot < capture.frames.size());
        const auto& pixels = capture.frames[slot];
        const auto offset = (layer * 32 + 16) * capture.pitch + x * 4;
        std::array<unsigned, 4> actual{};
        for (std::size_t i = 0; i < actual.size(); ++i)
            actual[i] = std::to_integer<unsigned>(pixels[offset + i]);
        if (actual != rgba)
        {
            std::fprintf(stderr, "Raster stream pixel slot=%u layer=%u x=%u actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n",
                slot, layer, x, actual[0], actual[1], actual[2], actual[3], rgba[0], rgba[1], rgba[2], rgba[3]);
        }
        CHECK(actual == rgba);
    };
    const auto check_pixel = [&](std::uint32_t slot, std::uint32_t layer, std::uint32_t x,
                                 std::array<unsigned, 4> rgba)
    {
        check_capture_pixel(stream_capture, slot, layer, x, rgba);
    };
    if (empty_stream.slot_index != rendered_stream.slot_index && empty_stream.slot_index != swapped_stream.slot_index)
    {
        check_pixel(empty_stream.slot_index, 0, 16, {0, 0, 0, 0});
        check_pixel(empty_stream.slot_index, 1, 40, {0, 0, 0, 0});
    }
    if (rendered_stream.slot_index != swapped_stream.slot_index)
        check_pixel(rendered_stream.slot_index, 0, 48, {65, 197, 161, 255});
    check_pixel(swapped_stream.slot_index, 0, 16, {241, 0, 0, 255});
    check_pixel(swapped_stream.slot_index, 0, 48, {129, 197, 97, 255});
    check_pixel(swapped_stream.slot_index, 1, 16, {0, 0, 0, 0});
    check_pixel(swapped_stream.slot_index, 1, 36, {0, 0, 0, 0});
    check_pixel(swapped_stream.slot_index, 1, 44, {83, 0, 0, 255});
    check_pixel(swapped_stream.slot_index, 1, 56, {0, 0, 0, 0});
    call<RemoveTarget>(*client, commands, stream_target);
    std::puts("RASTER STREAM PASS: dynamic program ABI, stale revision, replaced slots, resize and native pixels");

    const auto layer_effect = make_storage_effect(runtime_selection, "");
    call<PrepareEffect>(*client, commands, {layer_effect, device});
    const auto layer_target = call<CreateTarget>(*client, commands, {device, peer_desc}).target;
    const auto check_runtime_layers = [&](std::uint32_t count, std::size_t capture)
    {
        const auto layer_view = call<CreateView>(*client, commands, {scene}).view;
        const auto layer_data = make_resource(layer_view, EffectCaptureDescriptor::descriptor.type);
        const auto layer_job = make_storage_job(layer_effect);
        call<StartRenderJob>(*client, commands, {device, layer_job, layer_target, producer_binding(layer_data)});

        runtime_layer_count = 4 - count;
        std::array<std::uint32_t, 4> warmed{};
        for (auto& slot : warmed)
            slot = call<RequestRenderJob>(*client, commands, {layer_job}).slot_index;
        runtime_layer_conflict = true;
        CHECK(call<RequestRenderJob>(*client, commands, {layer_job}, false).error.type == kGraphInvalidRange);
        runtime_layer_conflict = false;
        for (auto bias : {ELayerBias::MISSING, ELayerBias::NONFINITE, ELayerBias::UNDECLARED})
        {
            runtime_layer_bias = bias;
            CHECK(call<RequestRenderJob>(*client, commands, {layer_job}, false).error.type == kGraphInvalidDescriptor);
        }
        runtime_layer_bias = ELayerBias::VALID;
        runtime_layer_count = 0;
        CHECK(call<RequestRenderJob>(*client, commands, {layer_job}, false).error.type == kGraphInvalidDescriptor);
        runtime_layer_count = count;
        const auto selected = call<RequestRenderJob>(*client, commands, {layer_job}).slot_index;
        CHECK(std::ranges::find(warmed, selected) != warmed.end());

        stopAndRemoveJob(*client, commands, layer_job);
        call<RemoveView>(*client, commands, {scene, layer_view});
        const auto& result = EffectCaptureBuffer::pixels[capture];
        const auto deadline = Clock::now() + 10s;
        while (!result.ready.load(std::memory_order_acquire)) CHECK(Clock::now() < deadline);
        CHECK(selected < result.frames.size());
        std::array<std::uint32_t, 7> words{};
        std::memcpy(words.data(), result.frames[selected].data(), sizeof(words));
        CHECK(words[0] == count);
        for (std::uint32_t i = 1; i <= 3; ++i)
            CHECK(words[i] == (i <= count ? i * 40 : 0));
        const auto raised = std::bit_cast<float>(words[4]);
        CHECK(raised > 0.27F && raised < 0.5F);
        if (count == 3)
        {
            CHECK(std::bit_cast<float>(words[5]) == 0.25F);
            const auto lowered = std::bit_cast<float>(words[6]);
            CHECK(lowered > 0 && lowered < 0.23F);
        }
    };
    check_runtime_layers(3, 5);
    check_runtime_layers(1, 6);
    call<RemoveTarget>(*client, commands, {layer_target});
    call<RemoveEffect>(*client, commands, {layer_effect});
    std::puts("RUNTIME LAYERS PASS: grow/shrink, per-layer depth bias, reset to zero and invalid section rejection");

    const auto uncounted_view = call<CreateView>(*client, commands, {scene}).view;
    const auto uncounted_data = make_resource(uncounted_view, EffectCaptureDescriptor::descriptor.type);
    const auto uncounted_target = call<CreateTarget>(*client, commands, {device, peer_desc}).target;
    const auto uncounted_job = call<CreateRenderJob>(*client, commands, {scene, uncounted_view, stream_effect, EJobCadence::REQUESTED}).job;
    const auto uncounted_source = make_resource(uncounted_view, StreamProgramStorage::descriptor.type);
    auto uncounted_binding = producer_binding(uncounted_data);
    call<SetStreamPrograms>(*client, commands, {uncounted_source, ETestStreamPrograms::REVERSED});
    call<StartRenderJob>(*client, commands, {device, uncounted_job, uncounted_target, std::move(uncounted_binding)});
    for (unsigned i = 0; i < config.frames_in_flight; ++i) call<RequestRenderJob>(*client, commands, {uncounted_job});
    const auto warmed_sets = UniformUploadProbe::set_allocations.load();
    const auto warmed_pools = UniformUploadProbe::pool_creations.load();
    stream_mode = ETestStreamMode::SHARED_BINDINGS;
    call<RequestRenderJob>(*client, commands, {uncounted_job});
    CHECK(UniformUploadProbe::set_allocations == warmed_sets && UniformUploadProbe::pool_creations == warmed_pools);
    UniformUploadProbe::reject_raster_pool = true;
    stream_mode = ETestStreamMode::DISTINCT_BINDINGS;
    const auto rejected_pool = call<RequestRenderJob>(*client, commands, {uncounted_job}, false);
    CHECK(rejected_pool.error.type == kDeviceFailure && !UniformUploadProbe::reject_raster_pool.load());
    UniformUploadProbe::reject_raster_set_countdown = 2;
    stream_mode = ETestStreamMode::DISTINCT_BINDINGS;
    const auto rejected_set = call<RequestRenderJob>(*client, commands, {uncounted_job}, false);
    CHECK(rejected_set.error.type == kDeviceFailure && UniformUploadProbe::reject_raster_set_countdown.load() == 0);
    const auto previous_sets = UniformUploadProbe::set_allocations.load();
    for (unsigned i = 0; i < config.frames_in_flight; ++i) call<RequestRenderJob>(*client, commands, {uncounted_job});
    CHECK(UniformUploadProbe::set_allocations.load() == previous_sets);
    std::uint32_t large_slot = 0;
    for (unsigned i = 0; i <= config.frames_in_flight; ++i)
    {
        const auto pools = UniformUploadProbe::pool_creations.load();
        const auto sets = UniformUploadProbe::set_allocations.load();
        stream_mode = ETestStreamMode::DISTINCT_BINDINGS;
        large_slot = call<RequestRenderJob>(*client, commands, {uncounted_job}).slot_index;
        const auto grown = UniformUploadProbe::pool_creations.load() - pools;
        const auto allocated = UniformUploadProbe::set_allocations.load() - sets;
        CHECK(grown == (i < config.frames_in_flight ? 1u : 0u));
        CHECK(i < config.frames_in_flight ? allocated >= 65 : allocated == 0);
        std::printf(
            "RASTER DESCRIPTORS slot=%u draws=67 private_draws=65 pool_creates=%u set_allocations=%u\n",
            large_slot, grown, allocated
        );
    }
    stream_mode = ETestStreamMode::NO_COUNT;
    const auto uncounted = call<RequestRenderJob>(*client, commands, {uncounted_job}).slot_index;
    stopAndRemoveJob(*client, commands, uncounted_job);
    call<RemoveView>(*client, commands, {scene, uncounted_view});
    const auto& uncounted_capture = EffectCaptureBuffer::pixels[7];
    const auto uncounted_deadline = Clock::now() + 10s;
    while (!uncounted_capture.ready.load(std::memory_order_acquire)) CHECK(Clock::now() < uncounted_deadline);
    CHECK(large_slot != uncounted);
    check_capture_pixel(uncounted_capture, large_slot, 0, 48, {65, 197, 161, 255});
    check_capture_pixel(uncounted_capture, uncounted, 0, 16, {65, 197, 161, 255});
    check_capture_pixel(uncounted_capture, uncounted, 0, 48, {65, 197, 161, 255});
    check_capture_pixel(uncounted_capture, uncounted, 1, 36, {83, 0, 0, 255});
    check_capture_pixel(uncounted_capture, uncounted, 1, 44, {83, 0, 0, 255});
    call<RemoveTarget>(*client, commands, {uncounted_target});
    call<RemoveEffect>(*client, commands, {stream_effect});
    std::puts("INDIRECT STREAM PASS: GPU count masks the second indexed and non-indexed commands");

    const TargetDesc tone_desc{64, 32, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN, true};
    const auto tone_target_a = call<CreateTarget>(*client, commands, {device, tone_desc}).target;
    const auto tone_target_b = call<CreateTarget>(*client, commands, {device, tone_desc}).target;
    const auto tone_view_a = call<CreateView>(*client, commands, {scene}).view;
    const auto tone_view_b = call<CreateView>(*client, commands, {scene}).view;
    const auto tone_bytes_a = make_resource(tone_view_a, EffectCaptureDescriptor::descriptor.type);
    const auto tone_bytes_b = make_resource(tone_view_b, EffectCaptureDescriptor::descriptor.type);
    call<PrepareEffect>(*client, commands, {tone_effect, device});
    const auto tone_job_a = make_storage_job(tone_effect);
    const auto tone_job_b = make_storage_job(tone_effect);
    call<StartRenderJob>(*client, commands, {device, tone_job_a, tone_target_a, producer_binding(tone_bytes_a)});
    const auto tone_compiles = call<QueryRenderStats>(*client, commands, {}).compiled_graphs;
    call<StartRenderJob>(*client, commands, {device, tone_job_b, tone_target_b, producer_binding(tone_bytes_b)});
    const auto tone_slot_a = call<RequestRenderJob>(*client, commands, {tone_job_a}).slot_index;
    call<SetToneMappingExposure>(*client, commands, {tone_effect, tone_left_node, 2.F});
    const auto invalid_tone = std::numeric_limits<float>::quiet_NaN();
    CHECK(call<SetToneMappingExposure>(*client, commands, {tone_effect, tone_right_node, invalid_tone}, false)
              .error.type == kFrameContract);
    const auto tone_slot_b = call<RequestRenderJob>(*client, commands, {tone_job_b}).slot_index;
    CHECK(call<QueryRenderStats>(*client, commands, {}).compiled_graphs == tone_compiles);

    stopAndRemoveJob(*client, commands, tone_job_a);
    stopAndRemoveJob(*client, commands, tone_job_b);
    call<RemoveView>(*client, commands, {scene, tone_view_a});
    call<RemoveView>(*client, commands, {scene, tone_view_b});
    const auto read_tone = [&](std::size_t index, std::uint32_t slot, float left_exposure)
    {
        const auto& result = EffectCaptureBuffer::pixels[index];
        const auto deadline = Clock::now() + 10s;
        while (!result.ready.load(std::memory_order_acquire)) CHECK(Clock::now() < deadline);
        CHECK(slot < result.frames.size());
        constexpr std::array source{0.125f, 0.375f, 0.625f};
        for (std::uint32_t row : {16u, 48u})
        {
            const bool srgb = row == 48;
            const auto offset = row * result.pitch + 16 * 4;
            for (std::size_t channel = 0; channel < source.size(); ++channel)
            {
                const auto x = source[channel] * std::exp2(srgb ? 1.f : left_exposure);
                auto mapped = (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f);
                if (srgb) mapped = 1.055f * std::pow(mapped, 1.f / 2.4f) - 0.055f;
                const int read = std::to_integer<unsigned char>(result.frames[slot][offset + channel]);
                CHECK(std::abs(read - static_cast<int>(std::round(mapped * 255))) <= 1);
            }
            CHECK(result.frames[slot][offset + 3] == std::byte{255});
        }
    };
    read_tone(8, tone_slot_a, -2.F);
    read_tone(9, tone_slot_b, 2.F);
    call<RemoveTarget>(*client, commands, {tone_target_a});
    call<RemoveTarget>(*client, commands, {tone_target_b});
    call<RemoveEffect>(*client, commands, {tone_effect});
    std::puts("TONE MAPPING PASS: per-node exposure, linear and sRGB readback, shared variant");

    CreateEffectRequest uniform_request;
    uniform_request.graph = std::make_unique<EffectGraph>();
    EffectGraphEdit uniform_edit{*uniform_request.graph};
    const auto uniform_node = take(uniform_edit.addNode(
        uniform_capability, uniform_selection, async_uniform ? "async" : ""
    ));
    CHECK(uniform_edit.setOutput("readback", uniform_edit.pinId(uniform_node, node_bytes), false));
    CHECK(uniform_edit.commit());
    uniform_request.output = {rdesc::ETextureFormat::RGBA16_SFLOAT, 1, ETargetKind::OFFSCREEN, true};
    const auto uniform_effect = createPreparedEffect(*client, commands, device, std::move(uniform_request));
    call<SetTestUniformBlock>(*client, commands, {uniform_effect, uniform_node, 0x41414141});
    const auto uniform_view_a = call<CreateView>(*client, commands, {scene}).view;
    const auto uniform_view_b = call<CreateView>(*client, commands, {scene}).view;
    const auto uniform_bytes_a = make_resource(uniform_view_a, EffectCaptureDescriptor::descriptor.type);
    const auto uniform_bytes_b = make_resource(uniform_view_b, EffectCaptureDescriptor::descriptor.type);
    const auto uniform_target_a = call<CreateTarget>(*client, commands, {device, peer_desc}).target;
    auto distinct_target = peer_desc;
    distinct_target.width *= 2;
    const auto uniform_target_b = call<CreateTarget>(*client, commands, {device, distinct_target}).target;
    const auto uniform_job_a = make_storage_job(uniform_effect);
    const auto uniform_job_b = make_storage_job(uniform_effect);
    call<StartRenderJob>(*client, commands, {device, uniform_job_a, uniform_target_a, producer_binding(uniform_bytes_a)});
    call<StartRenderJob>(*client, commands, {device, uniform_job_b, uniform_target_b, producer_binding(uniform_bytes_b)});
    const auto count_submits = [&] { return call<QueryRenderStats>(*client, commands, {}).submissions; };
    const auto before_compute = UniformUploadProbe::compute_submissions.load();
    const auto before_copies = UniformUploadProbe::copies.load();
    const auto before_backings = UniformUploadProbe::backings.load();
    const auto before_uniform = count_submits();
    const auto uniform_first = call<RequestRenderJob>(*client, commands, {uniform_job_a});
    CHECK(UniformUploadProbe::copies == before_copies + 1);
    if (async_uniform && UniformUploadProbe::hasSeparateComputeQueue())
        CHECK(UniformUploadProbe::compute_submissions > before_compute);
    const auto initial_submissions = count_submits() - before_uniform;
    const auto uniform_shared = call<RequestRenderJob>(*client, commands, {uniform_job_b});
    const auto shared_submissions = count_submits() - before_uniform - initial_submissions;
    CHECK(initial_submissions == shared_submissions + 1);
    CHECK(UniformUploadProbe::copies == before_copies + 1);
    call<SetTestUniformBlock>(*client, commands, {uniform_effect, uniform_node, 0x62626262});
    const auto before_changed = count_submits();
    const auto uniform_changed = call<RequestRenderJob>(*client, commands, {uniform_job_a});
    CHECK(count_submits() - before_changed == initial_submissions);
    CHECK(UniformUploadProbe::copies == before_copies + 2);
    // Select both graph variants in one Server round after the next setter.
    call<SetTestUniformBlock>(*client, commands, {uniform_effect, uniform_node, 0x73737373});
    const auto [paired_before, paired] = requestRound(std::array{uniform_job_a, uniform_job_b});
    if (!async_uniform) CHECK(count_submits() - paired_before == initial_submissions);
    CHECK(UniformUploadProbe::copies == before_copies + 3);

    stopAndRemoveJob(*client, commands, uniform_job_b);
    call<RemoveView>(*client, commands, {scene, uniform_view_b});
    verify_prepared(11, paired.front().slot_index, std::byte{0x73});
    if (uniform_shared.slot_index != paired.front().slot_index)
        verify_prepared(11, uniform_shared.slot_index, std::byte{0x41});

    // Cross the pool's capacity and check that retired buffers receive subsequent revisions.
    call<SetTestUniformBlock>(*client, commands, {uniform_effect, uniform_node, 0x84848484});
    const auto uniform_recycled = call<RequestRenderJob>(*client, commands, {uniform_job_a});
    CHECK(UniformUploadProbe::copies == before_copies + 4);
    CHECK(UniformUploadProbe::backings == before_backings + config.frames_in_flight + 1);
    stopAndRemoveJob(*client, commands, uniform_job_a);
    call<RemoveView>(*client, commands, {scene, uniform_view_a});
    const auto paired_slot = paired.front().slot_index;
    const auto recycled_slot = uniform_recycled.slot_index;
    if (uniform_first.slot_index != uniform_changed.slot_index &&
        uniform_first.slot_index != paired_slot && uniform_first.slot_index != recycled_slot)
        verify_prepared(10, uniform_first.slot_index, std::byte{0x41});
    if (uniform_changed.slot_index != paired_slot && uniform_changed.slot_index != recycled_slot)
        verify_prepared(10, uniform_changed.slot_index, std::byte{0x62});
    CHECK(recycled_slot != paired_slot);
    verify_prepared(10, paired_slot, std::byte{0x73});
    verify_prepared(10, recycled_slot, std::byte{0x84});
    if (async_uniform && UniformUploadProbe::hasSeparateComputeQueue())
    {
        using Fault = UniformUploadProbe::EFault;
        const auto start_uniform = [&](ViewId owner)
        {
            const auto data = make_resource(owner, EffectCaptureDescriptor::descriptor.type);
            const auto job = make_storage_job(uniform_effect);
            call<StartRenderJob>(*client, commands, {device, job, uniform_target_a, producer_binding(data)});
            return job;
        };
        const auto fault_view = call<CreateView>(*client, commands, {scene}).view;
        const auto fault_job = start_uniform(fault_view);
        call<SetTestUniformBlock>(*client, commands, {uniform_effect, uniform_node, 0x95959595});
        const auto accepted_before = UniformUploadProbe::accepted_uploads.load();
        const auto submissions_before = count_submits();
        UniformUploadProbe::fault = Fault::REJECT_UPLOAD;
        const auto rejected = call<RequestRenderJob>(*client, commands, {fault_job}, false);
        CHECK(rejected.error.type == kDeviceFailure);
        CHECK(rejected.error.args[0] == static_cast<std::uint64_t>(nri::Result::INVALID_ARGUMENT));
        CHECK(UniformUploadProbe::accepted_uploads == accepted_before && count_submits() == submissions_before);

        auto held_client = ipc::IpcCore::instance().createRequester(config.channel_name);
        CHECK(held_client);
        UniformUploadProbe::fault = Fault::HOLD_UPLOAD;
        auto held = std::async(std::launch::async, [&]
        {
            return call<RequestRenderJob>(*held_client, commands, {fault_job});
        });
        const auto held_deadline = Clock::now() + 10s;
        while (!UniformUploadProbe::upload_held.load())
        {
            CHECK(Clock::now() < held_deadline);
            std::this_thread::yield();
        }
        call<SetTestUniformBlock>(*client, commands, {uniform_effect, uniform_node, 0xa6a6a6a6});
        UniformUploadProbe::release_upload.store(true);
        UniformUploadProbe::release_upload.notify_all();
        const auto frozen = held.get();
        UniformUploadProbe::fault = Fault::NONE;
        CHECK(UniformUploadProbe::checked_upload_wait);
        CHECK(UniformUploadProbe::accepted_uploads == accepted_before + 1);
        const auto next = call<RequestRenderJob>(*client, commands, {fault_job});
        CHECK(UniformUploadProbe::accepted_uploads == accepted_before + 2);
        CHECK(frozen.slot_index != next.slot_index);

        const auto prefix_before = count_submits();
        const auto compute_before = UniformUploadProbe::compute_submissions.load();
        UniformUploadProbe::fault = Fault::REJECT_SUFFIX;
        const auto partial = call<RequestRenderJob>(*client, commands, {fault_job}, false);
        UniformUploadProbe::fault = Fault::NONE;
        const auto accepted_prefix = count_submits() - prefix_before;
        CHECK(partial.error.type == kDeviceFailure && UniformUploadProbe::suffix_rejected.load());
        CHECK(accepted_prefix > 0 && UniformUploadProbe::compute_submissions > compute_before);
        CHECK(UniformUploadProbe::accepted_uploads == accepted_before + 2);
        stopAndRemoveJob(*client, commands, fault_job);
        call<RemoveView>(*client, commands, {scene, fault_view});
        verify_prepared(12, frozen.slot_index, std::byte{0x95});
        verify_prepared(12, next.slot_index, std::byte{0xa6});
        std::printf("UNIFORM FAULT PASS: rejected upload retried, frozen value preserved, accepted prefix=%llu\n",
            static_cast<unsigned long long>(accepted_prefix));

        stopAndRemoveJob(*client, commands, first);
        stopAndRemoveJob(*client, commands, peer);
        const auto unknown_view = call<CreateView>(*client, commands, {scene}).view;
        const auto unknown_job = start_uniform(unknown_view);
        call<SetTestUniformBlock>(*client, commands, {uniform_effect, uniform_node, 0xb7b7b7b7});
        const auto before_unknown = count_submits();
        const auto device_state = call<QueryRenderDevice>(*client, commands, device);
        UniformUploadProbe::fault = Fault::UNKNOWN_UPLOAD;
        const auto unknown = call<RequestRenderJob>(*client, commands, {unknown_job}, false);
        CHECK(unknown.error.type == kOutcomeUnknown && UniformUploadProbe::unknown_retired.load());
        CHECK(unknown.job == unknown_job && unknown.frame_index == 0 && unknown.slot_index == 0);
        CHECK(unknown.bufferVersion({0, 4}).writer.isNull());
        render.join();
        CHECK(!server_result && server_result.error() == unknown.error);
        CHECK(server->stats().submissions == before_unknown);
        CHECK(server->stats().validation_errors == 0);
        std::printf("UNIFORM UNKNOWN PASS: native queue drained before terminal error=%llu, queues=%u\n",
            static_cast<unsigned long long>(unknown.error.type), device_state.physical_queues);
        return;
    }
    call<RemoveTarget>(*client, commands, uniform_target_a);
    call<RemoveTarget>(*client, commands, uniform_target_b);
    call<RemoveEffect>(*client, commands, {uniform_effect});
    std::puts("UNIFORM BLOCK PASS: one native copy per revision, same-round reuse and bounded backing");

    call<SetNodeTestValue>(*client, commands, {effect, forward, 13, 17});

    CreateEffectRequest shutdown_request{std::make_unique<EffectGraph>()};
    EffectGraphEdit shutdown_edit{*shutdown_request.graph};
    const auto shutdown_node = take(shutdown_edit.addNode(capability, selected, "producer"));
    CHECK(shutdown_edit.setOutput("display", shutdown_edit.pinId(shutdown_node, node_color), true));
    CHECK(shutdown_edit.commit());
    const auto shutdown_effect = call<CreateEffect>(*client, commands, std::move(shutdown_request)).effect;
    auto shutdown = begin_prepare(shutdown_effect, device);
    WindowUpdates cancelled_updates;
    const auto setter = std::ranges::find(commands.commands, SetNodeTestValue::name, &CommandInfo::name);
    CHECK(setter != commands.commands.end());
    cancelled_updates.append<SetNodeTestValue>(setter->id, {effect, forward, 17, 18});
    const auto [before_stop, cancelled] = requestRound(
        std::array{first, peer}, ERoundRequest::WINDOW_THEN_STOP, std::move(cancelled_updates), kCommandCancelled
    );
    CHECK(!cancelled.front().updates.empty() && cancelled.back().updates.empty());
    CHECK(server->stats().submissions == before_stop);
    release_prepare(ENodeBuildGate::PASS);
    CHECK(shutdown.get().error.type == kCommandCancelled);
    render.join();
    CHECK(server_result);
    CHECK(server->stats().validation_errors == 0);
    std::printf("EFFECT GRAPH PASS backend=%s explicit prepare, concurrent rejection, retry, shutdown and pixel readback\n",
        d3d12 ? "D3D12" : "Vulkan");
}

static void preparedBuffers(bool d3d12)
{
    enum class EPhase { SMALL, GROWN, REPLACEMENT, PREFIX_EXTENT, FAIL_AFTER_WRITE, OMIT, OVERSIZED, SHRUNK, UNVERSIONED };
    static std::atomic phase{EPhase::SMALL};
    static std::atomic_uint graph_builds{};
    class Producer final
    {
    public:
        using Configuration = std::uint64_t;
        using LinkedData = Configuration;

        static RenderResult<void>
        onFrame(const LinkedData& scratch_bytes, const EffectFrameInputs<ShadowLightSnapshot>& inputs, EffectFrameWriter& frame) noexcept
        {
            const auto selected = phase.load();
            const std::array extents{scratch_bytes};
            const auto incomplete = frame.publishBuffers({}, extents);
            CHECK(!incomplete && incomplete.error().type == kGraphUnknownResource);
            const auto publish = [&](std::span<const std::byte> bytes, std::uint64_t revision = 0) noexcept
            {
                const std::array uploads{FrameBufferUpload{bytes, revision}};
                auto result = frame.publishBuffers(uploads, extents);
                if (result)
                {
                    const auto duplicate = frame.publishBuffers(uploads, extents);
                    CHECK(!duplicate && duplicate.error().type == kFrameContract);
                }
                return result;
            };
            if (selected == EPhase::OMIT) return {};
            const auto& lights = std::get<0>(inputs.values);
            const auto& snapshot = lights.value.get();
            const bool invalid_counts = snapshot.counts != std::array{1u, 0u, 0u} || snapshot.casters.size() != 1;
            if (invalid_counts) return lux::cxx::unexpected(RenderError{kFrameContract});
            const auto& light = snapshot.casters.front();
            if (light.dense_index != 0 || !std::holds_alternative<DirectionalLightGpu>(light.value))
                return lux::cxx::unexpected(RenderError{kFrameContract});
            const auto marker = static_cast<std::uint32_t>(light.settings.depth_bias_constant) * 0x01010101u;
            const std::array initial{4u, marker, 0u, marker};
            std::array<std::uint32_t, 256> grown{256};
            grown[1] = selected == EPhase::REPLACEMENT ? 0xa6a6a6a6u : marker;
            grown.back() = marker;
            std::array<std::uint32_t, 257> oversized{};
            if (selected == EPhase::OVERSIZED)
                return publish(std::as_bytes(std::span{oversized}));
            if (selected == EPhase::SMALL)
                return publish(std::as_bytes(std::span{initial}), lights.revision);
            const bool replacement = selected == EPhase::REPLACEMENT || selected == EPhase::PREFIX_EXTENT;
            const auto revision = lights.revision * 2 + replacement;
            if (selected == EPhase::PREFIX_EXTENT)
            {
                std::array<std::uint32_t, 128> half{};
                half.front() = 128;
                half[1] = half.back() = 0x71717171u;
                return publish(std::as_bytes(std::span{half}), revision);
            }
            if (selected == EPhase::SHRUNK)
                return publish(std::as_bytes(std::span{initial}), revision);
            const auto selected_revision = selected == EPhase::UNVERSIONED ? 0 : revision;
            auto written = publish(std::as_bytes(std::span{grown}), selected_revision);
            if (selected == EPhase::FAIL_AFTER_WRITE && written)
                return lux::cxx::unexpected(RenderError{kFrameContract});
            return written;
        }
    };

    RenderServerConfig config;
    config.channel_name = "lux.render.test.prepared-buffer";
    config.frames_in_flight = 2;
    config.validation = nativeValidationEnabled();
    config.uploads.emplace();
    config.uploads->chunk_bytes = 512;
    config.uploads->staging_pages = 1;
    config.uploads->window_regions = 1;
    DeviceOptions options;
    if (d3d12) options.backend = EGraphicsBackend::D3D12;
    auto server = take(RenderServer::create(config, lux::execution::ExecutionRuntime::instance()));
    take(server->registerResource<EffectCaptureBuffer>());

    EffectCapability definition;
    constexpr std::string_view node_name = "test.prepared_buffers";
    definition.id = graph::nodeTypeId(node_name);
    definition.canonical_name = node_name;
    const EffectPortDefinition definition_ports[]{{node_bytes, "bytes", {"test.readback_bytes", 1}, graph::EPinDirection::OUTPUT}};
    definition.ports = definition_ports;
    CHECK(server->registerEffectCapability(definition));
    ImplementationEntry implementation;
    implementation.capability = definition.id;
    implementation.identity = {"test.prepared_buffers.compute", 1};
    implementation.supported_outputs = {node_bytes};
    static constexpr auto build_implementation = [](EffectCompileContext& context, std::string_view mode) noexcept -> RenderResult<void>
    {
        ++graph_builds;
        auto selected = context.declarePreparedBuffer("selection", 16, 1024);
        if (!selected) return lux::cxx::unexpected(selected.error());
        const auto scratch = context.declareFrameBuffer("scratch", 16, 16);
        if (!scratch) return lux::cxx::unexpected(scratch.error());
        auto lights = mode == "wrong-type" ? context.readResourceView<std::uint32_t>(kSceneLights, "lights") :
            context.readResourceView<ShadowLightSnapshot>(kSceneLights, "lights");
        if (!lights) return lux::cxx::unexpected(lights.error());
        const auto add_producer = [&]() noexcept -> RenderResult<void>
        {
            FrameAlgorithmUses uses{
                .uploads = {selected->resource}, .frame_sizes = {scratch->resource}, .resource_views = {*lights}
            };
            if (mode == "wrong-upload") std::swap(uses.uploads, uses.frame_sizes);
            if (mode == "wrong-size") uses.frame_sizes = {selected->resource};
            if (mode == "repeated-size") uses.frame_sizes.push_back(scratch->resource);
            return context.onFrame<&Producer::onFrame>(16, std::move(uses));
        };
        if (mode != "missing") CHECK(add_producer());
        if (mode == "duplicate") CHECK(add_producer());
        const ResourceNameView owner{EffectCaptureDescriptor::descriptor.type.name()};
        const auto readback = take(context.importFinalBuffer("readback", owner, {
            "", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION
        }));
        const auto output = take(context.declareResource({"generated", GraphBufferDesc{effect_capture_bytes}}));
        ComputeWork compute;
        compute.interface.program = take(context.addProgram(GraphProgram{kEffectPreparedBuffer}));
        compute.interface.resources = {
            {"prepared", {*selected}},
            {"generated", {{output, 0}, {}, EGraphAccess::WRITE}}
        };
        compute.dispatch = DispatchArguments{{effect_capture_bytes / (sizeof(std::uint32_t) * 64), 1, 1}};
        const auto dispatched = take(context.addComputePass("read producer buffer", std::move(compute)));
        const auto result = take(context.addTransferPass(
            "capture", TransferWork{{CopyBuffer{{dispatched.written.front()}, {readback}}}}
        ));
        return context.publish(node_bytes, {{node_bytes, result.written.front()}});
    };
    implementation.create = fixture::Effect<build_implementation>::create;
    const auto selection = implementation.identity;
    CHECK(server->registerEffectImplementation(std::move(implementation)));

    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread worker([&] { CHECK(server->run()); });
    const auto commands = queryCommands(*client);
    const auto make_effect = [&](std::string mode)
    {
        CreateEffectRequest request;
        request.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*request.graph};
        const auto node = take(edit.addNode(definition, selection, std::move(mode)));
        CHECK(edit.setOutput("readback", edit.pinId(node, node_bytes)));
        CHECK(edit.commit());
        return call<CreateEffect>(*client, commands, std::move(request)).effect;
    };
    const auto device = call<CreateDeviceSession>(*client, commands, options).device;
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto view = call<CreateView>(*client, commands, {scene}).view;
    const TargetDesc output{64, 64, rdesc::ETextureFormat::RGBA8_UNORM};
    const auto target = call<CreateTarget>(*client, commands, {device, output}).target;
    JobBinding binding;
    const auto capture = call<GetOrCreateResource>(*client, commands, {
        device, ViewScope{scene, view}, EffectCaptureDescriptor::descriptor.type
    }).resource_id;
    binding.resources.push_back({"readback", capture});
    const auto light_owner = call<GetOrCreateResource>(*client, commands, {device, scene, kLightResourceName}).resource_id;
    const std::array<VDirectLight, 1> values{DirectionalLightGpu{}};
    std::array settings{LightShadowSettings{.enabled = true, .depth_bias_constant = 54}};
    const auto light = call<LightingCommands::CreateLights>(*client, commands, {light_owner, {1, values, settings}})
        .lights.front();
    binding.resources.push_back({"lights", light_owner, 0});
    const auto start = [&](EffectId effect)
    {
        return call<CreateRenderJob>(*client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
    };
    for (auto [mode, error] : {std::pair{"missing", kGraphUndefinedRead},
                               std::pair{"duplicate", kGraphAmbiguousProducer},
                               std::pair{"wrong-upload", kGraphInvalidDescriptor},
                               std::pair{"wrong-size", kGraphInvalidDescriptor},
                               std::pair{"repeated-size", kGraphAmbiguousProducer}})
    {
        const auto invalid = make_effect(mode);
        CHECK(call<PrepareEffect>(*client, commands, {invalid, device}, false).error.type == error);
        call<RemoveEffect>(*client, commands, {invalid});
    }
    const auto wrong_effect = make_effect("wrong-type");
    CHECK(call<PrepareEffect>(*client, commands, {wrong_effect, device}, false).error.type == kStorageMismatch);
    call<RemoveEffect>(*client, commands, {wrong_effect});
    const auto effect = make_effect("");
    call<PrepareEffect>(*client, commands, {effect, device});
    const auto job = start(effect);
    call<StartRenderJob>(*client, commands, {device, job, target, binding});
    const auto published_builds = graph_builds.load();
    const auto submissions = [&] { return call<QueryRenderStats>(*client, commands, {}).submissions; };
    const auto before = submissions();
    call<RequestRenderJob>(*client, commands, {job});
    const auto other_initial = call<RequestRenderJob>(*client, commands, {job});
    const auto initial_submits = submissions() - before;
    if (initial_submits != 4) std::fprintf(stderr, "Prepared buffers used %llu submissions to initialize two FIF slots\n",
        static_cast<unsigned long long>(initial_submits));
    CHECK(initial_submits == 4);
    const auto initial = call<RequestRenderJob>(*client, commands, {job});
    CHECK(submissions() - before == 5);
    CHECK(initial.slot_index != other_initial.slot_index);

    for (auto [failure, error] : {
            std::pair{EPhase::FAIL_AFTER_WRITE, kFrameContract},
            std::pair{EPhase::OMIT, kGraphUndefinedRead},
            std::pair{EPhase::OVERSIZED, kFrameContract}
        })
    {
        phase.store(failure);
        CHECK(call<RequestRenderJob>(*client, commands, {job}, false).error.type == error);
        CHECK(submissions() - before == 5);
    }
    settings.front().depth_bias_constant = 133;
    const std::array updates{DirectLightUpdate{light, values.front(), settings.front()}};
    call<LightingCommands::Update>(*client, commands, {light_owner, updates});
    CHECK(submissions() - before == 5);
    phase = EPhase::GROWN;
    const auto grown = call<RequestRenderJob>(*client, commands, {job});
    CHECK(submissions() - before == 8);

    const auto recovery_scene = call<CreateScene>(*client, commands, {}).scene;
    const auto recovery_view = call<CreateView>(*client, commands, {recovery_scene}).view;
    const auto recovery_target = call<CreateTarget>(*client, commands, {device, output}).target;
    const auto recovery_capture = call<GetOrCreateResource>(*client, commands, {
        device, ViewScope{recovery_scene, recovery_view}, EffectCaptureDescriptor::descriptor.type
    }).resource_id;
    const auto recovery_lights =
        call<GetOrCreateResource>(*client, commands, {device, recovery_scene, kLightResourceName}).resource_id;
    settings.front().depth_bias_constant = 97;
    call<LightingCommands::CreateLights>(*client, commands, {recovery_lights, {1, values, settings}});
    binding.resources.front().resource = recovery_capture;
    binding.resources.back().resource = recovery_lights;
    const auto recovery_job =
        call<CreateRenderJob>(*client, commands, {recovery_scene, recovery_view, effect, EJobCadence::REQUESTED}).job;
    call<StartRenderJob>(*client, commands, {device, recovery_job, recovery_target, binding});
    CHECK(graph_builds == published_builds);
    call<RequestRenderJob>(*client, commands, {recovery_job});
    call<RequestRenderJob>(*client, commands, {recovery_job});

    // Both replicas contain the same revision. Overwrite only the beginning of one of them.
    phase = EPhase::REPLACEMENT;
    const auto before_reject = submissions();
    UniformUploadProbe::reject_submission_countdown = 2;
    const auto rejected = call<RequestRenderJob>(*client, commands, {recovery_job}, false);
    CHECK(rejected.error.type == kDeviceFailure && UniformUploadProbe::reject_submission_countdown == 0);
    CHECK(rejected.error.args[0] == static_cast<std::uint64_t>(nri::Result::INVALID_ARGUMENT));
    CHECK(submissions() - before_reject == 1);
    phase = EPhase::GROWN;
    call<RequestRenderJob>(*client, commands, {recovery_job});
    call<RequestRenderJob>(*client, commands, {recovery_job});
    CHECK(submissions() - before_reject == 5);
    phase = EPhase::SHRUNK;
    const auto before_shrink = submissions();
    for (unsigned i = 0; i < 3; ++i) call<RequestRenderJob>(*client, commands, {recovery_job});
    CHECK(submissions() - before_shrink == 5);
    phase = EPhase::UNVERSIONED;
    const auto before_unversioned = submissions();
    for (unsigned i = 0; i < 3; ++i) call<RequestRenderJob>(*client, commands, {recovery_job});
    CHECK(submissions() - before_unversioned == 9);
    stopAndRemoveJob(*client, commands, recovery_job);
    call<RemoveView>(*client, commands, {recovery_scene, recovery_view});

    // A 512-byte accepted prefix of a rejected 1024-byte selection cannot satisfy a new 512-byte
    // payload, even when the source revision matches. Verify both FIF slots actually upload it.
    const auto prefix_view = call<CreateView>(*client, commands, {recovery_scene}).view;
    binding.resources.front().resource = call<GetOrCreateResource>(*client, commands, {
        device, ViewScope{recovery_scene, prefix_view}, EffectCaptureDescriptor::descriptor.type
    }).resource_id;
    const auto prefix_job = call<CreateRenderJob>(*client, commands, {
        recovery_scene, prefix_view, effect, EJobCadence::REQUESTED
    }).job;
    call<StartRenderJob>(*client, commands, {device, prefix_job, recovery_target, binding});
    phase = EPhase::REPLACEMENT;
    const auto before_prefix = submissions();
    UniformUploadProbe::reject_submission_countdown = 2;
    const auto partial = call<RequestRenderJob>(*client, commands, {prefix_job}, false);
    CHECK(partial.error.type == kDeviceFailure && submissions() - before_prefix == 1);
    phase = EPhase::PREFIX_EXTENT;
    const auto left = call<RequestRenderJob>(*client, commands, {prefix_job});
    const auto right = call<RequestRenderJob>(*client, commands, {prefix_job});
    CHECK(left.slot_index != right.slot_index && submissions() - before_prefix == 5);
    stopAndRemoveJob(*client, commands, prefix_job);
    call<RemoveView>(*client, commands, {recovery_scene, prefix_view});
    stopAndRemoveJob(*client, commands, job);
    call<RemoveView>(*client, commands, {scene, view});

    server->stop();
    worker.join();
    CHECK(server->stats().validation_errors == 0);
    const auto& pixels = EffectCaptureBuffer::pixels.front();
    CHECK(pixels.ready.load() == 1 && pixels.frames.size() == 2);
    const auto unchanged_slot = grown.slot_index == initial.slot_index ? other_initial.slot_index : initial.slot_index;
    for (auto [slot, value] : {std::pair{unchanged_slot, 0x36}, std::pair{grown.slot_index, 0x85}})
        CHECK(std::ranges::all_of(pixels.frames[slot], [value](std::byte data) noexcept
        {
            return data == static_cast<std::byte>(value);
        }));
    const auto& repaired = EffectCaptureBuffer::pixels[1];
    CHECK(repaired.ready.load() == 1 && repaired.frames.size() == 2);
    for (const auto& frame : repaired.frames)
        CHECK(std::ranges::all_of(frame, [](std::byte data) noexcept { return data == std::byte{0x61}; }));
    const auto& prefix_capture = EffectCaptureBuffer::pixels[2];
    CHECK(prefix_capture.ready.load() == 1 && prefix_capture.frames.size() == 2);
    for (const auto& frame : prefix_capture.frames)
        CHECK(std::ranges::all_of(frame, [](std::byte byte) { return byte == std::byte{0x71}; }));
    std::puts("PREPARED PREFIX PASS: 512/1024 accepted, same revision at 512 bytes uploaded to both FIFs, 0x71 readback");
    std::printf("PREPARED BUFFER PASS backend=%s typed Storage snapshots, revised owners and rejected suffix retry\n",
        d3d12 ? "D3D12" : "Vulkan");
}

static int sharedRasterTextures(bool d3d12, bool use_materials = false, bool upload_fault = false)
{
    static std::array<TextureId, 2> textures;
    static std::array<MaterialId, 2> materials;
    static MaterialId stale_second_texture;
    static std::atomic_uint selected_texture{};
    class Producer final
    {
    public:
        struct Configuration final { bool materials, declares_texture; std::uint32_t first_program; };
        struct LinkedData final
        {
            DrawStreamSlot stream;
            std::array<RasterProgramSlot, 3> programs;
            std::optional<RasterResourceSlot> materials;
            std::optional<RasterResourceSlot> textures;
        };

        static RenderResult<LinkedData>
        resolve(const Configuration& input, std::span<const RasterOperands, 1> streams) noexcept
        {
            LinkedData linked{streams.front().stream(), {}, {}, {}};
            const auto count = input.materials ? linked.programs.size() : 1;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                auto slot = streams.front().programAt(input.first_program + i);
                if (!slot) return lux::cxx::unexpected(slot.error());
                linked.programs[i] = *slot;
            }
            if (input.declares_texture)
            {
                const auto slot = streams.front().resource("textures", TextureResource::descriptor.type);
                if (!slot) return lux::cxx::unexpected(slot.error());
                linked.textures = *slot;
            }
            if (input.materials)
            {
                const auto records = streams.front().resource("materials", MaterialResource::descriptor.type);
                if (!records) return lux::cxx::unexpected(records.error());
                linked.materials = *records;
            }
            return linked;
        }

        struct Workspace final
        {
            std::optional<MaterialDrawBinding> material;
            std::optional<TextureDrawBinding> texture;
        };

        static RenderResult<std::unique_ptr<Workspace>>
        install(const LinkedData& linked, const TargetDesc&, const InstalledDrawSources& sources) noexcept
        {
            auto result = std::make_unique<Workspace>();
            if (linked.materials)
            {
                auto binding = MaterialDrawBinding::bind(sources, *linked.materials, linked.textures);
                if (!binding) return lux::cxx::unexpected(binding.error());
                result->material.emplace(*binding);
            }
            else if (linked.textures)
            {
                auto binding = TextureDrawBinding::bind(sources, *linked.textures);
                if (!binding) return lux::cxx::unexpected(binding.error());
                result->texture.emplace(*binding);
            }
            return result;
        }

        static RenderResult<void>
        onFrame(const LinkedData& linked, const EffectFrameInputs<>&, EffectFrameWriter& frame, Workspace& workspace) noexcept
        {
            const auto index = selected_texture.load();
            const auto program = linked.programs[linked.materials ? index : 0];
            if (linked.materials)
            {
                auto value = workspace.material->record(program, materials[index]);
                if (!value) return lux::cxx::unexpected(value.error());
                auto repeated = workspace.material->record(program, materials[index]);
                if (!repeated) return lux::cxx::unexpected(repeated.error());
                std::array references{*value, *value};
                const RasterStreamDraw textured{program, {}, {}, {0, sizeof(*value)}, DrawVertices{12}};
                std::array draws{textured, textured};
                std::uint32_t count = 1;
                if (index == 0)
                {
                    const auto unused = linked.programs[1];
                    // This other material has a distinct record and texture. No draw uses its Program.
                    const auto ignored = workspace.material->record(unused, materials[1]);
                    CHECK(ignored);
                    auto stale = program;
                    ++stale.revision;
                    const auto rejected = workspace.material->record(stale, materials[index]);
                    CHECK(!rejected && rejected.error().type == kFrameContract);
                }
                else
                {
                    const auto rejected = workspace.material->record(program, stale_second_texture);
                    CHECK(!rejected && rejected.error().type == kStorageIdentity);
                    const auto plain = linked.programs[2];
                    // Its texture has been released. This Program reads only the tint in the same record.
                    const auto overlay = workspace.material->record(plain, materials[0]);
                    CHECK(overlay);
                    references[1] = *overlay;
                    draws[1].program = plain;
                    draws[1].constants.first = sizeof(*overlay);
                    count = 2;
                }
                return frame.setRasterStream(linked.stream, {
                    std::span{draws}.first(count), {}, {}, std::as_bytes(std::span{references}.first(count))
                });
            }
            CHECK(workspace.texture);
            const auto id = textures[index];
            auto texture = workspace.texture->sample(id);
            if (!texture) return lux::cxx::unexpected(texture.error());
            auto repeated = workspace.texture->sample(id);
            if (!repeated) return lux::cxx::unexpected(repeated.error());
            const TestSharedTextureIndices indices{texture->image_index, texture->sampler_index};
            const RasterStreamDraw draw{program, {}, {}, {0, sizeof(indices)}, DrawVertices{12}};
            return frame.setRasterStream(linked.stream, {
                std::span{&draw, 1}, {}, {}, std::as_bytes(std::span{&indices, 1})
            });
        }
    };

    RenderServerConfig config;
    config.channel_name = use_materials ? "lux.render.test.material-stream" : "lux.render.test.shared-texture";
    config.frames_in_flight = 2;
    config.validation = nativeValidationEnabled();
    config.uploads.emplace();
    if (upload_fault)
    {
        config.uploads->chunk_bytes = 512;
        config.uploads->pending_bytes = 512;
        config.uploads->staging_pages = 1;
#ifdef _WIN32
        UploadAcquireProbe::enabled = true;
#endif
    }
    auto server = take(RenderServer::create(config, lux::execution::ExecutionRuntime::instance()));
    take(server->registerResource<EffectCaptureBuffer>());
    EffectCapability definition;
    constexpr std::string_view name = "test.shared_raster_texture";
    definition.id = graph::nodeTypeId(name);
    definition.canonical_name = name;
    const EffectPortDefinition definition_ports[]{{node_bytes, "bytes", {"test.readback_bytes", 1}, graph::EPinDirection::OUTPUT}};
    definition.ports = definition_ports;
    CHECK(server->registerEffectCapability(definition));
    ImplementationEntry implementation;
    implementation.capability = definition.id;
    implementation.identity = {"test.shared_raster_texture.draw", 1};
    implementation.supported_outputs = {node_bytes};
    static constexpr auto build_implementation = [](EffectCompileContext& context, std::string_view mode) noexcept -> RenderResult<void>
    {
        const auto readback = take(context.importFinalBuffer(
            "readback", EffectCaptureDescriptor::descriptor.type,
            {"", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION}
        ));
        RasterPassTemplate stage;
        stage.purpose = "shared-texture";
        if (mode != "undeclared")
        {
            stage.draw_resources.push_back({
                "textures", ResourceName{TextureResource::descriptor.type.name()},
                EDrawResourceRead::SAMPLED_IMAGES
            });
        }
        stage.inputs = {
            {"uTex", rdesc::EDescriptorType::SAMPLED_IMAGE, EGraphUse::SAMPLED, 2},
            {"uTexSamplers", rdesc::EDescriptorType::SAMPLER, EGraphUse::SAMPLED, 2}
        };
        const bool misplaced_table = mode == "misplaced-table";
        const bool cold_failure = mode == "failed-layout" || mode == "failed-link";
        const bool material = mode == "material" || misplaced_table || cold_failure;
        if (material)
        {
            constexpr DrawResourceReads records{EDrawResourceRead::STORAGE_RECORDS};
            if (misplaced_table) stage.draw_resources.front().reads |= records;
            stage.draw_resources.push_back({"materials", ResourceName{MaterialResource::descriptor.type.name()}, misplaced_table ? DrawResourceReads{} : records});
            for (const auto name : {rdesc::kMaterialTableName, std::string_view{"surfacePages"}})
            {
                stage.inputs.push_back({
                    .name = std::string{name}, .kind = rdesc::EDescriptorType::STORAGE_BUFFER,
                    .usage = EGraphUse::STORAGE_READ, .stages = 2, .shared_records = true
                });
            }
        }
        std::vector<RasterPassTemplate::StreamProgram> programs;
        const auto add = [&](rdesc::ShaderProgramArtifact shader, ParameterBinding parameters)
        {
            const auto id = take(context.addProgram(GraphProgram{std::move(shader)}));
            programs.push_back({id, {}, std::move(parameters)});
        };
        ParameterBinding parameters;
        if (material)
        {
            parameters.setInlineConstant(rdesc::MaterialGpuReference{});
            add(streamShader(context.shaderTarget(), kEffectStreamMaterial), parameters);
            add(streamShader(context.shaderTarget(), kEffectRecordTable), parameters);
            add(streamShader(context.shaderTarget(), kEffectRecordOnly), std::move(parameters));
            if (cold_failure) programs[0].graphics.blend.enabled = true;
            if (mode == "failed-layout") programs[1].geometry_locations = {0};
        }
        else
        {
            parameters.setInlineConstant(TestSharedTextureIndices{});
            add(streamShader(context.shaderTarget(), kEffectStreamSharedTexture), std::move(parameters));
        }
        stage.programs = std::move(programs);
        const auto color = take(context.declareResource({"shared texture output", GraphImageDesc{64, 64}}));
        RasterWork raster;
        raster.program = std::move(stage);
        raster.colors.push_back({{{color, 0}}});
        const auto drawn = take(context.addRasterPass("sample a shared texture", std::move(raster)));
        auto producer = Producer::Configuration{
            material, mode != "undeclared", mode == "failed-link" ? 3u : 0u
        };
        CHECK((context.onFrame<&Producer::onFrame, &Producer::resolve, &Producer::install>(std::move(producer), {
            .draw_streams = {drawn.pass}
        })));
        const auto captured = take(context.addTransferPass("copy texture pixels", TransferWork{{CopyImageBuffer{
            {drawn.written.front()}, {readback}, 256, effect_capture_bytes, ImageCopyRegion{0, 0, 64, 64}
        }}}));
        return context.publish(node_bytes, {{node_bytes, captured.written.front()}});
    };
    implementation.create = fixture::Effect<build_implementation>::create;
    const auto selection = implementation.identity;
    CHECK(server->registerEffectImplementation(std::move(implementation)));

    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread worker([&] { CHECK(server->run()); });
    const auto commands = queryCommands(*client);
    DeviceOptions options;
    options.texture_descriptors = true;
    if (use_materials) options.cached_unused_programs = 0;
    UniformUploadProbe::watch_texture_pool = !use_materials && !d3d12 && !upload_fault;
#ifdef _WIN32
    UniformUploadProbe::test_unpublished_retirement = use_materials;
#endif
    options.descriptor_initial_counts[0] = UniformUploadProbe::watch_texture_pool ? 1 : 2;
    options.multiple_queues = upload_fault;
    if (d3d12) options.backend = EGraphicsBackend::D3D12;
    const auto created = call<CreateDeviceSession>(*client, commands, options, false);
    if (created.error.type == kDeviceUnsupported)
    {
        server->stop();
        worker.join();
        std::puts("Shared texture descriptor tables unsupported on this adapter");
        return 77;
    }
    CHECK(!created.error.type);
    const auto device = created.device;
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto view = call<CreateView>(*client, commands, {scene}).view;
    const auto target = call<CreateTarget>(*client, commands, {
        device, {64, 64, rdesc::ETextureFormat::RGBA8_UNORM}
    }).target;
    const auto readback = call<GetOrCreateResource>(*client, commands, {
        device, ViewScope{scene, view}, EffectCaptureDescriptor::descriptor.type
    }).resource_id;
    const auto owner =
        call<GetOrCreateResource>(*client, commands, {device, DeviceScope{}, kTextureResourceName}).resource_id;
    rdesc::TextureInfo info{};
    info.width = info.height = info.mip_count = 1;
    info.channel = 4;
    info.pixel_format = rdesc::ETexturePixelFormat::RGBA8_UNORM;
    info.color_space = rdesc::ETextureColorSpace::DATA;
    info.mip_ranges[0] = {0, 4, 1, 1};
    constexpr std::array colors{
        std::array{std::byte{63}, std::byte{13}, std::byte{9}, std::byte{255}},
        std::array{std::byte{7}, std::byte{161}, std::byte{29}, std::byte{255}}
    };
#ifdef _WIN32
    if (upload_fault)
    {
        if (!UploadAcquireProbe::qualified)
        {
            server->stop();
            worker.join();
            std::puts("Separate row-capable Copy queue unavailable");
            return 77;
        }
        constexpr std::array<std::byte, 512> full_window{};
        auto failure_info = info;
        failure_info.width = 128;
        failure_info.mip_ranges[0] = {0, full_window.size(), 128, 1};
        UploadAcquireProbe::phase = UploadAcquireProbe::EPhase::ARMED;
        const auto rejected = call<TextureCommands::Upload>(
            *client, commands, {owner, {failure_info, full_window}}, false
        );
        CHECK(rejected.error.type == kDeviceFailure && rejected.texture.isNull());
        CHECK(UploadAcquireProbe::phase == UploadAcquireProbe::EPhase::ACQUIRE_REJECTED);
        CHECK(UploadAcquireProbe::candidate.load() && UploadAcquireProbe::destructions == 0);
        auto retry_client = ipc::IpcCore::instance().createRequester(config.channel_name);
        CHECK(retry_client);
        auto retry = std::async(std::launch::async, [&]
        {
            return call<TextureCommands::Upload>(*retry_client, commands, {owner, {info, colors[0]}}).texture;
        });
        const auto deadline = Clock::now() + 10s;
        while (!UploadAcquireProbe::retries)
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
        CHECK(retry.wait_for(0s) == std::future_status::timeout);
        CHECK(UploadAcquireProbe::destructions == 0 && EffectCaptureBuffer::test_texture_retirement);
        EffectCaptureBuffer::test_texture_retirement->signal();
        textures[0] = retry.get();
    }
#endif
    for (std::size_t i = upload_fault ? 1 : 0; i < colors.size(); ++i)
        textures[i] = call<TextureCommands::Upload>(*client, commands, {owner, {info, colors[i]}}).texture;

    ResourceInstanceId material_owner;
    MaterialProgramId material_program;
    std::uint32_t material_bytes = 0;
    TextureId unused_texture;
    if (use_materials)
    {
        material_owner = call<GetOrCreateResource>(*client, commands, {
            device, DeviceScope{}, kMaterialResourceName
        }).resource_id;
        const auto target = d3d12 ? rdesc::EShaderTarget::DXIL : rdesc::EShaderTarget::SPIRV;
        auto shader = kEffectStreamMaterial.load(target);
        rdesc::MaterialProgramSet program;
        program.target = target;
        for (const auto& set : shader.interface.sets)
            for (const auto& field : set.bindings)
                if (field.name == rdesc::kMaterialTableName) program.parameters = field.block;
        material_bytes = program.parameters.size;
        CHECK(material_bytes != 0);
        program.parameter_defaults = {{{1, 1, 1, 1}}};
        program.texture_count = 2;
        program.programs.push_back({rdesc::EMaterialPurpose::FORWARD, "", std::move(shader)});
        const auto registered = call<MaterialCommands::RegisterProgram>(
            *client, commands, {material_owner, program}, false
        );
        if (registered.error.type == kDeviceUnsupported)
        {
            server->stop();
            worker.join();
            std::puts("Shared buffer descriptor tables unsupported on this adapter");
            return 77;
        }
        CHECK(!registered.error.type);
        material_program = registered.program;
        for (std::size_t i = 0; i < materials.size(); ++i)
        {
            materials[i] = call<MaterialCommands::CreateInstance>(*client, commands, {
                material_program, {{{0, 0, 0, 1}}}, {textures[i], textures[i]}
            }).material;
        }
        unused_texture = call<TextureCommands::Upload>(*client, commands, {owner, {info, colors[1]}}).texture;
        stale_second_texture = call<MaterialCommands::CreateInstance>(*client, commands, {
            material_program, {{{1, 1, 1, 1}}}, {unused_texture, textures[0]}
        }).material;
        const std::array updates{
            MaterialColorUpdate{materials[0], 0, {1, 1, 1, 1}}, MaterialColorUpdate{materials[1], 0, {1, 1, 1, 1}}
        };
        call<MaterialCommands::UpdateColor>(*client, commands, {material_owner, updates});
    }

    const auto create_effect = [&](std::string mode)
    {
        CreateEffectRequest request;
        request.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*request.graph};
        const auto node = take(edit.addNode(definition, selection, std::move(mode)));
        CHECK(edit.setOutput("bytes", edit.pinId(node, node_bytes)));
        CHECK(edit.commit());
        return call<CreateEffect>(*client, commands, std::move(request)).effect;
    };
    const auto new_job = [&](EffectId effect)
    {
        return call<CreateRenderJob>(*client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
    };
    JobBinding binding;
    binding.resources.push_back({"readback", readback});
    const auto undeclared = create_effect("undeclared");
    CHECK(call<PrepareEffect>(*client, commands, {undeclared, device}, false).error.type == kRuntimeUnsupported);
    call<RemoveEffect>(*client, commands, {undeclared});

    using Gate = UniformUploadProbe::EPipelineGate;
    const auto effect = create_effect(use_materials ? "material" : "");
    if (use_materials)
    {
        UniformUploadProbe::pipeline_gate = Gate::ARMED;
        auto compiler = ipc::IpcCore::instance().createRequester(config.channel_name);
        CHECK(compiler);
        auto preparing = std::async(std::launch::async, [&]
        {
            return call<PrepareEffect>(*compiler, commands, {effect, device});
        });
        const auto deadline = Clock::now() + 10s;
        while (UniformUploadProbe::pipeline_gate != Gate::HELD)
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
        CHECK(call<ListEffectJobs>(*client, commands, {effect}).jobs.empty());
        CHECK(call<RemoveEffect>(*client, commands, {effect}, false).error.type == kDeviceBusy);
        CHECK(call<MaterialCommands::UnregisterProgram>(*client, commands, {material_program}, false).error.type == kDeviceBusy);
        UniformUploadProbe::pipeline_gate = Gate::OPEN;
        UniformUploadProbe::pipeline_gate.notify_all();
        preparing.get();
    }
    else call<PrepareEffect>(*client, commands, {effect, device});
    const auto job = new_job(effect);
    if (use_materials) binding.resources.push_back({"materials", material_owner, 0});
    CHECK(call<StartRenderJob>(*client, commands, {device, job, target, binding}, false).error.type == kStorageIdentity);
    binding.resources.push_back({"textures", readback, 0});
    CHECK(call<StartRenderJob>(*client, commands, {device, job, target, binding}, false).error.type == kStorageMismatch);
    binding.resources.back().resource = owner;
    if (use_materials)
    {
        auto wrong_material = binding;
        std::ranges::find(wrong_material.resources, "materials", &JobResourceInput::binding)->resource = owner;
        CHECK(call<StartRenderJob>(*client, commands, {device, job, target, wrong_material}, false).error.type == kStorageMismatch);
    }
    call<StartRenderJob>(*client, commands, {device, job, target, binding});

    const auto before = server->developmentStats();
#ifdef _WIN32
    UniformUploadProbe::gate_texture_gpu = UniformUploadProbe::texture_retirement_fence != nullptr;
#endif
    UniformUploadProbe::submission_gate = Gate::ARMED;
    auto held_client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(held_client);
    auto held = std::async(std::launch::async, [&]
    {
        return call<RequestRenderJob>(*held_client, commands, {job});
    });
    const auto deadline = Clock::now() + 10s;
    while (UniformUploadProbe::submission_gate != Gate::HELD)
    {
        CHECK(Clock::now() < deadline);
        std::this_thread::yield();
    }
    const auto busy = call<TextureCommands::Release>(*client, commands, {textures[0]}, false);
    if (use_materials)
    {
        const auto busy_material = call<MaterialCommands::ReleaseInstance>(*client, commands, {materials[0]}, false);
        CHECK(busy_material.error.type == kDeviceBusy);
    }
    const bool exercise_growth = UniformUploadProbe::watch_texture_pool && UniformUploadProbe::can_grow_texture_table;
    std::vector<std::future<TextureCommands::Upload::Response>> growth;
    if (exercise_growth)
    {
        for (std::size_t i = 0; i < 7; ++i)
        {
            auto growth_client = ipc::IpcCore::instance().createRequester(config.channel_name);
            CHECK(growth_client);
            growth.push_back(std::async(std::launch::async, [&, requester = std::move(*growth_client)]() mutable
            {
                return call<TextureCommands::Upload>(requester, commands, {owner, {info, colors.front()}});
            }));
        }
        const auto growth_deadline = Clock::now() + 10s;
        auto& pools = UniformUploadProbe::texture_pools;
        while (pools.back().creates == 0)
        {
            CHECK(Clock::now() < growth_deadline);
            std::this_thread::yield();
        }
        CHECK(UniformUploadProbe::old_pool_command.load());
        CHECK(pools.front().creates == 1 && pools.front().destroys == 0);
        CHECK(held.wait_for(0s) == std::future_status::timeout);
        CHECK(call<TextureCommands::Release>(*client, commands, {textures[0]}, false).error.type == kDeviceBusy);
    }
    UniformUploadProbe::submission_gate = Gate::OPEN;
    UniformUploadProbe::submission_gate.notify_all();
    CHECK(busy.error.type == kDeviceBusy);
    const auto first = held.get();
#ifdef _WIN32
    if (use_materials)
    {
        CHECK(EffectCaptureBuffer::test_texture_retirement);
        for (const auto* mode : {"failed-layout", "failed-link"})
        {
            const auto invalid = create_effect(mode);
            UniformUploadProbe::cold_prefix_destroyed = false;
            UniformUploadProbe::watch_cold_prefix = true;
            const auto failed = call<PrepareEffect>(*client, commands, {invalid, device}, false);
            CHECK(failed.error.type == kGraphInvalidDescriptor && !UniformUploadProbe::watch_cold_prefix);
            const auto retired = Clock::now() + 10s;
            while (!UniformUploadProbe::cold_prefix_destroyed)
            {
                CHECK(Clock::now() < retired);
                std::this_thread::yield();
            }
            CHECK(!UniformUploadProbe::held_pipeline_destroyed);
            call<RemoveEffect>(*client, commands, {invalid});
        }
        EffectCaptureBuffer::test_texture_retirement->signal();
        std::puts("COLD PREFIX PASS: native preparation and CPU link failures released unpublished PSOs behind a held GPU fence");
    }
#endif
    if (exercise_growth)
    {
#ifdef _WIN32
        CHECK(UniformUploadProbe::texture_pools.front().destroys == 0);
        CHECK(EffectCaptureBuffer::test_texture_retirement);
        EffectCaptureBuffer::test_texture_retirement->signal();
#endif
        for (auto& request : growth)
        {
            const auto uploaded = request.get();
            call<TextureCommands::Release>(*client, commands, {uploaded.texture});
        }
    }
    call<TextureCommands::Release>(*client, commands, {textures[0]});
    auto second_color = colors[1];
    if (!use_materials)
    {
        const auto source = textures[1];
        second_color = {std::byte{31}, std::byte{109}, std::byte{221}, std::byte{255}};
        const TexturePatchData patch{source, {0, 0, 0, 1, 1, 4}, second_color};
        auto clone_client = ipc::IpcCore::instance().createRequester(config.channel_name);
        CHECK(clone_client);
        UniformUploadProbe::submission_gate = Gate::ARMED;
        auto cloning = std::async(std::launch::async, [&]
        {
            return call<TextureCommands::CloneRegion>(*clone_client, commands, {patch});
        });
        const auto clone_deadline = Clock::now() + 10s;
        while (UniformUploadProbe::submission_gate != Gate::HELD)
        {
            CHECK(Clock::now() < clone_deadline);
            std::this_thread::yield();
        }
        CHECK(call<TextureCommands::Release>(*client, commands, {source}, false).error.type == kDeviceBusy);
        UniformUploadProbe::submission_gate = Gate::OPEN;
        UniformUploadProbe::submission_gate.notify_all();
        textures[1] = cloning.get().texture;
        CHECK(textures[1] != source);
        call<TextureCommands::Release>(*client, commands, {source});
        CHECK(call<TextureCommands::Release>(*client, commands, {source}, false).error.type == kStorageIdentity);
    }
    selected_texture = 1;
    const auto second = call<RequestRenderJob>(*client, commands, {job});
    const auto after = server->stats();
    if (before)
    {
        const auto reads = server->developmentStats();
        CHECK(reads && reads->raster_image_ranges == before->raster_image_ranges + 2);
        CHECK(reads->raster_image_subresources == before->raster_image_subresources + 2);
        if (use_materials)
        {
            CHECK(reads->raster_buffer_ranges == before->raster_buffer_ranges + 2);
            CHECK(reads->raster_buffer_bytes == before->raster_buffer_bytes + 3 * material_bytes);
        }
    }
    selected_texture = 0;
    CHECK(call<RequestRenderJob>(*client, commands, {job}, false).error.type == kStorageIdentity);
    CHECK(server->stats().submissions == after.submissions);
    CHECK(first.slot_index != second.slot_index);

    stopAndRemoveJob(*client, commands, job);
    call<RemoveEffect>(*client, commands, {effect});
    if (use_materials)
    {
        selected_texture = 1;
        const auto misplaced = create_effect("misplaced-table");
        call<PrepareEffect>(*client, commands, {misplaced, device});
        const auto wrong_grant = new_job(misplaced);
        const auto rejected = call<StartRenderJob>(*client, commands, {device, wrong_grant, target, binding}, false);
        CHECK(rejected.error.type == kStorageMismatch && server->stats().submissions == after.submissions);
        call<RemoveRenderJob>(*client, commands, {wrong_grant});
        call<RemoveEffect>(*client, commands, {misplaced});
    }
    call<RemoveView>(*client, commands, {scene, view});
    if (use_materials)
    {
        const auto retirement_deadline = Clock::now() + 10s;
        while (!UniformUploadProbe::held_pipeline_destroyed.load())
        {
            CHECK(Clock::now() < retirement_deadline);
            std::this_thread::yield();
        }
        std::puts("MATERIAL EFFECT PASS: compiled PSO retired after Effect removal while material records remained live");
        call<MaterialCommands::ReleaseInstance>(*client, commands, {stale_second_texture});
        call<TextureCommands::Release>(*client, commands, {unused_texture});
        for (auto material : materials) call<MaterialCommands::ReleaseInstance>(*client, commands, {material});
        call<MaterialCommands::UnregisterProgram>(*client, commands, {material_program});
    }
    call<TextureCommands::Release>(*client, commands, {textures[1]});
    server->stop();
    worker.join();
    CHECK(server->stats().validation_errors == 0);
    const auto& capture = EffectCaptureBuffer::pixels.front();
    CHECK(capture.ready.load() == 1 && capture.frames.size() == 2);
    const std::array expected{std::pair{first.slot_index, colors[0]}, std::pair{second.slot_index, second_color}};
    for (const auto& [slot, color] : expected)
    {
        const auto pixel = std::span<const std::byte>{capture.frames[slot]}.subspan(capture.pitch * 32 + 16 * 4, 4);
        CHECK(std::ranges::equal(pixel, color));
    }
#ifdef _WIN32
    if (upload_fault)
    {
        CHECK(!UploadAcquireProbe::candidate.load() && UploadAcquireProbe::destructions == 1);
        std::puts("UPLOAD HANDOFF PASS: rejected Acquire, fence retirement, staging reuse and retry pixels");
    }
#endif
    if (use_materials)
    {
        const auto rendered = std::span<const std::byte>{capture.frames[second.slot_index]};
        const auto right = rendered.subspan(capture.pitch * 32 + 48 * 4, 4);
        constexpr std::array white{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
        CHECK(std::ranges::equal(right, white));
        std::puts("RECORD TABLES PASS: sampled left and record-only right; unused Program and stale texture suffix excluded");
    }
    if (!use_materials) std::puts("TEXTURE CLONE PASS: pending source protection, stale ID and patched pixel");
    if (exercise_growth)
    {
        for (const auto& generation : UniformUploadProbe::texture_pools)
            CHECK(generation.creates == 1 && generation.destroys == 1);
        std::puts("VULKAN POOLS PASS: 2 -> 4 -> 8 -> 16 before receipt; original binding and all destructions verified");
    }
    std::printf("%s PASS backend=%s native pixels, deduplicated reads, release protection, growth=%s\n",
        use_materials ? "SHARED MATERIAL" : "SHARED TEXTURE", d3d12 ? "D3D12" : "Vulkan",
        exercise_growth ? "verified" : "not exercised");
    return 0;
}

static void meshRasterStream(bool d3d12)
{
    enum class EDraw
    {
        DIRECT, INDIRECT, ARENA, MIXED_GEOMETRY, INDEX_WIDTH, VERTEX_LAYOUT, STALE, SHORT_BUFFER, RELEASED
    };
    constexpr auto arena_bytes = 2 * sizeof(nri::DrawIndexedDesc);
    constexpr std::uint64_t minimum_argument_bytes = 84;
    constexpr std::uint64_t maximum_argument_bytes = 168;
    constexpr std::uint64_t tail_indirect_offset = 120;
    static std::atomic draw_mode{EDraw::DIRECT};
    // Only numeric identities escape the synchronous call. Rebuilding a nonempty table while the
    // previous revision is still published necessarily allocates different backing.
    static const auto observe_selection = [](const ResourceViewValue<MeshDrawTopology>& selected) noexcept
    {
        const auto& mesh = selected.value.get();
        CHECK(mesh.draw_groups.size() == 1 && mesh.surfaces.size() == 1);
        CHECK(mesh.group_sources.size() == 1 && mesh.camera_dispatch[0] == 1 && mesh.meshes.size() == 1);
        CHECK(mesh.draw_groups.front().candidate_count == 1 && mesh.group_sources.front() == mesh.surfaces.front().source);
        const std::array identity{
            reinterpret_cast<std::uintptr_t>(&mesh),
            reinterpret_cast<std::uintptr_t>(mesh.meshes.data()),
            reinterpret_cast<std::uintptr_t>(mesh.draw_groups.data())
        };
        static std::uint64_t revision = 0;
        static std::array<std::uintptr_t, 3> previous{};
        if (revision == selected.revision) CHECK(std::ranges::equal(identity, previous));
        else
        {
            CHECK(selected.revision > revision);
            previous = identity;
            revision = selected.revision;
        }
    };
    static std::array<MeshId, 2> meshes;
    static std::uint32_t vertex_stride;
    static GeometryAttribute position;
    class Producer final
    {
    public:
        struct Configuration final
        {
            GraphResourceId arguments;
            GraphResourceId arena_arguments;
        };

        struct LinkedData final
        {
            DrawStreamSlot stream;
            RasterResourceSlot meshes;
            RasterBufferSlot arguments;
            RasterBufferSlot arena_arguments;
            std::array<RasterProgramSlot, 2> programs;
        };

        static RenderResult<LinkedData>
        resolve(const Configuration& input, std::span<const RasterOperands, 1> streams) noexcept
        {
            return LinkedData{
                streams.front().stream(),
                take(streams.front().resource("meshes", MeshResource::descriptor.type)),
                take(streams.front().buffer(input.arguments)),
                take(streams.front().buffer(input.arena_arguments)),
                {take(streams.front().programAt(0)), take(streams.front().programAt(1))}
            };
        }

        struct Workspace final
        {
            MeshDrawBinding meshes;
            std::optional<RasterIndexedGeometry> original;
        };

        static RenderResult<std::unique_ptr<Workspace>>
        install(const LinkedData& linked, const TargetDesc&, const InstalledDrawSources& sources) noexcept
        {
            auto binding = MeshDrawBinding::bind(sources, linked.meshes);
            if (!binding) return lux::cxx::unexpected(binding.error());
            return std::make_unique<Workspace>(*binding);
        }

        static RenderResult<void>
        onFrame(
            const LinkedData& linked,
            const EffectFrameInputs<MeshDrawTopology>& inputs,
            EffectFrameWriter& frame,
            Workspace& workspace
        ) noexcept
        {
            const auto& snapshot = std::get<0>(inputs.values);
            observe_selection(snapshot);
            const auto mode = draw_mode.load();
            const auto bytes = mode == EDraw::INDIRECT ? maximum_argument_bytes : minimum_argument_bytes;
            const std::array<std::uint64_t, 1> extents{bytes};
            const auto program = linked.programs[mode == EDraw::VERTEX_LAYOUT ? 1 : 0];
            std::array<nri::DrawIndexedDesc, 2> commands{};
            if (mode == EDraw::ARENA)
            {
                const ProducerConstants constants{191};
                std::array<RasterStreamVertex, 2> vertices;
                std::array<RasterStreamDraw, 2> draws;
                for (std::uint32_t i = 0; i < draws.size(); ++i)
                {
                    auto mesh = workspace.meshes.geometry(meshes[i]);
                    if (!mesh) return lux::cxx::unexpected(mesh.error());
                    commands[i] = {6, 1, mesh->arena_first_index, mesh->arena_vertex_offset, 0};
                    vertices[i] = {0, mesh->vertices};
                    draws[i] = {
                        program, {}, {i, 1}, {0, sizeof(constants)}, RasterStreamIndirect{
                            {linked.arena_arguments, i * sizeof(nri::DrawIndexedDesc), sizeof(nri::DrawIndexedDesc)},
                            {},
                            RasterStreamIndexBinding{mesh->indices, mesh->index_type, ERasterIndexOrigin::BUFFER_BASE}
                        }
                    };
                }
                const std::array uploads{FrameBufferUpload{std::as_bytes(std::span{commands}), 2}};
                auto written = frame.publishBuffers(uploads, extents);
                if (!written) return written;
                return frame.setRasterStream(linked.stream, {
                    draws, {}, vertices, std::as_bytes(std::span{&constants, 1})
                });
            }
            const std::array uploads{FrameBufferUpload{std::as_bytes(std::span{commands}), 1}};
            auto written = frame.publishBuffers(uploads, extents);
            if (!written) return written;
            const auto& selection = snapshot.value.get();
            const auto mesh = mode == EDraw::RELEASED ? meshes[0] : selection.meshes[selection.mesh_programs[selection.surfaces.front().source].mesh];
            auto geometry = workspace.meshes.geometry(mesh);
            if (!geometry) return lux::cxx::unexpected(geometry.error());
            if (mode == EDraw::MIXED_GEOMETRY || mode == EDraw::DIRECT)
            {
                const auto other = mode == EDraw::MIXED_GEOMETRY ? meshes[0] : mesh;
                const auto second = workspace.meshes.geometry(other);
                if (!second) return lux::cxx::unexpected(second.error());
                geometry->indices = second->indices;
            }
            if (!workspace.original) workspace.original = *geometry;
            if (mode == EDraw::STALE) geometry = *workspace.original;
            if (mode == EDraw::INDEX_WIDTH) geometry->index_type = EIndexType::UINT16;

            const RasterStreamVertex vertex{0, geometry->vertices};
            const ProducerConstants constants{191};
            RasterStreamDraw draw{
                program,
                {}, {0, 1}, {0, sizeof(constants)}, RasterStreamIndexed{geometry->indices, geometry->index_type, 6}
            };
            if (mode == EDraw::INDIRECT || mode == EDraw::SHORT_BUFFER)
            {
                draw.draw = RasterStreamIndirect{
                    {linked.arguments, tail_indirect_offset, sizeof(nri::DrawIndexedDesc)}, {},
                    RasterStreamIndexBinding{geometry->indices, geometry->index_type}
                };
            }
            const std::array draws{draw, draw};
            return frame.setRasterStream(linked.stream, {
                draws, {}, std::span{&vertex, 1}, std::as_bytes(std::span{&constants, 1})
            });
        }
    };

    RenderServerConfig config;
    config.channel_name = "lux.render.test.mesh-stream";
    config.frames_in_flight = 3;
    config.validation = nativeValidationEnabled();
    config.uploads.emplace();
    auto server = take(RenderServer::create(config, lux::execution::ExecutionRuntime::instance()));
    take(server->registerResource<EffectCaptureBuffer>());
    EffectCapability definition;
    constexpr std::string_view name = "test.mesh_raster_stream";
    definition.id = graph::nodeTypeId(name);
    definition.canonical_name = name;
    const EffectPortDefinition definition_ports[]{{node_bytes, "bytes", {"test.readback_bytes", 1}, graph::EPinDirection::OUTPUT}};
    definition.ports = definition_ports;
    CHECK(server->registerEffectCapability(definition));
    ImplementationEntry implementation;
    implementation.capability = definition.id;
    implementation.identity = {"test.mesh_raster_stream.draw", 1};
    implementation.supported_outputs = {node_bytes};
    static constexpr auto build_implementation = [](EffectCompileContext& context, std::string_view) noexcept -> RenderResult<void>
    {
        constexpr ResourceRequirement instances_source{
            .binding = "instances", .type = kInstanceResourceName, .domain = EResourceDomain::SCENE, .scope_default = true
        };
        const auto instances = take(context.readResourceView<MeshDrawTopology>(instances_source, "instances"));
        const auto readback = take(context.importFinalBuffer(
            "readback", EffectCaptureDescriptor::descriptor.type,
            {"", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION}
        ));
        const auto arguments = take(context.declareFrameBuffer(
            "mesh indirect arguments", minimum_argument_bytes, maximum_argument_bytes
        ));
        const auto count = take(context.declareResource({"mesh indirect count scratch", GraphBufferDesc{16}}));
        const auto arena = take(context.declarePreparedBuffer(
            "arena arguments", arena_bytes, arena_bytes, EGraphUse::INDIRECT
        ));
        ComputeWork generate;
        generate.interface.program = take(context.addProgram(GraphProgram{
            kEffectStreamIndirect.load(context.shaderTarget())
        }));
        generate.interface.resources = {
            {"arguments", {arguments, {}, EGraphAccess::WRITE}},
            {"count", {{count, 0}, {}, EGraphAccess::WRITE}}
        };
        generate.dispatch = DispatchArguments{{1, 1, 1}};
        const auto generated = take(context.addComputePass("generate mesh draw", std::move(generate)));

        RasterPassTemplate stage;
        stage.purpose = "mesh-stream";
        stage.draw_resources = {
            {.binding = "meshes", .source = ResourceName{MeshResource::descriptor.type.name()}, .reads = EDrawResourceRead::INDEXED_GEOMETRY}
        };
        auto shader = streamShader(context.shaderTarget(), kEffectStreamInline, kEffectStreamMesh);
        const auto& attribute = shader.interface.vertex_inputs.front();
        if (context.shaderTarget() == rdesc::EShaderTarget::DXIL)
            CHECK(attribute.semantic == "POSITION" && attribute.semantic_index == 7);
        else CHECK(attribute.location == 3);
        GraphicsState graphics;
        graphics.vertex_bindings = {{0, vertex_stride}};
        graphics.vertex_attributes = {{attribute.location, 0, position.offset, position.format}};
        const auto id = take(context.addProgram(GraphProgram{std::move(shader)}));
        ParameterBinding parameters;
        parameters.setInlineConstant(ProducerConstants{});
        RasterPassTemplate::StreamProgram normal{id, graphics, parameters, {position.location}};
        auto wrong_layout = normal;
        wrong_layout.graphics.vertex_attributes.front().offset += sizeof(float);
        stage.programs = std::vector{std::move(normal), std::move(wrong_layout)};
        stage.resources = {
            {.version = generated.written.front(), .usage = EGraphUse::INDIRECT},
            {.version = arena, .usage = EGraphUse::INDIRECT}
        };
        const auto color = take(context.declareResource({"mesh output", GraphImageDesc{64, 64}}));
        RasterWork raster;
        raster.program = std::move(stage);
        raster.colors.push_back({{{color, 0}}});
        const auto drawn = take(context.addRasterPass("draw uploaded mesh", std::move(raster)));
        auto producer = Producer::Configuration{
            arguments.resource, arena.resource
        };
        CHECK((context.onFrame<&Producer::onFrame, &Producer::resolve, &Producer::install>(std::move(producer), {
            .uploads = {arena.resource}, .frame_sizes = {arguments.resource},
            .resource_views = {instances}, .draw_streams = {drawn.pass}
        })));
        const auto captured = take(context.addTransferPass("copy mesh pixels", TransferWork{{CopyImageBuffer{
            {drawn.written.front()}, {readback}, 256, effect_capture_bytes, ImageCopyRegion{0, 0, 64, 64}
        }}}));
        return context.publish(node_bytes, {{node_bytes, captured.written.front()}});
    };
    implementation.create = fixture::Effect<build_implementation>::create;
    const auto implementation_id = implementation.identity;
    CHECK(server->registerEffectImplementation(std::move(implementation)));

    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread worker([&] { CHECK(server->run()); });
    const auto commands = queryCommands(*client);
    DeviceOptions options;
    if (d3d12) options.backend = EGraphicsBackend::D3D12;
    const auto device = call<CreateDeviceSession>(*client, commands, options).device;
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto view = call<CreateView>(*client, commands, {scene}).view;
    const auto target = call<CreateTarget>(*client, commands, {
        device, {64, 64, rdesc::ETextureFormat::RGBA8_UNORM}
    }).target;
    const auto readback = call<GetOrCreateResource>(*client, commands, {
        device, ViewScope{scene, view}, EffectCaptureDescriptor::descriptor.type
    }).resource_id;
    const auto owner = call<GetOrCreateResource>(*client, commands, {device, DeviceScope{}, kMeshResourceName})
        .resource_id;
    rdesc::Mesh mesh;
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.vertices.resize(4);
    for (auto& vertex : mesh.vertices)
    {
        vertex.normal = {0, 0, 1};
        vertex.tangent = {1, 0, 0};
        vertex.bitangent = {0, 1, 0};
        vertex.uv.setZero();
    }
    for (std::size_t i = 0; i < meshes.size(); ++i)
    {
        const auto x = static_cast<float>(i) - 1.f;
        mesh.vertices[0].position = {x, -1, 0};
        mesh.vertices[1].position = {x + 1, -1, 0};
        mesh.vertices[2].position = {x + 1, 1, 0};
        mesh.vertices[3].position = {x, 1, 0};
        meshes[i] = call<MeshCommands::UploadMesh>(*client, commands, {owner, &mesh}).mesh;
    }
    const auto geometry = take(describeIndexedMesh(mesh));
    vertex_stride = geometry.vertex_stride;
    position = geometry.attributes.front();
    const auto instances = call<GetOrCreateResource>(*client, commands, {device, scene, kInstanceResourceName})
        .resource_id;
    const auto transforms = call<QueryResource>(*client, commands, {device, scene, kTransformResourceName})
        .resource_id;
    const auto materials = call<GetOrCreateResource>(*client, commands, {device, DeviceScope{}, kMaterialResourceName})
        .resource_id;
    rdesc::MaterialProgramSet material_program;
    material_program.target = d3d12 ? rdesc::EShaderTarget::DXIL : rdesc::EShaderTarget::SPIRV;
    material_program.programs.push_back({
        rdesc::EMaterialPurpose::FORWARD, "", kEffectStreamInline.load(material_program.target)
    });
    const auto compiled = call<MaterialCommands::RegisterProgram>(*client, commands, {
        materials, std::move(material_program)
    }).program;
    const auto material = call<MaterialCommands::CreateInstance>(*client, commands, {compiled, {}, {}}).material;
    auto matrix = kIdentityTransform;
    const auto transform = call<TransformCommands::Create>(*client, commands, {transforms, std::span{&matrix, 1}})
        .transforms.front();
    MeshInstanceData selected_instance{meshes[1], material, 0, transform, false, false};
    const auto disabled = call<InstanceCommands::CreateInstance>(*client, commands, {instances, selected_instance}).instance;
    selected_instance.render_enabled = true;
    const auto instance = call<InstanceCommands::CreateInstance>(*client, commands, {instances, selected_instance})
        .instance;
    const auto select_mesh = [&](MeshId selected)
    {
        selected_instance.mesh = selected;
        auto inactive = selected_instance;
        inactive.render_enabled = false;
        const std::array updates{MeshInstanceUpdate{disabled, inactive}, MeshInstanceUpdate{instance, selected_instance}};
        call<InstanceCommands::Update>(*client, commands, {instances, updates});
    };

    CreateEffectRequest request;
    request.graph = std::make_unique<EffectGraph>();
    EffectGraphEdit edit{*request.graph};
    const auto node = take(edit.addNode(definition, implementation_id));
    CHECK(edit.setOutput("bytes", edit.pinId(node, node_bytes)));
    CHECK(edit.commit());
    const auto effect = createPreparedEffect(*client, commands, device, std::move(request));
    const auto job = call<CreateRenderJob>(*client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
    JobBinding binding;
    binding.resources = {
        {"readback", readback}, {"instances", instances, 0},
        {"meshes", readback, 0}
    };
    const auto rejected = call<StartRenderJob>(*client, commands, {device, job, target, binding}, false);
    if (rejected.error.type != kStorageMismatch)
        std::fprintf(stderr, "Mesh binding error=%llu, column=%llu\n",
            static_cast<unsigned long long>(rejected.error.type),
            static_cast<unsigned long long>(rejected.error.args[0])
        );
    CHECK(rejected.error.type == kStorageMismatch);
    binding.resources.back().resource = owner;
    using Gate = UniformUploadProbe::EPipelineGate;
    select_mesh(meshes[0]);
    call<StartRenderJob>(*client, commands, {device, job, target, binding});

    const auto before = server->developmentStats();
    UniformUploadProbe::submission_gate = Gate::ARMED;
    auto held_client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(held_client);
    auto held = std::async(std::launch::async, [&]
    {
        return call<RequestRenderJob>(*held_client, commands, {job});
    });
    const auto deadline = Clock::now() + 10s;
    while (UniformUploadProbe::submission_gate != Gate::HELD)
    {
        CHECK(Clock::now() < deadline);
        std::this_thread::yield();
    }
    const auto busy = call<MeshCommands::ReleaseMesh>(*client, commands, {meshes[0]}, false);
    UniformUploadProbe::submission_gate = Gate::OPEN;
    UniformUploadProbe::submission_gate.notify_all();
    CHECK(busy.error.type == kDeviceBusy);
    const auto first = held.get();
    const auto peer_view = call<CreateView>(*client, commands, {scene}).view;
    const auto peer_target = call<CreateTarget>(*client, commands, {
        device, {64, 64, rdesc::ETextureFormat::RGBA8_UNORM}
    }).target;
    const auto peer = call<CreateRenderJob>(*client, commands, {scene, peer_view, effect, EJobCadence::REQUESTED}).job;
    auto peer_binding = binding;
    peer_binding.resources.front().resource = call<GetOrCreateResource>(*client, commands, {
        device, ViewScope{scene, peer_view}, EffectCaptureDescriptor::descriptor.type
    }).resource_id;
    call<StartRenderJob>(*client, commands, {device, peer, peer_target, std::move(peer_binding)});
    select_mesh(meshes[1]);
    draw_mode = EDraw::INDIRECT;
    const auto second = call<RequestRenderJob>(*client, commands, {job});
    // A disabled member can change geometry without selecting new programs for the active mesh.
    MeshInstanceUpdate inactive_change{disabled, selected_instance};
    inactive_change.description.render_enabled = false;
    inactive_change.description.mesh = meshes[0];
    call<InstanceCommands::Update>(*client, commands, {instances, {&inactive_change, 1}});
    inactive_change.description.mesh = selected_instance.mesh;
    call<InstanceCommands::Update>(*client, commands, {instances, {&inactive_change, 1}});
    matrix[0] = 2;
    const TransformRecordUpdate transform_update{transform, matrix};
    call<TransformCommands::Update>(*client, commands, {transforms, std::span{&transform_update, 1}});
    const auto indirect_calls = UniformUploadProbe::indexed_indirect_calls.load();
    const auto indirect_draws = UniformUploadProbe::indexed_indirect_draws.load();
    draw_mode = EDraw::ARENA;
    const auto third = call<RequestRenderJob>(*client, commands, {job});
    const auto expected_calls = UniformUploadProbe::can_batch_indexed ? 1u : 2u;
    CHECK(UniformUploadProbe::indexed_indirect_calls == indirect_calls + expected_calls);
    CHECK(UniformUploadProbe::indexed_indirect_draws == indirect_draws + 2);
    CHECK(first.slot_index != second.slot_index && first.slot_index != third.slot_index);
    CHECK(second.slot_index != third.slot_index);
    draw_mode = EDraw::MIXED_GEOMETRY;
    const auto before_mixed = server->stats().submissions;
    CHECK(call<RequestRenderJob>(*client, commands, {job}, false).error.type == kGraphInvalidRange);
    CHECK(server->stats().submissions == before_mixed);
    call<MeshCommands::ReleaseMesh>(*client, commands, {meshes[0]});
    if (before)
    {
        const auto reads = server->developmentStats();
        CHECK(reads && reads->raster_geometry_ranges == before->raster_geometry_ranges + 8);
        const auto bytes = 4 * (4 * vertex_stride + 6 * sizeof(std::uint32_t));
        CHECK(reads->raster_geometry_bytes == before->raster_geometry_bytes + bytes);
    }

    const auto indexed_after = UniformUploadProbe::indexed_calls.load();
    const auto indirect_after = UniformUploadProbe::indexed_indirect_calls.load();
    const auto accepted = server->developmentStats();
    const std::array failures{
        EDraw::INDEX_WIDTH, EDraw::VERTEX_LAYOUT, EDraw::STALE, EDraw::SHORT_BUFFER, EDraw::RELEASED
    };
    for (const auto mode : failures)
    {
        draw_mode = mode;
        const auto failure = call<RequestRenderJob>(*client, commands, {job}, false);
        const auto expected = mode == EDraw::RELEASED ? kStorageIdentity :
            mode == EDraw::SHORT_BUFFER ? kDeviceArgument : kGraphInvalidRange;
        CHECK(failure.error.type == expected);
        CHECK(UniformUploadProbe::indexed_calls == indexed_after);
        CHECK(UniformUploadProbe::indexed_indirect_calls == indirect_after);
        if (accepted)
        {
            const auto reads = server->developmentStats();
            CHECK(reads && reads->raster_geometry_ranges == accepted->raster_geometry_ranges);
        }
    }
    draw_mode = EDraw::DIRECT;
    const auto peer_frame = call<RequestRenderJob>(*client, commands, {peer});
    stopAndRemoveJob(*client, commands, peer);
    call<RemoveView>(*client, commands, {scene, peer_view});
    stopAndRemoveJob(*client, commands, job);
    call<RemoveView>(*client, commands, {scene, view});
    call<InstanceCommands::ReleaseInstance>(*client, commands, {disabled});
    call<InstanceCommands::ReleaseInstance>(*client, commands, {instance});
    call<MeshCommands::ReleaseMesh>(*client, commands, {meshes[1]});
    server->stop();
    worker.join();
    CHECK(server->stats().validation_errors == 0);
    const auto& capture = EffectCaptureBuffer::pixels.front();
    CHECK(capture.ready.load() == 1 && capture.frames.size() == 3);
    for (const auto [slot, x] : {std::pair{first.slot_index, 16u}, std::pair{second.slot_index, 48u}})
    {
        const auto& pixels = capture.frames[slot];
        CHECK(pixels[capture.pitch * 32 + x * 4] == std::byte{191});
        CHECK(pixels[capture.pitch * 32 + (64 - x) * 4] == std::byte{0});
    }
    const auto& peer_pixels = EffectCaptureBuffer::pixels[1];
    CHECK(peer_pixels.ready.load() == 1);
    const auto& peer_image = peer_pixels.frames[peer_frame.slot_index];
    CHECK(peer_image[peer_pixels.pitch * 32 + 48 * 4] == std::byte{191});
    CHECK(peer_image[peer_pixels.pitch * 32 + 16 * 4] == std::byte{0});
    const auto& shared = capture.frames[third.slot_index];
    CHECK(shared[capture.pitch * 32 + 16 * 4] == std::byte{191});
    CHECK(shared[capture.pitch * 32 + 48 * 4] == std::byte{191});
    std::printf("MESH STREAM PASS backend=%s direct, indirect and arena pixels, indexed arena calls=%u\n",
        d3d12 ? "D3D12" : "Vulkan", expected_calls);
}

static void basicFeatureOutputs(bool d3d12)
{
    RenderServerConfig config;
    config.channel_name = "lux.render.test.hzb-feature";
    config.frames_in_flight = 2;
    config.validation = nativeValidationEnabled();
    DeviceOptions options;
    options.texture_descriptors = true;
    if (d3d12) options.backend = EGraphicsBackend::D3D12;
    auto& execution = lux::execution::ExecutionRuntime::instance();
    auto server = take(RenderServer::create(config, execution));
    registerOutputNodes(*server);
    take(server->registerResource<EffectCaptureBuffer>());

    EffectCapability depth_source;
    constexpr std::string_view depth_name = "test.depth.clear";
    depth_source.id = graph::nodeTypeId(depth_name);
    depth_source.canonical_name = depth_name;
    const EffectPortDefinition depth_source_ports[]{{DepthReductionPorts::depth.semantic, "depth", {DepthPyramid::depth_type, 1},
        graph::EPinDirection::OUTPUT}};
    depth_source.ports = depth_source_ports;
    CHECK(server->registerEffectCapability(depth_source));
    ImplementationEntry clear_depth;
    clear_depth.capability = depth_source.id;
    clear_depth.identity = {"test.depth.clear.fixed", 1};
    clear_depth.supported_outputs = {DepthReductionPorts::depth.semantic};
    static constexpr auto build_clear_depth = [](EffectCompileContext& context, std::string_view) noexcept -> RenderResult<void>
    {
        GraphImageDesc depth{64, 64, 1, 1, rdesc::ETextureFormat::D32_SFLOAT};
        depth.extent.source = EImageExtentSource::TARGET;
        const auto image = take(context.declareResource({"forward depth", depth}));
        const auto cleared = take(context.addTransferPass(
            "clear forward depth", TransferWork{{ClearImage{{{image, 0}, GraphImageRange{2}}, {}, .25f}}}
        ));
        return context.publish(DepthReductionPorts::depth.semantic, {{DepthReductionPorts::depth.semantic, cleared.written.front()}});
    };
    clear_depth.create = fixture::Effect<build_clear_depth>::create;
    const auto depth_selection = clear_depth.identity;
    CHECK(server->registerEffectImplementation(std::move(clear_depth)));

    EffectCapability read_pyramid;
    static constexpr auto pyramid_input = effectPortSemantic("pyramid");
    constexpr std::string_view read_name = "test.depth.pyramid.readback";
    read_pyramid.id = graph::nodeTypeId(read_name);
    read_pyramid.canonical_name = read_name;
    const EffectPortDefinition read_pyramid_ports[]{
        {pyramid_input, "pyramid", {DepthPyramid::pyramid_type, 1}, graph::EPinDirection::INPUT, 1, true},
        {node_bytes, "bytes", {"test.readback_bytes", 1}, graph::EPinDirection::OUTPUT}
    };
    read_pyramid.ports = read_pyramid_ports;
    CHECK(server->registerEffectCapability(read_pyramid));
    ImplementationEntry readback;
    readback.capability = read_pyramid.id;
    readback.identity = {"test.depth.pyramid.readback.texel", 1};
    readback.supported_outputs = {node_bytes};
    static constexpr auto build_readback = [](EffectCompileContext& context, std::string_view key) noexcept -> RenderResult<void>
    {
        const auto source = context.inputs(pyramid_input);
        CHECK(source.size() == 1 && source.front().size() == 1);
        const ResourceNameView resource{EffectCaptureDescriptor::descriptor.type.name()};
        const auto buffer = take(context.importFinalBuffer(key, resource, {
            "", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION
        }));
        const auto setup = take(context.addTransferPass(
            std::string{key} + ".initialize", TransferWork{{FillBuffer{{buffer}, 0}}}
        ));
        const CopyImageBuffer copy{
            {source.front().front().version, GraphImageRange{1, 3, 1, 0, 1}},
            {setup.written.front(), {}, EGraphAccess::READ_WRITE}, 256, 256, ImageCopyRegion{0, 0, 1, 1}
        };
        const auto done = take(context.addTransferPass(std::string{key} + ".copy", TransferWork{{copy}}));
        return context.publish(node_bytes, {{node_bytes, done.written.front()}});
    };
    readback.create = fixture::Effect<build_readback>::create;
    const auto read_selection = readback.identity;
    CHECK(server->registerEffectImplementation(std::move(readback)));

    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread render([&] { CHECK(server->run()); });
    const auto commands = queryCommands(*client);
    const auto request = [&](bool previous, std::string_view mips)
    {
        CreateEffectRequest result;
        result.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*result.graph};
        const auto depth = take(edit.addNode(depth_source, depth_selection, ""));
        const auto pyramid = take(edit.addNode(
            capabilities::DepthReduction, {std::string{"lux.render.depth_pyramid.forward_zo"}, capabilities::DepthReduction.version},
            "mips=" + std::string{mips}
        ));
        CHECK(edit.connect(edit.pinId(depth, DepthReductionPorts::depth.semantic), edit.pinId(pyramid, DepthReductionPorts::depth.semantic)));
        const auto connect = [&](graph::PinSemanticId port, std::string key)
        {
            const auto sink = take(edit.addNode(read_pyramid, read_selection, key));
            CHECK(edit.connect(edit.pinId(pyramid, port), edit.pinId(sink, pyramid_input)));
            CHECK(edit.setOutput(std::move(key), edit.pinId(sink, node_bytes), false));
        };
        connect(DepthReductionPorts::current.semantic, "current");
        if (previous) connect(DepthReductionPorts::previous.semantic, "previous");
        CHECK(edit.commit());
        return result;
    };
    CHECK(call<CreateEffect>(*client, commands, request(true, "0"), false).error.type == kInvalidEffectArguments);
    const auto history = call<CreateEffect>(*client, commands, request(true, "4")).effect;
    const auto current_only = call<CreateEffect>(*client, commands, request(false, "4")).effect;
    const auto session = call<CreateDeviceSession>(*client, commands, options).device;
    for (auto selected : {history, current_only}) call<PrepareEffect>(*client, commands, {selected, session});
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto view = call<CreateView>(*client, commands, {scene}).view;
    const TargetDesc target_desc{64, 64, rdesc::ETextureFormat::RGBA8_UNORM};
    const auto target = call<CreateTarget>(*client, commands, {session, target_desc}).target;
    const auto make_capture = [&](ViewId owner)
    {
        return call<GetOrCreateResource>(*client, commands, {
            session, ViewScope{scene, owner}, EffectCaptureDescriptor::descriptor.type
        }).resource_id;
    };
    const auto draw = [&](EffectId effect, bool previous)
    {
        const auto current_view = call<CreateView>(*client, commands, {scene}).view;
        const auto previous_view = previous ? call<CreateView>(*client, commands, {scene}).view : ViewId{};
        JobBinding binding;
        binding.resources.push_back({"current", make_capture(current_view)});
        if (previous) binding.resources.push_back({"previous", make_capture(previous_view)});
        const auto job = call<CreateRenderJob>(*client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
        call<StartRenderJob>(*client, commands, {session, job, target, binding});
        renderFrame(*client, commands, job);
        renderFrame(*client, commands, job);
        stopAndRemoveJob(*client, commands, job);
        call<RemoveView>(*client, commands, {scene, current_view});
        if (previous) call<RemoveView>(*client, commands, {scene, previous_view});
    };
    draw(history, true);
    draw(current_only, false);
    call<RemoveView>(*client, commands, {scene, view});
    verifyTonemapOutput({*client, commands, session}, scene,
        {rdesc::ETextureFormat::RGBA8_UNORM}, EExpectedEncoding::LINEAR);
    verifyTonemapOutput({*client, commands, session}, scene,
        {rdesc::ETextureFormat::RGBA8_UNORM}, EExpectedEncoding::SRGB);
    verifyTonemapOutput({*client, commands, session}, scene,
        {rdesc::ETextureFormat::RGBA8_SRGB}, EExpectedEncoding::SRGB);
    verifyTonemapOutput({*client, commands, session}, scene,
        {rdesc::ETextureFormat::RGBA16_SFLOAT}, EExpectedEncoding::HDR_LINEAR);
    verifyTargetInput(*client, commands, session, scene);
    const auto secondary = call<CreateDeviceSession>(*client, commands, options).device;
    verifyTargetInput(*client, commands, secondary, scene);
    std::puts("DEVICE HISTORY PASS: sampled and read back published pixels on both sessions");
    std::printf("TONE MAPPING PASS backend=%s linear, shader sRGB, target sRGB, FP16 HDR and node exposure\n",
        d3d12 ? "D3D12" : "Vulkan");
    server->stop();
    render.join();
    const auto values = [](const CapturedPixels& pixels)
    {
        CHECK(pixels.ready.load() == 1 && pixels.frames.size() == 2);
        std::array<float, 2> result{};
        for (std::size_t i = 0; i < result.size(); ++i)
            std::memcpy(&result[i], pixels.frames[i].data(), sizeof(float));
        std::ranges::sort(result);
        return result;
    };
    const auto& pixels = EffectCaptureBuffer::pixels;
    CHECK((values(pixels[0]) == std::array{.25f, .25f}));
    CHECK((values(pixels[1]) == std::array{.25f, 1.f}));
    CHECK((values(pixels[2]) == std::array{.25f, .25f}));
    CHECK(server->stats().validation_errors == 0);
    std::printf("DEPTH PYRAMID PASS backend=%s D32 to R32, mip 3 and optional History\n", d3d12 ? "D3D12" : "Vulkan");
}

#ifdef _WIN32
struct WsiFailureProbe final
{
    struct Descriptor final
    {
        using Config = void;

        static inline const ResourceDefinition descriptor{ResourceNameView{"test.wsi.failure"}, {}, {}, {}};
    };

    static inline std::atomic<bool> fail_view = false;
    static inline decltype(nri::CoreInterface::CreateTextureView) create_texture_view = nullptr;

    static RenderResult<std::unique_ptr<WsiFailureProbe>>
    create(ResourceCreateContext& context, ResourceInstanceId) noexcept
    {
        auto& core = const_cast<nri::CoreInterface&>(context.device.nriCore());
        create_texture_view = core.CreateTextureView;
        core.CreateTextureView = [](const nri::TextureViewDesc& description, nri::Descriptor*& view) noexcept
        {
            if (description.type == nri::TextureView::COLOR_ATTACHMENT && fail_view.exchange(false))
                return nri::Result::OUT_OF_MEMORY;
            return create_texture_view(description, view);
        };
        return std::make_unique<WsiFailureProbe>();
    }

    static ResourceRegistration factory() noexcept
    {
        return makeResourceRegistration<Descriptor, WsiFailureProbe, &create>();
    }
};

static HWND createProbeWindow()
{
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpszClassName = L"LuxRenderOutputProbe";
    window_class.lpfnWndProc = [](HWND window, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT
    {
        if (message == WM_DESTROY)
        {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    };
    CHECK(RegisterClassW(&window_class) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
    return CreateWindowExW(
        0, window_class.lpszClassName, L"LUX Render WSI probe", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 320, 240, nullptr, nullptr, instance, nullptr
    );
}

static void windowOutput(bool d3d12)
{
    std::promise<HWND> opened;
    auto window_result = opened.get_future();
    std::thread ui([&]
    {
        const auto window = createProbeWindow();
        CHECK(window);
        ShowWindow(window, SW_SHOW);
        opened.set_value(window);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    });
    const auto window = window_result.get();
    const auto dimensions = [&]
    {
        RECT client;
        CHECK(GetClientRect(window, &client));
        return std::pair{std::uint32_t(client.right), std::uint32_t(client.bottom)};
    };
    RenderServerConfig config;
    config.channel_name = "lux.render.test.window";
    config.frames_in_flight = 2;
    config.validation = nativeValidationEnabled();
    config.diagnostics.report = [](void*, const RenderDiagnostic& diagnostic) noexcept
    {
        std::fprintf(stderr, "WSI diagnostic %u: %.*s\n", unsigned(diagnostic.severity),
            int(diagnostic.message.size()), diagnostic.message.data());
    };
    DeviceOptions options;
    if (d3d12) options.backend = EGraphicsBackend::D3D12;
    auto& execution = lux::execution::ExecutionRuntime::instance();
    auto server = take(RenderServer::create(config, execution));
    registerOutputNodes(*server);
    take(server->registerResource<WsiFailureProbe>());
    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread render([&] { CHECK(server->run()); });
    const auto commands = queryCommands(*client);
    const auto session = call<CreateDeviceSession>(*client, commands, options).device;
    call<GetOrCreateResource>(*client, commands, {session, DeviceScope{}, WsiFailureProbe::Descriptor::descriptor.type});
    const auto [width, height] = dimensions();
    const TargetDesc description{width, height, rdesc::ETextureFormat::BGRA8_UNORM, 1, ETargetKind::SURFACE};
    auto target = call<CreateTarget>(*client, commands, {session, description, SurfaceDescription{Win32Surface{window}}});
    CHECK(target.output_available && target.description.width == width && target.description.height == height);
    auto surface_request = clearOutputRequest();
    surface_request.output = target.description.outputContract();
    const auto effect = createPreparedEffect(*client, commands, session, std::move(surface_request));
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto view = call<CreateView>(*client, commands, {scene}).view;
    const auto first = startRenderJob(*client, commands, session, scene, view, target.target, effect);
    const auto second = call<CreateRenderJob>(*client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
    CHECK(call<StartRenderJob>(*client, commands, {session, second, target.target}, false).error.type == kTargetInUse);
    // Reject the input after Acquire so the next writer must reuse the unconsumed READY image.
    CHECK(call<RequestRenderJob>(*client, commands, {first, FrameData{}}, false).error.type == kFrameContract);
    CHECK(server->stats().submissions == 0);
    stopAndRemoveJob(*client, commands, first);
    call<StartRenderJob>(*client, commands, {session, second, target.target});
    const auto waitPresented = [&](std::uint64_t count)
    {
        const auto deadline = Clock::now() + 15s;
        while (server->stats().presentations < count)
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::sleep_for(1ms);
        }
    };
    for (int i = 0; i < 6; ++i) renderFrame(*client, commands, second);
    waitPresented(6);

    auto hdr = target.description;
    hdr.display_mode = EDisplayMode::HDR;
    CreateEffectRequest hdr_request;
    hdr_request.graph = std::make_unique<EffectGraph>();
    hdr_request.output = target.description.outputContract();
    hdr_request.output.format = rdesc::ETextureFormat::RGBA16_SFLOAT;
    EffectGraphEdit hdr_edit{*hdr_request.graph};
    const auto hdr_source = take(hdr_edit.addNode(outputSource(true), {std::string{hdr_source_name}, 1}));
    const auto hdr_tone = take(hdr_edit.addNode(capabilities::ToneMapping, {"lux.render.tone_mapping.hdr_linear", 1}, "exposure=2"));
    CHECK(hdr_edit.connect(hdr_edit.pinId(hdr_source, ToneMappingPorts::color.semantic),
                           hdr_edit.pinId(hdr_tone, ToneMappingPorts::hdr_color.semantic)));
    CHECK(hdr_edit.setOutput("color", hdr_edit.pinId(hdr_tone, ToneMappingPorts::color.semantic), true));
    CHECK(hdr_edit.commit());
    const auto hdr_effect = createPreparedEffect(*client, commands, session, std::move(hdr_request));
    const auto switched = call<ResizeTarget>(*client, commands, {target.target, hdr, hdr_effect}, false);
    if (switched.error.type)
    {
        CHECK(switched.error.type == kDeviceUnsupported && switched.output_available);
        CHECK(switched.epoch == target.epoch && switched.description == target.description);
        std::puts("HDR UNSUPPORTED: HDR request preserved the SDR target and writer");
    }
    else
    {
        CHECK(switched.output_available && switched.target == target.target && switched.epoch == target.epoch + 1);
        CHECK(switched.description.display_mode == EDisplayMode::HDR && switched.sdr_white_nits > 0.F);
        CHECK(switched.description.format == rdesc::ETextureFormat::RGBA16_SFLOAT);
        renderFrame(*client, commands, second);
        waitPresented(7);
        target = call<ResizeTarget>(*client, commands, {target.target, target.description, effect});
        CHECK(target.output_available && target.epoch == switched.epoch + 1 && target.sdr_white_nits == 0.F);
        CHECK(target.description.display_mode == EDisplayMode::SDR);
        std::puts("HDR PRESENT PASS: same target and writer returned from linear HDR to SDR");
    }
    call<RemoveEffect>(*client, commands, {hdr_effect});
    const auto before_resize = server->stats().presentations;
    renderFrame(*client, commands, second);
    waitPresented(before_resize + 1);

    CHECK(SetWindowPos(window, nullptr, 0, 0, 400, 300, SWP_NOMOVE | SWP_NOZORDER));
    const auto [changed_width, changed_height] = dimensions();
    auto requested = target.description;
    requested.width = changed_width;
    requested.height = changed_height;
    // The first color view of the replacement fails after the previous swapchain is closed.
    WsiFailureProbe::fail_view = true;
    const auto failed = call<ResizeTarget>(*client, commands, {target.target, requested}, false);
    CHECK(!WsiFailureProbe::fail_view && failed.error.type == kDeviceFailure);
    CHECK(!failed.output_available && failed.target == target.target && failed.epoch == target.epoch);
    CHECK(call<SetTargetActive>(*client, commands, {target.target, true, failed.epoch}, false).error.type ==
        kRuntimeState);
    const auto resized = call<ResizeTarget>(*client, commands, {target.target, requested, effect});
    CHECK(resized.output_available && resized.epoch == target.epoch + 1);
    CHECK(resized.description.width == changed_width && resized.description.height == changed_height);
    CHECK(call<SetTargetActive>(*client, commands, {target.target, true, target.epoch}, false).error.type ==
          kRuntimeState);
    call<SetTargetActive>(*client, commands, {target.target, true, resized.epoch});
    call<RebindRenderJobEffect>(*client, commands, {session, second, effect});
    CHECK(call<StopRenderJob>(*client, commands, first, false).error.type == kJobIdentity);
    for (int i = 0; i < 6; ++i) renderFrame(*client, commands, second);
    waitPresented(before_resize + 7);
    const auto submitted = server->stats().submissions;
    CHECK(call<RequestRenderJob>(*client, commands, {second, FrameData{}}, false).error.type == kFrameContract);
    CHECK(server->stats().submissions == submitted);
    CHECK(call<RemoveTarget>(*client, commands, target.target, false).error.type == kDeviceBusy);
    call<StopRenderJob>(*client, commands, second);
    call<RemoveTarget>(*client, commands, target.target);
    // Reusing the same HWND proves Remove returned the native window association.
    target = call<CreateTarget>(*client, commands, {session, requested, SurfaceDescription{Win32Surface{window}}});
    CHECK(target.output_available);
    call<RemoveRenderJob>(*client, commands, second);
    const auto final_job = startRenderJob(*client, commands, session, scene, view, target.target, effect);
    renderFrame(*client, commands, final_job);
    server->stop(); // The live Target is retired before the window's thread is closed.
    render.join();
    CHECK(server->stats().validation_errors == 0);
    CHECK(PostMessageW(window, WM_CLOSE, 0, 0));
    ui.join();
    std::puts("WSI RETIREMENT PASS: READY Acquire reused after Stop and released by Remove; live Target settled at join");
    std::puts("WSI RETRY PASS: failed replacement reports unavailable; same TargetId recovered");
    std::printf("WSI PASS backend=%s real presents=%llu resize/writer replacement/remove/shutdown\n",
        d3d12 ? "D3D12" : "Vulkan", static_cast<unsigned long long>(server->stats().presentations));
}

struct SignalRetirementFence final
{
    static constexpr std::string_view name{"test.device.signal_retirement"};
    using Request = EmptyRequest;
    using Response = AckResponse;
};

struct SetViewQueueGate final
{
    static constexpr std::string_view name{"test.device.view_queue_gate"};
    struct Request final
    {
        ResourceInstanceId resource;
        bool hold;
    };
    using Data = Request;
    struct Response final
    {
        RenderError error;
        std::uint64_t copied_bytes = 0;
    };
    static ResourceInstanceId resourceId(const Request& request) noexcept { return request.resource; }
};

struct DeviceRetirementProbe final
{
    struct Descriptor final
    {
        using Config = void;

        static inline const ResourceDefinition descriptor{ResourceNameView{"test.device.retirement"}, {}, {}, {}};
    };

    static inline std::array<nri::AdapterDesc, 3> adapters;
    static inline std::array<bool, 3> present_retirement{};
    static inline std::array<std::atomic<bool>, 3> destroyed{};
    static inline std::size_t created = 0;
    static inline decltype(nri::CoreInterface::QueueSubmit) submit = nullptr;
    static inline decltype(nri::CoreInterface::QueueWaitIdle) wait_idle = nullptr;
    static inline RetirementFence* gate = nullptr;
    static inline std::atomic<bool> arm = false, held = false;
    static inline std::atomic<std::int64_t>* accepted_ns = nullptr;
    static inline std::atomic<bool> hold_queue_idle = false, queue_idle_entered = false;
    DeviceSession& session;
    ResourceUploader* uploads;
    std::size_t index;
    std::optional<RetirementFence> fence, view_gate;

    static void setViewGate(
        DeviceRetirementProbe& owner, const SetViewQueueGate::Request& request, SetViewQueueGate::Response& reply
    ) noexcept
    {
        auto& view_gate = owner.view_gate;
        auto& session = owner.session;
        auto& queue = session.queue();
        const auto& core = session.nriCore();
        if (request.hold)
        {
            CHECK(!view_gate);
            // Every earlier use is complete. Only work placed behind this native gate can delay row reuse.
            CHECK(core.QueueWaitIdle(&queue.native()) == nri::Result::SUCCESS);
            CHECK(queue.poll());
            view_gate.emplace(session);
            const nri::FenceSubmitDesc wait{view_gate->fence, 1, nri::StageBits::ALL};
            nri::QueueSubmitDesc hold{};
            hold.waitFences = &wait;
            hold.waitFenceNum = 1;
            CHECK(core.QueueSubmit(queue.native(), hold) == nri::Result::SUCCESS);
        }
        else
        {
            CHECK(view_gate);
            view_gate->signal();
            CHECK(core.QueueWaitIdle(&queue.native()) == nri::Result::SUCCESS);
            CHECK(queue.poll());
            view_gate.reset();
        }
        CHECK(owner.uploads);
        reply = {{}, owner.uploads->statistics().copied_bytes};
    }

    explicit DeviceRetirementProbe(ResourceCreateContext& context) noexcept
        : session(context.device), uploads(context.uploads), index(created++)
    {
        CHECK(index < adapters.size());
        adapters[index] = session.description().adapterDesc;
        present_retirement[index] = session.supportsPresentRetirement();
        if (index != 1) return;
        fence.emplace(session);
        gate = &*fence;
        auto& core = const_cast<nri::CoreInterface&>(session.nriCore());
        submit = core.QueueSubmit;
        wait_idle = core.QueueWaitIdle;
        core.QueueWaitIdle = [](nri::Queue* queue) noexcept
        {
            if (hold_queue_idle.load())
            {
                queue_idle_entered.store(true);
                hold_queue_idle.wait(true);
            }
            return wait_idle(queue);
        };
        core.QueueSubmit = [](nri::Queue& queue, const nri::QueueSubmitDesc& input) noexcept
        {
            if (!arm.exchange(false))
            {
                const auto result = submit(queue, input);
                if (accepted_ns && result == nri::Result::SUCCESS && input.commandBufferNum)
                {
                    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch());
                    accepted_ns->store(now.count(), std::memory_order_release);
                }
                return result;
            }
            CHECK(input.waitFenceNum == 0);
            const nri::FenceSubmitDesc wait{gate->fence, 1, nri::StageBits::ALL};
            auto waiting = input;
            waiting.waitFences = &wait;
            waiting.waitFenceNum = 1;
            const auto result = submit(queue, waiting);
            held.store(result == nri::Result::SUCCESS);
            return result;
        };
    }

    ~DeviceRetirementProbe() noexcept
    {
        if (fence)
        {
            const_cast<nri::CoreInterface&>(fence->core).QueueSubmit = submit;
            const_cast<nri::CoreInterface&>(fence->core).QueueWaitIdle = wait_idle;
            gate = nullptr;
            fence.reset();
        }
        destroyed[index].store(true);
    }

    static void signal(const EmptyRequest&, AckResponse& response) noexcept
    {
        CHECK(gate && held.exchange(false));
        gate->signal();
        response = {};
    }

    static RenderResult<std::unique_ptr<DeviceRetirementProbe>>
    create(ResourceCreateContext& context, ResourceInstanceId) noexcept
    {
        return std::make_unique<DeviceRetirementProbe>(context);
    }

    static ResourceRegistration factory() noexcept
    {
        return makeResourceRegistration<Descriptor, DeviceRetirementProbe, &create>();
    }
};

namespace
{
    std::int64_t clockNs() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    }

    template <class Command>
    std::uint64_t sendTimed(ipc::IpcRequester& client, CommandId command, typename Command::Request request)
    {
        ipc::IpcMessage message;
        message.emplace<typename Command::Request>(std::move(request));
        const auto correlation = nextSceneRequest();
        CHECK(client.trySend(commandTag(command), correlation, message));
        return correlation;
    }

    struct PairedRequestTimes final
    {
        std::int64_t operation_send, peer_send, operation_reply = 0, peer_reply = 0;
    };

    struct FairnessReplay final
    {
        ipc::IpcRequester& client;
        const CommandList& commands;
        RenderJobId peer;
        const std::atomic<std::int64_t>& peer_accepted;
        CommandId frame_command;

        template<class Command>
        void measure(CommandId command, typename Command::Request request, unsigned round, const char* phase)
        {
            renderFrame(client, commands, peer);
            const auto previous_accept = peer_accepted.load();
            PairedRequestTimes timestamps{clockNs(), 0};
            const auto operation = sendTimed<Command>(client, command, std::move(request));
            timestamps.peer_send = clockNs();
            const auto frame = sendTimed<RequestRenderJob>(client, frame_command, {peer});
            auto& notifications = ipc::IpcCore::instance();
            const auto deadline = Clock::now() + 10s;
            while (!timestamps.operation_reply || !timestamps.peer_reply)
            {
                const auto epoch = notifications.workEpoch();
                ipc::IpcMessage message;
                std::uint64_t id = 0;
                const auto status = client.tryReceive(id, message);
                CHECK(status != ipc::EIpcReceive::CLOSED && Clock::now() < deadline);
                if (status != ipc::EIpcReceive::RECEIVED)
                {
                    notifications.waitUntil(epoch, deadline);
                    continue;
                }
                const auto received = clockNs();
                if (id == operation)
                {
                    CHECK(!timestamps.operation_reply && !message.get<AckResponse>().error.type);
                    timestamps.operation_reply = received;
                }
                else
                {
                    const auto& reply = message.get<JobSubmitted>();
                    CHECK(id == frame && !timestamps.peer_reply && !reply.error.type && reply.job == peer);
                    timestamps.peer_reply = received;
                }
            }
            const auto accepted = peer_accepted.load();
            CHECK(previous_accept && previous_accept < timestamps.operation_send && timestamps.peer_send <= accepted);
            CHECK(accepted <= timestamps.peer_reply);
            std::printf("PAIR,%u,%s,%lld,%lld,%lld,%lld,%lld,%lld\n", round, phase,
                        previous_accept, timestamps.operation_send, timestamps.peer_send, accepted,
                        timestamps.peer_reply, timestamps.operation_reply);
        }
    };
}

static int startFairness(bool d3d12, bool reverse_adapters)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    RenderServerConfig config{"lux.render.test.start_fairness", nativeValidationEnabled()};
    config.frames_in_flight = 1;
    auto server = take(RenderServer::create(config, lux::execution::ExecutionRuntime::instance()));
    registerOutputNodes(*server);
    take(server->registerResource<DeviceRetirementProbe>());
    auto requester = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(requester);
    auto& client = *requester;
    std::atomic<std::int64_t> accepted{0};
    DeviceRetirementProbe::accepted_ns = &accepted;
    std::thread render([&] { CHECK(server->run()); });
    lux::cxx::scope_exit stop([&]() noexcept
    {
        server->stop();
        render.join();
        DeviceRetirementProbe::accepted_ns = nullptr;
    });
    const auto commands = queryCommands(client);
    DeviceOptions options;
    options.backend = d3d12 ? EGraphicsBackend::D3D12 : EGraphicsBackend::VULKAN;
    options.physical_device_index = reverse_adapters ? 1 : 0;
    const auto a = call<CreateDeviceSession>(client, commands, options).device;
    options.physical_device_index = reverse_adapters ? 0 : 1;
    const auto second = call<CreateDeviceSession>(client, commands, options, false);
    if (second.error.type) return 77;
    const auto b = second.device;
    const auto type = DeviceRetirementProbe::Descriptor::descriptor.type;
    for (const auto device : {a, b}) call<GetOrCreateResource>(client, commands, {device, DeviceScope{}, type});
    const auto& adapters = DeviceRetirementProbe::adapters;
    if (adapters[0].uid.high == adapters[1].uid.high && adapters[0].uid.low == adapters[1].uid.low) return 77;
    std::printf("ADAPTERS,%s,%s,%llx/%llx,%s,%llx/%llx\n", d3d12 ? "d3d12" : "vulkan", adapters[0].name,
                adapters[0].uid.high, adapters[0].uid.low, adapters[1].name, adapters[1].uid.high, adapters[1].uid.low);
    const auto scene = call<CreateScene>(client, commands, {}).scene;
    const auto view = call<CreateView>(client, commands, {scene}).view;
    const TargetDesc desc{96, 64, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    const auto output_a = call<CreateTarget>(client, commands, {a, desc}).target;
    const auto reuse_a = call<CreateTarget>(client, commands, {a, desc}).target;
    const auto output_b = call<CreateTarget>(client, commands, {b, desc}).target;
    const auto effect_b = createPreparedEffect(client, commands, b, clearOutputRequest());
    const auto peer = startRenderJob(client, commands, b, scene, view, output_b, effect_b);
    const auto command = [&](std::string_view name)
    {
        const auto found = std::ranges::find(commands.commands, name, &CommandInfo::name);
        CHECK(found != commands.commands.end());
        return found->id;
    };
    const auto prepare = command(PrepareEffect::name);
    const auto start = command(StartRenderJob::name);
    FairnessReplay replay{client, commands, peer, accepted, command(RequestRenderJob::name)};
    for (unsigned round = 0; round < 108; ++round)
    {
        const auto effect = call<CreateEffect>(client, commands, clearOutputRequest()).effect;
        const CreateRenderJob::Request recipe{scene, view, effect, EJobCadence::REQUESTED};
        const auto first = call<CreateRenderJob>(client, commands, recipe).job;
        const auto second_job = call<CreateRenderJob>(client, commands, recipe).job;
        const auto before = server->stats().compiled_graphs;
        replay.measure<PrepareEffect>(prepare, {effect, a}, round, "prepare");
        CHECK(server->stats().compiled_graphs == before + 1);
        replay.measure<StartRenderJob>(start, {a, first, output_a}, round, "cold");
        CHECK(server->stats().compiled_graphs == before + 1);
        // Both measurements start synchronously against the same prepared graph.
        replay.measure<StartRenderJob>(start, {a, second_job, reuse_a}, round, "reuse");
        CHECK(server->stats().compiled_graphs == before + 1);
        stopAndRemoveJob(client, commands, second_job);
        stopAndRemoveJob(client, commands, first);
        call<RemoveEffect>(client, commands, {effect});
    }
    stopAndRemoveJob(client, commands, peer);
    CHECK(server->stats().validation_errors == 0);
    return 0;
}

static void verifyViewRowRetirement(
    ipc::IpcRequester& client, const CommandList& commands, SceneId scene, std::array<DeviceSessionId, 2> devices
)
{
    const auto observed = call<CreateView>(client, commands, {scene}).view;
    const auto idle = call<CreateView>(client, commands, {scene}).view;
    ViewGpuParameters parameters;
    parameters.background = {1, 0, 0, 1};
    call<UpdateView>(client, commands, {{scene, observed}, parameters});
    CreateEffectRequest request;
    request.graph = std::make_unique<EffectGraph>();
    EffectGraphEdit edit{*request.graph};
    const auto draw = take(edit.addNode(outputSource(false), {"test.output.view", 1}));
    const auto capture = take(edit.addNode(ColorCapture::definition(), {std::string{ColorCapture::name}, 1}));
    const auto color = edit.pinId(draw, ToneMappingPorts::color.semantic);
    CHECK(edit.connect(color, edit.pinId(capture, ColorCapture::color)));
    CHECK(edit.setOutput("color", color, true));
    CHECK(edit.setOutput("pixels", edit.pinId(capture, ColorCapture::pixels)));
    CHECK(edit.commit());
    const auto effect = createPreparedEffect(client, commands, devices[0], std::move(request));
    call<PrepareEffect>(client, commands, {effect, devices[1]});
    std::array<ResourceInstanceId, 2> probes;
    std::array<CapturedPixels, 2> pixels, idle_pixels;
    std::array<JobSubmitted, 2> receipts;
    std::array<std::uint64_t, 2> uploads;
    std::array<RenderJobId, 2> jobs;
    std::array<TargetId, 2> targets;
    for (unsigned i = 0; i < devices.size(); ++i)
    {
        const auto device = devices[i];
        probes[i] = call<QueryResource>(client, commands, {
            device, DeviceScope{}, DeviceRetirementProbe::Descriptor::descriptor.type
        }).resource_id;
        const TargetDesc target{64, 64, rdesc::ETextureFormat::RGBA8_UNORM};
        targets[i] = call<CreateTarget>(client, commands, {device, target}).target;
        const auto start = [&](ViewId view, CapturedPixels& capture_pixels)
        {
            const ViewScope scope{scene, view};
            const auto camera = call<GetOrCreateResource>(client, commands, {
                device, scope, kViewParameterResourceName
            }).resource_id;
            const auto output = GraphCaptureBuffer::bind({client, commands, device}, scope, {&capture_pixels});
            JobBinding binding;
            binding.resources = {{std::string{ViewParameterResource::gpu_binding}, camera},
                                 {std::string{ColorCapture::name}, output}};
            return startRenderJob(client, commands, device, scene, view, targets[i], effect, std::move(binding));
        };
        const auto finished = start(idle, idle_pixels[i]);
        renderFrame(client, commands, finished);
        stopAndRemoveJob(client, commands, finished);
        jobs[i] = start(observed, pixels[i]);
        renderFrame(client, commands, jobs[i]);
    }
    // Update once after both replicas exist. Both devices must sample the newly selected row.
    parameters.background = {0, 1, 0, 1};
    call<UpdateView>(client, commands, {{scene, observed}, parameters});
    // B has the current revision in every FIF. Its held graph read must protect the row without a new upload.
    for (unsigned i = 0; i < 3; ++i) renderFrame(client, commands, jobs[1]);
    for (unsigned i = 0; i < devices.size(); ++i)
    {
        uploads[i] = call<SetViewQueueGate>(client, commands, {probes[i], true}).copied_bytes;
        receipts[i] = call<RequestRenderJob>(client, commands, {jobs[i]});
        stopAndRemoveJob(client, commands, jobs[i]);
    }
    call<RemoveView>(client, commands, {scene, observed});
    CHECK(call<UpdateView>(client, commands, {{scene, observed}, {}}, false).error.type == kRuntimeIdentity);
    call<RemoveView>(client, commands, {scene, idle});
    const auto both_held = call<CreateView>(client, commands, {scene}).view;
    CHECK(both_held.index == idle.index && both_held.gen != idle.gen);
    const auto released_a = call<SetViewQueueGate>(client, commands, {probes[0], false});
    CHECK(released_a.copied_bytes - uploads[0] == sizeof(ViewGpuParameters));
    const auto one_held = call<CreateView>(client, commands, {scene}).view;
    CHECK(one_held.index != observed.index);
    const auto released_b = call<SetViewQueueGate>(client, commands, {probes[1], false});
    CHECK(released_b.copied_bytes == uploads[1]);
    const auto recycled = call<CreateView>(client, commands, {scene}).view;
    CHECK(recycled.index == observed.index && recycled.gen != observed.gen);
    CHECK(call<RemoveView>(client, commands, {scene, observed}, false).error.type == kRuntimeIdentity);
    const auto deadline = Clock::now() + 10s;
    for (unsigned i = 0; i < devices.size(); ++i)
    {
        while (!pixels[i].ready.load() || !idle_pixels[i].ready.load()) CHECK(Clock::now() < deadline);
        const auto& bytes = pixels[i].frames.at(receipts[i].slot_index);
        CHECK(bytes[0] == std::byte{0} && bytes[1] == std::byte{255} && bytes[2] == std::byte{0});
    }
    for (auto id : {both_held, one_held, recycled}) call<RemoveView>(client, commands, {scene, id});
    for (auto target : targets) call<RemoveTarget>(client, commands, target);
    call<RemoveEffect>(client, commands, {effect});
    std::printf("VIEW ROW RETIREMENT PASS: old=%u/%u idle=%u/%u reused=%u/%u "
                "held_uploads=%llu,%llu green_readbacks=2\n",
        observed.index, observed.gen, idle.index, idle.gen, recycled.index, recycled.gen,
        static_cast<unsigned long long>(released_a.copied_bytes - uploads[0]),
        static_cast<unsigned long long>(released_b.copied_bytes - uploads[1]));
}

static bool physicalDeviceLifecycle(bool d3d12)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto backend = d3d12 ? "D3D12" : "Vulkan";
    RenderServerConfig config{"lux.render.test.device_lifecycle", nativeValidationEnabled()};
    config.uploads.emplace();
    config.diagnostics.report = [](void*, const RenderDiagnostic& diagnostic) noexcept
    {
        if (diagnostic.severity == ERenderDiagnosticSeverity::ERROR_MESSAGE)
            std::fprintf(stderr, "Device lifecycle diagnostic: %.*s\n",
                int(diagnostic.message.size()), diagnostic.message.data());
    };
    auto server = take(RenderServer::create(config, lux::execution::ExecutionRuntime::instance()));
    registerOutputNodes(*server);
    const std::array view_gate_commands{CommandBinding::declare<SetViewQueueGate, &DeviceRetirementProbe::setViewGate>()};
    take(server->registerResource<DeviceRetirementProbe>(view_gate_commands));
    const std::array gate_commands{CommandBinding::declare<SignalRetirementFence, &DeviceRetirementProbe::signal>()};
    CHECK(server->registerCommands(gate_commands));
    auto requester = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(requester);
    auto& client = *requester;
    std::thread render([&] { CHECK(server->run()); });
    const auto commands = queryCommands(client);
    DeviceOptions options;
    options.backend = d3d12 ? EGraphicsBackend::D3D12 : EGraphicsBackend::VULKAN;
    options.texture_descriptors = true;
    options.physical_device_index = 0;
    const auto a = call<CreateDeviceSession>(client, commands, options).device;
    options.physical_device_index = 1;
    const auto second = call<CreateDeviceSession>(client, commands, options, false);
    if (second.error.type)
    {
        std::printf("PHYSICAL LIFECYCLE UNKNOWN backend=%s adapter 1 error=%llu arg=%llu\n",
            backend, second.error.type, second.error.args[0]);
        server->stop();
        render.join();
        return false;
    }
    const auto b = second.device;
    const auto probe_type = DeviceRetirementProbe::Descriptor::descriptor.type;
    const auto probe_a = call<GetOrCreateResource>(client, commands, {a, DeviceScope{}, probe_type}).resource_id;
    const auto probe_b = call<GetOrCreateResource>(client, commands, {b, DeviceScope{}, probe_type}).resource_id;
    const auto& adapters = DeviceRetirementProbe::adapters;
    const bool distinct = adapters[0].uid.low != adapters[1].uid.low || adapters[0].uid.high != adapters[1].uid.high;
    if (!distinct)
    {
        std::printf("PHYSICAL LIFECYCLE UNKNOWN backend=%s duplicate adapter UID\n", backend);
        server->stop();
        render.join();
        return false;
    }
    std::printf("DEVICE LIFECYCLE ADAPTERS backend=%s A=%s uid=%llx/%llx B=%s uid=%llx/%llx\n",
        backend, adapters[0].name, adapters[0].uid.high, adapters[0].uid.low,
        adapters[1].name, adapters[1].uid.high, adapters[1].uid.low);

    const auto scene = call<CreateScene>(client, commands, {}).scene;
    const auto view = call<CreateView>(client, commands, {scene}).view;
    verifyViewRowRetirement(client, commands, scene, {a, b});
    verifyTargetInput(client, commands, a, scene, b);
    verifyTargetInput(client, commands, b, scene, a);

    std::promise<std::array<HWND, 2>> opened;
    auto ready = opened.get_future();
    std::thread ui([&]
    {
        std::array windows{createProbeWindow(), createProbeWindow()};
        CHECK(windows[0] && windows[1]);
        CHECK(SetWindowPos(windows[0], nullptr, 40, 40, 320, 240, SWP_NOZORDER));
        CHECK(SetWindowPos(windows[1], nullptr, 420, 40, 320, 240, SWP_NOZORDER));
        for (auto window : windows) ShowWindow(window, SW_SHOW);
        opened.set_value(windows);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    });
    const auto windows = ready.get();
    const auto surface_desc = [](HWND window)
    {
        RECT client;
        CHECK(GetClientRect(window, &client));
        return TargetDesc{std::uint32_t(client.right), std::uint32_t(client.bottom),
            rdesc::ETextureFormat::BGRA8_UNORM, 1, ETargetKind::SURFACE};
    };
    const auto surface_a = call<CreateTarget>(
        client, commands, {a, surface_desc(windows[0]), SurfaceDescription{Win32Surface{windows[0]}}}
    );
    const auto surface_b = call<CreateTarget>(
        client, commands, {b, surface_desc(windows[1]), SurfaceDescription{Win32Surface{windows[1]}}}
    );
    CHECK(surface_a.output_available && surface_b.output_available);
    if (!d3d12)
        std::printf("VULKAN WSI RETIREMENT A=%s B=%s\n",
            DeviceRetirementProbe::present_retirement[0] ? "present-fence" : "queue-idle",
            DeviceRetirementProbe::present_retirement[1] ? "present-fence" : "queue-idle");
    const auto prepare_surface = [&](const TargetCreated& target, DeviceSessionId device)
    {
        auto request = clearOutputRequest();
        request.output = target.description.outputContract();
        return createPreparedEffect(client, commands, device, std::move(request));
    };
    const auto surface_effect = prepare_surface(surface_a, a);
    const auto compatible = surface_a.description.outputContract() == surface_b.description.outputContract();
    const auto peer_effect = compatible ? surface_effect : prepare_surface(surface_b, b);
    if (compatible) call<PrepareEffect>(client, commands, {peer_effect, b});
    const auto job_a = startRenderJob(client, commands, a, scene, view, surface_a.target, surface_effect);
    const auto present = [&](RenderJobId job)
    {
        const auto before = server->stats().presentations;
        renderFrame(client, commands, job);
        const auto deadline = Clock::now() + 10s;
        while (server->stats().presentations == before)
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::sleep_for(1ms);
        }
        CHECK(server->stats().presentations == before + 1);
    };
    const auto resized = [&](const TargetCreated& target, TargetDesc description, const TargetCreated& result)
    {
        CHECK(!result.error.type && result.output_available);
        CHECK(result.description == description && result.epoch == target.epoch + 1);
        call<SetTargetActive>(client, commands, {target.target, true, result.epoch});
    };
    const auto resize = [&](const TargetCreated& target, TargetDesc description)
    {
        resized(target, description, call<ResizeTarget>(client, commands, {target.target, description}));
    };
    const auto new_window_extent = [&](const TargetCreated& target, HWND window)
    {
        auto description = target.description;
        const auto extent = surface_desc(window);
        description.width = extent.width;
        description.height = extent.height;
        return description;
    };
    present(job_a);
    CHECK(SetWindowPos(windows[0], nullptr, 0, 0, 360, 260, SWP_NOMOVE | SWP_NOZORDER));
    resize(surface_a, new_window_extent(surface_a, windows[0]));
    const auto job_b = startRenderJob(client, commands, b, scene, view, surface_b.target, peer_effect);
    present(job_b);
    CHECK(call<RemoveDeviceSession>(client, commands, b, false).error.type == kDeviceBusy);
    CHECK(SetWindowPos(windows[1], nullptr, 0, 0, 370, 270, SWP_NOMOVE | SWP_NOZORDER));
    const auto new_extent = new_window_extent(surface_b, windows[1]);
    const bool can_hold_native_wait = d3d12 || !DeviceRetirementProbe::present_retirement[1];
    if (can_hold_native_wait)
    {
        auto resizing = ipc::IpcCore::instance().createRequester(config.channel_name);
        CHECK(resizing);
        DeviceRetirementProbe::hold_queue_idle.store(true);
        auto pending = std::async(std::launch::async, [&]
        {
            return call<ResizeTarget>(*resizing, commands, {surface_b.target, new_extent});
        });
        const auto deadline = Clock::now() + 10s;
        while (!DeviceRetirementProbe::queue_idle_entered.load())
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::sleep_for(1ms);
        }
        present(job_a);
        CHECK(pending.wait_for(0s) == std::future_status::timeout);
        DeviceRetirementProbe::hold_queue_idle.store(false);
        DeviceRetirementProbe::hold_queue_idle.notify_all();
        resized(surface_b, new_extent, pending.get());
        std::puts("WSI QUEUE ISOLATION PASS: peer presented while the resizing device held its own queue wait");
    }
    else
    {
        resize(surface_b, new_extent);
        present(job_a);
    }
    present(job_b);
    stopAndRemoveJob(client, commands, job_b);
    call<RemoveTarget>(client, commands, surface_b.target);

    const TargetDesc offscreen{32, 32, rdesc::ETextureFormat::RGBA8_UNORM};
    const auto off_a = call<CreateTarget>(client, commands, {a, offscreen});
    const auto off_b = call<CreateTarget>(client, commands, {b, offscreen});
    const auto off_effect = createPreparedEffect(client, commands, a, clearOutputRequest());
    call<PrepareEffect>(client, commands, {off_effect, b});
    const auto off_job_a = startRenderJob(client, commands, a, scene, view, off_a.target, off_effect);
    const auto off_job_b = startRenderJob(client, commands, b, scene, view, off_b.target, off_effect);
    CHECK(call<RemoveDeviceSession>(client, commands, b, false).error.type == kDeviceBusy);
    auto larger_offscreen = offscreen;
    larger_offscreen.width = 48;
    resize(off_a, larger_offscreen);
    resize(off_b, larger_offscreen);
    renderFrame(client, commands, off_job_a);
    DeviceRetirementProbe::arm.store(true);
    renderFrame(client, commands, off_job_b);
    CHECK(DeviceRetirementProbe::held.load());
    call<StopRenderJob>(client, commands, off_job_b);
    CHECK(call<RequestRenderJob>(client, commands, {off_job_b}, false).error.type == kJobStopped);
    stopAndRemoveJob(client, commands, off_job_b);
    CHECK(call<RequestRenderJob>(client, commands, {off_job_b}, false).error.type == kJobIdentity);
    call<RemoveTarget>(client, commands, off_b.target);

    auto deleting = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(deleting);
    auto removal = std::async(std::launch::async, [&]
    {
        return call<RemoveDeviceSession>(*deleting, commands, b, false);
    });
    const auto deadline = Clock::now() + 10s;
    while (!call<GetOrCreateResource>(client, commands, {b, DeviceScope{}, probe_type}, false).error.type)
    {
        CHECK(Clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
    CHECK(call<QueryResource>(client, commands, {b, DeviceScope{}, probe_type}, false).error.type == kDeviceBusy);
    CHECK(call<RemoveDeviceSession>(client, commands, b, false).error.type == kDeviceBusy);
    CHECK(call<CreateTarget>(client, commands, {b, offscreen}, false).error.type == kDeviceBusy);
    CHECK(removal.wait_for(0s) == std::future_status::timeout && !DeviceRetirementProbe::destroyed[1].load());
    present(job_a);
    renderFrame(client, commands, off_job_a);
    CHECK(DeviceRetirementProbe::held.load());
    call<SignalRetirementFence>(client, commands, {});
    CHECK(!removal.get().error.type && DeviceRetirementProbe::destroyed[1].load());
    CHECK(call<QueryRenderDevice>(client, commands, b, false).error.type == kRuntimeIdentity);
    CHECK(call<QueryResource>(client, commands, {a, DeviceScope{}, probe_type}).resource_id == probe_a);
    CHECK(probe_b != probe_a && !DeviceRetirementProbe::destroyed[0].load());
    present(job_a);

    const auto replacement = call<CreateDeviceSession>(client, commands, options).device;
    CHECK(replacement != b && replacement.index == b.index);
    call<GetOrCreateResource>(client, commands, {replacement, DeviceScope{}, probe_type});
    CHECK(adapters[2].uid.low == adapters[1].uid.low && adapters[2].uid.high == adapters[1].uid.high);
    const auto reopen_b = [&]
    {
        return call<CreateTarget>(
            client, commands, {replacement, surface_desc(windows[1]), SurfaceDescription{Win32Surface{windows[1]}}}
        );
    };
    const auto unused_target = reopen_b();
    const auto same_output = unused_target.description.outputContract() == surface_b.description.outputContract();
    const auto reopened_effect = same_output ? peer_effect : prepare_surface(unused_target, replacement);
    if (same_output) call<PrepareEffect>(client, commands, {reopened_effect, replacement});
    const auto unused_job = startRenderJob(
        client, commands, replacement, scene, view, unused_target.target, reopened_effect
    );
    CHECK(call<RequestRenderJob>(client, commands, {unused_job, FrameData{}}, false).error.type == kFrameContract);
    call<StopRenderJob>(client, commands, unused_job);
    call<RemoveTarget>(client, commands, unused_target.target);
    call<RemoveRenderJob>(client, commands, unused_job);
    std::puts("WSI ACQUIRE PASS: removed and reopened the window after an unconsumed Acquire");

    const auto recreated = reopen_b();
    const auto final_job = startRenderJob(client, commands, replacement, scene, view, recreated.target, reopened_effect);
    present(final_job);
    call<Stop>(client, commands, {});
    render.join();
    CHECK(std::ranges::all_of(DeviceRetirementProbe::destroyed, [](const auto& value) { return value.load(); }));
    const auto presents = server->stats().presentations;
    CHECK(server->stats().validation_errors == 0 && presents == 7);
    CHECK(PostMessageW(windows[0], WM_CLOSE, 0, 0));
    ui.join();
    std::printf("DEVICE LIFECYCLE PASS backend=%s presents=%llu dual_wsi=1 Offscreen resize/Stop/Shutdown\n",
        backend, presents);
    std::puts("DEVICE RETIREMENT PASS: peer submitted while GPU fence held, removal revoked only B, all borrows settled");
    return true;
}
#endif

int main(int argc, char** argv)
{
    auto execution = lux::execution::ExecutionRuntime::create({2, 128, {128}});
    CHECK(execution);
#ifdef _WIN32
    if (argc > 1 && std::string_view{argv[1]} == "--transform-retirement")
        return transformRetirementBacklog(argc > 2 && std::string_view{argv[2]} == "d3d12"
            ? EGraphicsBackend::D3D12 : EGraphicsBackend::VULKAN);
    if (argc > 1 && std::string_view{argv[1]} == "--upload-acquire-failure")
        return sharedRasterTextures(argc > 2 && std::string_view{argv[2]} == "d3d12", false, true);
#endif
    if (argc > 1 && std::string_view{argv[1]} == "--shared-texture")
        return sharedRasterTextures(argc > 2 && std::string_view{argv[2]} == "d3d12");
    if (argc > 1 && std::string_view{argv[1]} == "--material-stream")
        return sharedRasterTextures(argc > 2 && std::string_view{argv[2]} == "d3d12", true);
    if (argc > 1 && std::string_view{argv[1]} == "--mesh-stream")
    {
        meshRasterStream(argc > 2 && std::string_view{argv[2]} == "d3d12");
        return 0;
    }
    if (argc > 1 && std::string_view{argv[1]} == "--effect-graph")
    {
        const auto mode = argc > 3 ? std::string_view{argv[3]} : std::string_view{};
        if (!mode.empty() && mode != "uniform-faults") return 2;
        const bool async_uniform = mode == "uniform-faults";
        effectGraphOutput(argc > 2 && std::string_view{argv[2]} == "d3d12", async_uniform);
        if (async_uniform && !UniformUploadProbe::hasSeparateComputeQueue())
        {
            std::puts("No independent compute queue on this adapter");
            return 77;
        }
        return 0;
    }
    if (argc > 1 && std::string_view{argv[1]} == "--basic-features")
    {
        basicFeatureOutputs(argc > 2 && std::string_view{argv[2]} == "d3d12");
        return 0;
    }
    if (argc > 1 && std::string_view{argv[1]} == "--prepared-buffer")
    {
        preparedBuffers(argc > 2 && std::string_view{argv[2]} == "d3d12");
        return 0;
    }
#ifdef _WIN32
    if (argc > 1 && std::string_view{argv[1]} == "--wsi")
    {
        windowOutput(argc > 2 && std::string_view{argv[2]} == "d3d12");
        return 0;
    }
    if (argc > 1 && std::string_view{argv[1]} == "--dual-device-lifecycle")
        return physicalDeviceLifecycle(argc > 2 && std::string_view{argv[2]} == "d3d12") ? 0 : 77;
    if (argc >= 3 && std::string_view{argv[1]} == "--start-fairness")
    {
        const std::string_view api{argv[2]};
        const bool reverse = argc == 4 && std::string_view{argv[3]} == "reverse";
        if ((argc != 3 && !reverse) || (api != "vulkan" && api != "d3d12")) return 2;
        return startFairness(api == "d3d12", reverse);
    }
#endif
    Validation validation;
    if (argc > 1 && std::string_view{argv[1]} == "--device-lost")
    {
        deviceLostDelivery(validation, false);
        CHECK(validation.errors == 0);
        return 0;
    }
    if (argc != 1) return 2;
    graphOutput(validation);
    importedExtentContracts(validation);
    importedIdentityReuse(validation, EGraphicsBackend::VULKAN);
#ifdef _WIN32
    importedIdentityReuse(validation, EGraphicsBackend::D3D12);
#endif
    deviceLostDelivery(validation);
    CHECK(validation.errors == 0);
}
