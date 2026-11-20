#include "NativeDevice.hpp"
#include "SceneCapture.hpp"
#include "EffectGraphColor.hpp"
#include <lux/engine/render/capabilities/ColorEncoding.hpp>
#include <lux/engine/render/effect/EffectBundles.hpp>
#include <lux/engine/render/effect/DepthPyramid.hpp>
#include <lux/engine/render/effect/GBuffer.hpp>
#include <lux/engine/render/effect/Lighting.hpp>
#include <lux/engine/render/effect/MeshVisibility.hpp>
#include <lux/engine/render/effect/ToneMapping.hpp>
#include "EffectGraphImport.hpp"
#include "EffectGraphProducer.hpp"
#include "EffectMeshImports.hpp"
#include "EffectMeshImportsPaged.hpp"
#include "EffectNodeUniform.hpp"
#include "EffectPreparedBuffer.hpp"
#include "EffectStreamIndirect.hpp"
#include "EffectStreamInline.hpp"
#include "EffectStreamLayers.hpp"
#include "EffectStreamMaterial.hpp"
#include "EffectStreamMesh.hpp"
#include "EffectStreamSharedTexture.hpp"
#include "EffectStreamUniform.hpp"
#include "EffectStreamVertex.hpp"
#include <lux/engine/render/features/shadow/ShadowInputs.hpp>
#include <lux/engine/render/frame/FrameData.hpp>
#include <lux/engine/render/resources/lighting/Resource.hpp>
#include <lux/engine/render/resources/instance/Resource.hpp>
#include <lux/engine/render/resources/material/Resource.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/render/resources/mesh/Resource.hpp>
#include <lux/engine/render/resources/texture/Resource.hpp>
#include <lux/engine/render/resources/transform/Binding.hpp>
#include <lux/engine/render/resources/transform/Resource.hpp>
#include <lux/engine/render/resources/view/Resource.hpp>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <Extensions/NRIWrapperD3D12.h>
#include <Extensions/NRIWrapperVK.h>
#include <vulkan/vulkan.h>
#undef interface
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
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <lux/engine/render/graph/execute/ResizeResources.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include "FrameHarness.hpp"
#include <lux/engine/render/extensions/effect/EffectBuildContext.hpp>
#include <lux/engine/render/extensions/effect/EffectImplementation.hpp>
#include <lux/engine/render/extensions/effect/EffectInitializationContext.hpp>
#include <lux/engine/render/extensions/graph/RenderGraphBuilder.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/graph/compile/GraphCompiler.hpp>
#include <optional>
#include <string>
#include <string_view>
using namespace foundation_test;

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
    lux::render::GraphCpuDataId output;
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
    RenderGraphBuilder builder;
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
    const EffectOutputDesc output_description{target.format, target.samples, target.kind, target.sampled};
    auto ready_result = stdexec::sync_wait(GraphCompiler::compile(
        *session, lux::execution::ExecutionRuntime::instance().cpuScheduler(), definition, analysis, &output_description
    ));
    CHECK(ready_result);
    auto graph = take(std::move(std::get<0>(*ready_result)));
    auto physical = std::optional{take(OffscreenTarget::create(*session, target, frames->frameCount()))};
    auto execution = std::make_unique<RenderGraphInstance>(
        *session, frames->frameCount(), graph.executable, graph.programs, physical->output()
    );
    CHECK(execution->initialize());
    auto incompatible_output = physical->output();
    incompatible_output.description.format = lux::rdesc::ETextureFormat::BGRA8_UNORM;
    auto rejected_resize = execution->prepareResize(incompatible_output, {});
    CHECK(!rejected_resize && rejected_resize.error().type == kDeviceArgument);
    auto missing_output = execution->prepareResize({});
    CHECK(!missing_output && missing_output.error().type == kDeviceArgument);
    CHECK(execution->target() == target); // Rejection must not withdraw the adopted output.
    const std::array contexts{execution.get()};
    std::vector<StorageFrameBinding> storage_preparation;
    nri::Buffer* output = nullptr;
    const nri::BufferDesc description{4096, 0, nri::BufferUsageBits::NONE};
    CHECK(
        session->nriCore().CreatePlacedBuffer(session->device(), NriHostReadbackHeap, description, output) ==
        nri::Result::SUCCESS
    );
    CHECK(take(runTask(frames->render(contexts, storage_preparation))) == 1);
    CHECK(take(runTask(frames->render(contexts, storage_preparation))) == 1);
    CHECK(take(runTask(frames->render(contexts, storage_preparation))) == 1);
    CHECK(take(runTask(frames->render(contexts, storage_preparation))) == 0);
    CHECK(session->drain());
    for (auto id : {pixels, copied})
    {
        copyBuffer(
            *session,
            *execution->nativeBuffer(id.index, execution->progress().last_submitted_slot),
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
    CHECK(take(runTask(frames->render(contexts, storage_preparation))) == 1);
    auto last_uses = session->makeFrontiers();
    physical->appendUses(last_uses);
    CHECK(last_uses != prior_uses);
    execution.reset(); // Physical resources outlive the business Context through Session cleanup.
    physical.reset();
    CHECK(session->drain());
    session->nriCore().DestroyBuffer(output);
    std::puts("GPU RETIREMENT PASS: readback pixels and Offscreen release use accepted frontiers");
}

static void importedIdentityReuse(Validation& validation)
{
    TestDevice session({}, diagnostics(validation), nativeValidationEnabled());
    auto frames = std::make_unique<FrameHarness>(*session, 1);
    RenderGraphBuilder builder;
    const auto color = take(builder.declareResource({"color", GraphImageDesc{16, 16}}));
    RasterWork work;
    work.program = RasterPassTemplate{"identity regression"};
    work.colors = {{{{color, 0}}}};
    const auto pass = take(builder.addRasterPass("clear with dynamic reads", std::move(work)));
    CHECK(builder.exportOutput({pass.written.front()}));
    auto graph = take(std::move(builder).finish());
    auto analysis = take(analyzeRenderGraph(graph));
    auto ready_result = stdexec::sync_wait(GraphCompiler::compile(
        *session, lux::execution::ExecutionRuntime::instance().cpuScheduler(), graph, analysis, nullptr
    ));
    CHECK(ready_result);
    auto plan = take(std::move(std::get<0>(*ready_result)));
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
    const auto revision = [&](const RenderGraphInstance& instance)
    {
        dump.clear();
        CHECK(instance.dumpResources(sink));
        return number("\"imported_revision\":");
    };
    const auto rebuilds = [&]()
    {
        dump.clear();
        CHECK(frames->dumpSubmissions(sink));
        return number("\"dependency_rebuilds\":");
    };
    std::optional<RenderGraphInstance> instance;
    instance.emplace(*session, 1, plan.executable, plan.programs);
    CHECK(instance->initialize());
    const auto initial_revision = revision(*instance);
    std::uint64_t creation_revisions = 0;
    {
        RenderGraphInstance probe{*session, 1, plan.executable, plan.programs};
        CHECK(probe.initialize());
        creation_revisions = revision(probe) - initial_revision;
    }
    CHECK(creation_revisions != 0);
    CHECK(session->drain());
    nri::Buffer* buffer = nullptr;
    CHECK(session->nriCore().CreatePlacedBuffer(
        session->device(), NriDeviceHeap, {32, 4, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE}, buffer
    ) == nri::Result::SUCCESS);
    const auto render = [&]()
    {
        const std::array instances{&*instance};
        CHECK(take(runTask(frames->render(instances, {}))) == 1);
        CHECK(!frames->submitting());
        CHECK(session->drain());
    };
    // A viewport can change sampled resources several times, then be detached and recreated
    // at the same allocator address. Local ++ used to collide with the next global identity.
    const auto raster_slot = plan.assembly.source_pass_slots[pass.pass.index].index;
    CHECK(raster_slot < plan.executable.passes().size());
    const auto& raster_pass = plan.executable.passes()[raster_slot];
    for (std::uint64_t update = 0; update < 2 * creation_revisions; ++update)
    {
        auto& draws = instance->rasterDraws(raster_pass, 0);
        draws.reads = {{buffer, GraphBufferRange{16 * (update % 2), 16}, EGraphUse::STORAGE_READ, 1}};
        render();
    }
    const auto cached_revision = revision(*instance);
    const auto cached_rebuilds = rebuilds();
    const auto* address = &*instance;
    instance.reset();
    CHECK(session->drain());
    instance.emplace(*session, 1, plan.executable, plan.programs);
    CHECK(&*instance == address);
    CHECK(instance->initialize());
    render();
    CHECK(rebuilds() == cached_rebuilds + 1);
    CHECK(revision(*instance) > cached_revision);
    const auto adopted_rebuilds = rebuilds();
    render();
    CHECK(rebuilds() == adopted_rebuilds); // Stable inputs still reuse the dependency cache.
    instance.reset();
    CHECK(session->drain());
    session->nriCore().DestroyBuffer(buffer);
    std::puts("IMPORTED IDENTITY PASS: same address rebuilt; unchanged inputs reused dependencies");
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
    session->deferRelease(queue, [&]() noexcept
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
        RenderGraphBuilder builder;
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
        auto ready_result = stdexec::sync_wait(GraphCompiler::compile(
            *session, lux::execution::ExecutionRuntime::instance().cpuScheduler(), graph, analysis, nullptr
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
            RenderGraphInstance invalid{*session, 1, plan.executable, plan.programs};
            invalid.bindImage(source, 0, *images[0]);
            invalid.bindImage(destination, 0, *images[1]);
            const auto rejected = invalid.initialize();
            CHECK(!rejected && rejected.error().type == kInvalidTargetExtent);
        }
        {
            RenderGraphInstance instance{*session, 1, plan.executable, plan.programs};
            instance.bindImage(source, 0, *images[0]);
            instance.bindImage(destination, 0, *images[2]);
            CHECK(instance.initialize());
            const std::array<TargetFrame, 1> smaller_frames{{{images[1]}}};
            const RenderGraphInstance::ImageReplacement replacement{destination, smaller_frames};
            const auto rejected = instance.prepareResize({}, {&replacement, 1});
            CHECK(!rejected && rejected.error().type == kInvalidTargetExtent);
            // A graph with only imported resources legitimately accepts an empty output.
            const std::array<TargetFrame, 1> original{{{images[2]}}};
            const RenderGraphInstance::ImageReplacement valid{destination, original};
            auto unchanged = take(instance.prepareResize({}, {&valid, 1}));
            CHECK(instance.prepareResizeAdoption(unchanged));
            instance.commitResize(std::move(unchanged));
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

    CapabilityDefinition outputSource(bool hdr)
    {
        const auto name = hdr ? hdr_source_name : display_clear_name;
        const auto type = hdr ? HdrColorBundle::type : ToneMapping::display_type;
        return {
            .identity = {graph::nodeTypeId(name), std::string{name}, 1},
            .ports = {{ToneMapping::color, "color", {std::string{type}, 1}, graph::EPinDirection::OUTPUT}}
        };
    }

    template <bool Hdr>
    RenderResult<void> buildOutputClear(EffectBuildContext& context, std::string_view) noexcept
    {
        const auto format = Hdr ? rdesc::ETextureFormat::RGBA16_SFLOAT : context.target().format;
        GraphImageDesc image{0, 0, 1, 1, format};
        image.extent.source = EImageExtentSource::TARGET;
        auto& builder = context.builder();
        const auto color = take(builder.declareResource({"output clear", image}));
        const auto clear = take(builder.addTransferPass(
            "clear output", TransferWork{{ClearImage{{{color, 0}}, {.25f, .5f, .75f, 1}}}}
        ));
        return context.publish(ToneMapping::color, {{ToneMapping::color, clear.written.front()}});
    }

    RenderResult<void> buildOutputImport(EffectBuildContext& context, std::string_view) noexcept
    {
        const auto image = context.importTargetImage(hdr_binding, rdesc::ETextureFormat::RGBA16_SFLOAT);
        if (!image) return lux::cxx::unexpected(image.error());
        return context.publish(ToneMapping::color, {{ToneMapping::color, *image}});
    }

    void registerOutputNodes(RenderServer& server)
    {
        const auto add = [&server](
            const CapabilityDefinition& definition, std::string_view name, graph::PinSemanticId output,
            EffectImplementation::Build build
        )
        {
            EffectImplementation implementation;
            implementation.capability = definition.identity.id;
            implementation.identity = {std::string{name}, 1};
            implementation.supported_outputs = {output};
            implementation.initialize = [](EffectInitializationContext&, std::string_view) noexcept
                -> RenderResult<void> { return {}; };
            implementation.build = build;
            CHECK(server.registerEffectImplementation(std::move(implementation)));
        };
        const auto display = outputSource(false);
        const auto hdr = outputSource(true);
        CHECK(server.registerEffectCapability(display));
        CHECK(server.registerEffectCapability(hdr));
        add(display, display_clear_name, ToneMapping::color, buildOutputClear<false>);
        add(hdr, hdr_source_name, ToneMapping::color, buildOutputClear<true>);
        add(hdr, hdr_import_name, ToneMapping::color, buildOutputImport);
        ColorCapture::registerWith(server);
    }

    CreateEffectRequest clearOutputRequest(bool hdr = false)
    {
        CreateEffectRequest request;
        request.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*request.graph};
        const auto source = outputSource(hdr);
        const auto node = take(edit.addNode(source, {source.identity.canonical_name, 1}));
        CHECK(edit.setOutput("color", edit.pinId(node, ToneMapping::color), true));
        CHECK(edit.commit());
        return request;
    }

    graph::NodeId addOutputTone(EffectGraphEdit& edit, bool imported, EColorEncoding encoding, std::string exposure)
    {
        const auto hdr = take(edit.addNode(
            outputSource(true), {std::string{imported ? hdr_import_name : hdr_source_name}, 1}
        ));
        const auto implementation = encoding == EColorEncoding::SRGB ? ToneMapping::srgb : ToneMapping::linear;
        const auto tone = take(edit.addNode(ToneMapping::definition(), {std::string{implementation}, 1},
            "exposure=" + exposure));
        const auto capture = take(edit.addNode(ColorCapture::definition(), {std::string{ColorCapture::name}, 1}));
        CHECK(edit.connect(edit.pinId(hdr, ToneMapping::color), edit.pinId(tone, ToneMapping::hdr_color)));
        CHECK(edit.connect(edit.pinId(tone, ToneMapping::color), edit.pinId(capture, ColorCapture::color)));
        CHECK(edit.setOutput("color", edit.pinId(tone, ToneMapping::color), true));
        CHECK(edit.setOutput("pixels", edit.pinId(capture, ColorCapture::pixels)));
        return tone;
    }
}

static void verifyTonemapOutput(
    ipc::IpcRequester& client,
    const CommandList& commands,
    DeviceSessionId session,
    SceneId scene,
    EffectOutputDesc output,
    EColorEncoding encoding
)
{
    CapturedPixels pixels;
    CreateEffectRequest request;
    request.graph = std::make_unique<EffectGraph>();
    EffectGraphEdit edit{*request.graph};
    const auto tone = addOutputTone(edit, false, encoding, "2");
    CHECK(edit.commit());
    const auto effect = call<CreateEffect>(client, commands, std::move(request)).effect;
    const auto view = call<CreateView>(client, commands, {scene}).view;
    const auto target = call<CreateTarget>(client, commands, {session, {64, 64, output.format}}).target;
    const auto attached = call<CreateRenderJob>(client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
    StartRenderJob::Request invalid_start{session, attached, target};
    const auto resource = GraphCaptureBuffer::bind(client, commands, session, {scene, view}, {&pixels, output.format});
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
        client, commands, session, {other_scene, other_view}, {&pixels, output.format}
    );
    binding.resources.front().resource = other_resource;
    const auto other_attachment =
        startRenderJob(client, commands, session, other_scene, other_view, other_target, effect, binding);
    CHECK(resource != other_resource);
    CHECK(call<RemoveEffect>(client, commands, {effect}, false).error.type == kRuntimeState);
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
    CHECK(call<RemoveEffect>(client, commands, {effect}, false).error.type == kRuntimeState);
    stopAndRemoveJob(client, commands, other_attachment);
    call<RemoveEffect>(client, commands, {effect});
    const auto readback_type = GraphCaptureDescriptor::descriptor.type;
    CHECK(call<QueryResource>(client, commands, {session, ViewRequest{scene, view}, readback_type}).resource_id == resource);
    CHECK(call<QueryResource>(
        client, commands, {session, ViewRequest{other_scene, other_view}, readback_type}
    ).resource_id == other_resource);
    const auto deadline = Clock::now() + 10s;
    const auto retire_view = [&](ViewRequest location, unsigned count)
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
        if (frame[3] != std::byte{255}) return false;
        for (std::size_t channel = 0; channel < hdr.size(); ++channel)
        {
            const auto x = hdr[channel] * gain;
            auto value = (x * (2.51 * x + .03)) / (x * (2.43 * x + .59) + .14);
            if (encoding == EColorEncoding::SRGB)
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
        producer = call<CreateEffect>(client, commands, clearOutputRequest(true)).effect;

    CapturedPixels pixels;
    CreateEffectRequest consume;
    consume.graph = std::make_unique<EffectGraph>();
    EffectGraphEdit edit{*consume.graph};
    addOutputTone(edit, true, EColorEncoding::LINEAR, "0");
    CHECK(edit.commit());
    const auto consumer = call<CreateEffect>(client, commands, std::move(consume)).effect;
    const auto writer =
        call<CreateRenderJob>(client, commands, {scene, producer_view, producer, EJobCadence::REQUESTED}).job;
    call<StartRenderJob>(client, commands, {session, writer, source, std::move(writer_binding)});
    const auto reader = call<CreateRenderJob>(client, commands, {
        scene, consumer_view, consumer, EJobCadence::REQUESTED
    }).job;
    StartRenderJob::Request start{session, reader, target};
    const auto readback = GraphCaptureBuffer::bind(
        client, commands, session, {scene, consumer_view}, {&pixels, ldr_format}
    );
    start.binding.resources.push_back({std::string{ColorCapture::name}, readback});
    start.binding.target_images.push_back({source, "unknown"});
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

    RenderResult<void> initializeNodeTest(EffectInitializationContext& context, std::string_view args) noexcept
    {
        if (args != "producer" && args != "forward" && args != "dim" && args != "tone input")
            return lux::cxx::unexpected(RenderError{kFrameContract});
        return context.initializeState<int>(args == "producer" ? 5 : 10);
    }

    std::atomic_uint aliased_target_builds{};
    std::atomic_uint distinct_target_builds{};

    RenderResult<void> buildImportedTargetNode(EffectBuildContext& context, std::string_view) noexcept
    {
        constexpr auto format = rdesc::ETextureFormat::RGBA16_SFLOAT;
        auto first = context.importTargetImage("first", format);
        if (!first) return lux::cxx::unexpected(first.error());
        auto second = context.importTargetImage("second", format);
        if (!second) return lux::cxx::unexpected(second.error());
        if (first->resource == second->resource) ++aliased_target_builds;
        else ++distinct_target_builds;

        GraphImageDesc image{0, 0, 1, 1, format};
        image.extent.source = EImageExtentSource::TARGET;
        auto& builder = context.builder();
        const auto output = take(builder.declareResource({"import.output", image}));
        const auto intermediate = take(builder.declareResource({"import.intermediate", image}));
        ComputeWork work;
        work.interface.program = take(builder.addProgram(GraphProgram{kEffectGraphImport.load(context.shaderTarget())}));
        work.interface.resources = {
            {"first_image", {*first}},
            {"second_image", {*second}},
            {"generated", {{intermediate, 0}, {}, EGraphAccess::WRITE}}
        };
        work.dispatch = ImageDispatchArguments{intermediate};
        const auto generated = take(builder.addComputePass("sample target inputs", std::move(work)));
        const auto copied = take(builder.addTransferPass(
            "publish sampled inputs", TransferWork{{lux::render::CopyImage{{generated.written.front()}, {{output, 0}}}}}
        ));
        return context.publish(node_color, {{node_color, copied.written.front()}});
    }

    constexpr std::uint64_t effect_capture_bytes = 64 * 256;

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
            original_create_pool = core.CreateDescriptorPool;
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
                if (submission_gate.compare_exchange_strong(expected, EPipelineGate::HELD))
                    submission_gate.wait(EPipelineGate::HELD);
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
                const auto result = original_submit(queue, desc);
                if (result != nri::Result::SUCCESS) return result;
                if (&queue == compute_queue)
                {
                    ++compute_submissions;
                    if (active_fault == EFault::REJECT_SUFFIX) suffix_ready.store(true);
                }
                if (is_upload)
                {
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
                if (pipeline_gate.compare_exchange_strong(expected, EPipelineGate::HELD))
                    pipeline_gate.wait(EPipelineGate::HELD);
                return original_pipeline(device, desc, pipeline);
            };
            core.CreateDescriptorPool = [](
                nri::Device& device, const nri::DescriptorPoolDesc& desc, nri::DescriptorPool*& pool
            ) noexcept
            {
                const auto result = original_create_pool(device, desc, pool);
                if (watch_texture_pool && result == nri::Result::SUCCESS && desc.textureMaxNum == 4)
                    texture_pool_grown = true;
                return result;
            };
            core.DestroyDescriptorPool = [](nri::DescriptorPool* pool) noexcept
            {
                if (watch_texture_pool && submission_gate == EPipelineGate::HELD)
                    pool_destroyed_before_submit = true;
                original_destroy_pool(pool);
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
        static inline std::atomic_uint accepted_uploads{};
        static inline std::atomic_uint reject_submission_countdown{};
        static inline std::atomic<EFault> fault{EFault::NONE};
        static inline std::atomic pipeline_gate{EPipelineGate::OPEN};
        static inline std::atomic submission_gate{EPipelineGate::OPEN};
        static inline std::atomic_bool upload_held{}, release_upload{};
        static inline std::atomic_bool suffix_rejected{}, unknown_retired{};
        static inline std::atomic_bool can_grow_texture_table{}, watch_texture_pool{};
        static inline std::atomic_bool can_batch_indexed{};
        static inline std::atomic_uint indexed_calls{}, indexed_indirect_calls{}, indexed_indirect_draws{};
        static inline std::atomic_bool texture_pool_grown{}, pool_destroyed_before_submit{};
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
        static inline decltype(nri::CoreInterface::CreateDescriptorPool) original_create_pool{};
        static inline decltype(nri::CoreInterface::DestroyDescriptorPool) original_destroy_pool{};
        static inline decltype(nri::CoreInterface::CmdDrawIndexed) original_indexed{};
        static inline decltype(nri::CoreInterface::CmdDrawIndexedIndirect) original_indexed_indirect{};
        static inline std::atomic<const nri::CommandBuffer*> upload_command{};
        static inline std::atomic_bool suffix_ready{}, unknown_submitted{};
        static inline const nri::Queue* compute_queue = nullptr;
    };

    struct EffectCaptureDescriptor final
    {
        using Config = void;
        using Ops = void;
        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.effect.readback"}, 0, 0, {},
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
                nullptr, &bindings
            );
        }

        EffectCaptureBuffer(ResourceCreateContext& context, CapturedPixels& pixels) noexcept
            : capture_(context), config_{.result = &pixels, .bytes = effect_capture_bytes}
        {
        }

        static RenderResult<std::unique_ptr<EffectCaptureBuffer>>
        create(ResourceCreateContext& context, ResourceInstanceId) noexcept
        {
            CHECK(created_ < pixels.size());
            auto capture = std::make_unique<EffectCaptureBuffer>(context, pixels[created_++]);
            UniformUploadProbe::attach(context.device);
            if (!watched_instances.isNull())
            {
                const auto* factory = context.resources.factory(kInstanceResourceName);
                const auto& ops = *factory->storage->bindings();
                capture->paged_probe_.changed = [](void*, std::uint32_t frame, const ResourceInput& input) noexcept
                {
                    if (frame >= published_paged_reads.size()) return;
                    published_paged_reads[frame] = static_cast<std::uint32_t>(input.addressed_buffers.size());
                };
                constexpr auto role = EMeshInstanceBinding::PAGED_ROOT;
                std::array<ResourceInput, 3> bindings;
                CHECK(ops.resolveGraphImport(
                    context.resources.resourceInstance(watched_instances), static_cast<std::uint32_t>(role),
                    GraphBufferDesc{meshInstanceProjectionBytes(role)}, capture->paged_probe_,
                    std::span{bindings}.first(context.frame_count)
                ));
            }
            return capture;
        }

        static inline std::array<CapturedPixels, 14> pixels;
        static inline ResourceInstanceId watched_instances;
        static inline std::array<std::atomic_uint, 3> published_paged_reads;

    private:
        static RenderResult<StorageGraphImport> describe(const void*, std::uint32_t projection) noexcept
        {
            if (projection != 0) return lux::cxx::unexpected(RenderError{kStorageContract});
            return StorageGraphImport{GraphBufferDesc{effect_capture_bytes}, {}, EGraphUse::TRANSFER_DESTINATION};
        }

        static RenderResult<void> resolve(
            void* object, std::uint32_t projection, const VGraphResourceDesc& description,
            StorageSubscription& subscription, std::span<ResourceInput> output
        ) noexcept
        {
            CHECK(projection == 0);
            auto& owner = *static_cast<EffectCaptureBuffer*>(object);
            return CaptureBuffer::resolve(&owner.capture_, description, &owner.config_, subscription, output);
        }

        static inline std::size_t created_ = 0;
        CaptureBuffer capture_;
        CaptureConfig config_;
        StorageSubscription paged_probe_;
    };

    constexpr auto node_bytes = effectPortSemantic("bytes");
    std::atomic_uint storage_node_builds{};

    RenderResult<void> buildImportedStorageNode(EffectBuildContext& context, std::string_view args) noexcept
    {
        const auto type = args == "wrong-type" ?
            CaptureDescriptor::descriptor.type.name() : EffectCaptureDescriptor::descriptor.type.name();
        const auto bytes = args == "wrong-shape" ? sizeof(std::uint32_t) : effect_capture_bytes;
        const ResourceRequirement requirement{ResourceName{type}};
        const GraphResource declaration{
            "", GraphBufferDesc{bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION
        };
        auto first = context.importResource("first", requirement, declaration);
        if (!first) return lux::cxx::unexpected(first.error());
        auto second = context.importResource("second", requirement, declaration);
        if (!second) return lux::cxx::unexpected(second.error());
        ++storage_node_builds;

        auto& builder = context.builder();
        const bool share_owner = first->resource == second->resource;
        const auto pattern = share_owner ? 0x5a5a5a5au : 0x6b6b6b6bu;
        const auto transient = take(builder.declareResource({"staging", GraphBufferDesc{effect_capture_bytes}}));
        const auto filled = take(builder.addTransferPass("fill readback", TransferWork{{
            FillBuffer{{{transient, 0}}, pattern}
        }}));
        if (!share_owner)
        {
            CHECK(builder.addTransferPass("copy to first storage", TransferWork{{
                CopyBuffer{{filled.written.front()}, {*first}}
            }}));
        }
        const auto copied = take(builder.addTransferPass("copy to storage", TransferWork{{
            CopyBuffer{{filled.written.front()}, {*second}}
        }}));
        return context.publish(node_bytes, {{node_bytes, copied.written.front()}});
    }

    enum class ETestProducerResult : std::uint8_t { SUCCESS, FAIL_AFTER_WRITE, OMIT_OUTPUT };
    std::atomic<ETestProducerResult> producer_result{ETestProducerResult::SUCCESS};
    std::atomic_uint source_links{}, sink_links{}, source_instances{}, sink_preparations{};
    std::weak_ptr<const int> producer_source_lifetime;

    class TestValueProducer final : public lux::render::InstanceData
    {
    public:
        using Configuration = ProducerSourceConfig;
        using ResolvedInputs = PreparedDataSlot<std::uint32_t>;

        static RenderResult<ResolvedInputs> resolve(const Configuration& config, InstanceLinkContext& link) noexcept
        {
            ++source_links;
            return link.resolveCpuData<std::uint32_t>(config.output);
        }

        static InstanceDataResult instantiate(const ResolvedInputs& output, InstanceInstantiationContext&) noexcept
        {
            CHECK(producer_source_lifetime.expired());
            ++source_instances;
            return std::make_unique<TestValueProducer>(output);
        }

        explicit TestValueProducer(ResolvedInputs output) noexcept : output_(output) {}

        RenderResult<void> prepare(FramePreparation& frame) noexcept override
        {
            const auto selected = producer_result.exchange(ETestProducerResult::SUCCESS);
            if (selected == ETestProducerResult::OMIT_OUTPUT) return {};
            if (selected == ETestProducerResult::FAIL_AFTER_WRITE)
            {
                if (auto staged = frame.publish(output_, 0xbad0bad0u); !staged) return staged;
                return lux::cxx::unexpected(RenderError{kFrameContract});
            }
            const auto view = frame.viewParameters();
            if (!view) return lux::cxx::unexpected(view.error());
            CHECK(view->revision > view_revision_);
            view_revision_ = view->revision;
            return frame.publish(output_, std::bit_cast<std::uint32_t>(view->value.background[0]));
        }

    private:
        ResolvedInputs output_;
        std::uint64_t view_revision_ = 0;
    };

    class TestInlineProducer final : public lux::render::InstanceData
    {
    public:
        struct Configuration final
        {
            GraphCpuDataId input;
            GraphPassId pass;
        };

        struct ResolvedInputs final
        {
            PreparedDataSlot<std::uint32_t> input;
            InlineConstantWriter<ProducerConstants> constants;
        };

        static RenderResult<ResolvedInputs> resolve(const Configuration& config, InstanceLinkContext& link) noexcept
        {
            ++sink_links;
            auto input = link.resolveCpuData<std::uint32_t>(config.input);
            if (!input) return lux::cxx::unexpected(input.error());
            auto constants = link.resolveInlineConstants<ProducerConstants>(config.pass);
            if (!constants) return lux::cxx::unexpected(constants.error());
            return ResolvedInputs{*input, *constants};
        }

        static InstanceDataResult
        instantiate(const ResolvedInputs& inputs, InstanceInstantiationContext&) noexcept
        {
            return std::make_unique<TestInlineProducer>(inputs);
        }

        explicit TestInlineProducer(ResolvedInputs inputs) noexcept : inputs_(inputs) {}

        RenderResult<void> prepare(FramePreparation& frame) noexcept override
        {
            auto value = frame.read(inputs_.input);
            if (!value) return lux::cxx::unexpected(value.error());
            CHECK(value->revision > revision_);
            revision_ = value->revision;
            ++sink_preparations;
            return frame.write(inputs_.constants, ProducerConstants{value->value});
        }

    private:
        ResolvedInputs inputs_;
        std::uint64_t revision_ = 0;
    };

    struct UniformNodeState final
    {
        EffectUniformBlock<TestUniformBlock> block{TestUniformBlock{{0x31313131, 0x31313131, 0x31313131, 0x31313131}}};
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
        const auto result = state.block.set(TestUniformBlock{{v, v, v, v}});
        out.error = result ? RenderError{} : result.error();
    }

    RenderResult<void> buildUniformOutput(EffectBuildContext& context, std::string_view arguments) noexcept
    {
        const ResourceRequirement requirement{ResourceName{EffectCaptureDescriptor::descriptor.type.name()}};
        auto external = context.importResource("readback", requirement, {
            "", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION
        });
        if (!external) return lux::cxx::unexpected(external.error());
        const auto uniform = context.uniformBlock<TestUniformBlock, UniformNodeState, &UniformNodeState::block>("color");
        if (!uniform) return lux::cxx::unexpected(uniform.error());

        auto& builder = context.builder();
        const auto intermediate = take(builder.declareResource({"generated values", GraphBufferDesc{effect_capture_bytes}}));
        const bool async = arguments == "async";
        constexpr std::uint64_t branch_gpu_ns = 80'000;
        if (async)
        {
            const auto scratch = take(builder.declareResource({"independent graphics", GraphBufferDesc{16}}));
            CHECK(builder.addTransferPass(
                "independent graphics", TransferWork{{FillBuffer{{{scratch, 0}}, 1}}},
                {.side_effect = true, .estimated_gpu_ns = branch_gpu_ns}
            ));
        }
        ComputeWork compute;
        compute.allow_async_compute = async;
        compute.interface.program = take(builder.addProgram(GraphProgram{kEffectNodeUniform.load(context.shaderTarget())}));
        compute.interface.resources = {
            {"uniform_block", {*uniform}},
            {"generated", {{intermediate, 0}, {}, EGraphAccess::WRITE}}
        };
        compute.interface.resources.front().setUniformConstant<TestUniformBlock>();
        compute.dispatch = DispatchArguments{{effect_capture_bytes / (sizeof(std::uint32_t) * 64), 1, 1}};
        const auto produced = take(builder.addComputePass(
            "compute uniform values", std::move(compute), {.estimated_gpu_ns = async ? branch_gpu_ns : 0}
        ));
        const auto copied = take(builder.addTransferPass(
            "read uniform values", TransferWork{{CopyBuffer{{produced.written.front()}, {*external}}}},
            {.estimated_gpu_ns = async ? 100u : 0u}
        ));
        return context.publish(node_bytes, {{node_bytes, copied.written.front()}});
    }

    RenderResult<void> buildPreparedOutput(EffectBuildContext& context, std::string_view args) noexcept
    {
        const ResourceRequirement requirement{ResourceName{EffectCaptureDescriptor::descriptor.type.name()}};
        auto imported = context.importResource("readback", requirement, {
            "", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION
        });
        if (!imported) return lux::cxx::unexpected(imported.error());

        const auto slot = args == "type" ?
            take(context.declareCpuData<float>()) : take(context.declareCpuData<std::uint32_t>());
        const auto dead = take(context.declareCpuData<std::uint32_t>());
        auto& builder = context.builder();
        const auto gpu = take(builder.declareResource({"prepared values", GraphBufferDesc{effect_capture_bytes}}));
        ComputeWork compute;
        compute.interface.program = take(builder.addProgram(GraphProgram{kEffectGraphProducer.load(context.shaderTarget())}));
        compute.interface.resources = {{"generated", {{gpu, 0}, {}, EGraphAccess::WRITE}}};
        compute.interface.parameters.setInlineConstant(ProducerConstants{0});
        compute.dispatch = DispatchArguments{{effect_capture_bytes / (sizeof(std::uint32_t) * 64), 1, 1}};
        const auto produced = take(builder.addComputePass("compute prepared values", std::move(compute)));
        const auto copy = take(builder.addTransferPass(
            "read prepared values", TransferWork{{CopyBuffer{{produced.written.front()}, {*imported}}}}
        ));

        const auto seed = take(builder.declareResource({"companion seed", GraphBufferDesc{4}}));
        const auto scratch = take(builder.declareResource({"companion scratch", GraphBufferDesc{4}}));
        const auto undefined = take(builder.declareResource({"uninitialized companion input", GraphBufferDesc{4}}));
        const auto unused = take(builder.declareResource({"unused companion target", GraphBufferDesc{4}}));
        const auto seeded = take(builder.addTransferPass("seed companion", TransferWork{{FillBuffer{{{seed, 0}}, 7}}}));
        const auto companion = take(builder.addTransferPass(
            "keep companion", TransferWork{{CopyBuffer{{seeded.written.front()}, {{scratch, 0}}}}}
        ));
        const auto dormant = take(builder.addTransferPass(
            "dormant undefined read", TransferWork{{CopyBuffer{{{undefined, 0}}, {{unused, 0}}}}}
        ));

        CHECK(context.addPreparation({
            InstanceDataFactory::make<TestInlineProducer>(),
            take(ConfigValue::make(TestInlineProducer::Configuration{slot, produced.pass})),
            {slot}, {}, {produced.pass}
        }));
        if (args != "missing")
        {
            auto lifetime = std::make_shared<const int>(0);
            if (args.empty()) producer_source_lifetime = lifetime;
            CHECK(context.addPreparation({
                InstanceDataFactory::make<TestValueProducer>(),
                take(ConfigValue::make(ProducerSourceConfig{slot, std::move(lifetime)})), {}, {slot}, {}, {companion.pass}
            }));
        }
        // The unused preparation has an unlinkable configuration. Even a dead cycle has to be rejected.
        std::vector<GraphCpuDataId> dead_inputs;
        if (args == "cycle")
        {
            const auto second_dead = take(context.declareCpuData<std::uint32_t>());
            dead_inputs.push_back(second_dead);
            CHECK(context.addPreparation({
                InstanceDataFactory::make<TestValueProducer>(), take(ConfigValue::make(ProducerSourceConfig{})),
                {dead}, {second_dead}, {}
            }));
        }
        // The companion's RAW predecessor activates this second group only when they are associated.
        const auto dormant_passes = args == "joint-closure" ?
            std::vector{seeded.pass, dormant.pass} : std::vector{dormant.pass};
        CHECK(context.addPreparation({
            InstanceDataFactory::make<TestValueProducer>(), take(ConfigValue::make(ProducerSourceConfig{})),
            std::move(dead_inputs), {args == "duplicate" ? slot : dead}, {}, dormant_passes
        }));
        return context.publish(node_bytes, {{node_bytes, copy.written.front()}});
    }

    enum class ETestStreamMode : std::uint8_t
    {
        VALID, BAD_INDEX, BAD_CONSTANTS, BAD_BINDING, BAD_IMAGE, BAD_SAMPLER, DUPLICATE_ELEMENT,
        DUPLICATE_LAYER, MISSING_LAYER, UNCOVERED_DRAW, EMPTY, SWAP_TILES,
        BAD_INDIRECT_STRIDE, SHORT_INDIRECT_ARGS, BAD_COUNT, WRONG_INDIRECT_USE, NO_COUNT, OLD_PROGRAM_SLOT
    };
    std::atomic<ETestStreamMode> stream_mode{ETestStreamMode::VALID};
    enum class ETestStreamPrograms : std::uint8_t { INITIAL, INVALID, PENDING, REVERSED };
    std::atomic stream_programs{ETestStreamPrograms::INITIAL};
    std::atomic_uint64_t stream_program_revision{1};

    rdesc::ShaderProgramArtifact streamShader(
        rdesc::EShaderTarget target, const rdesc::EmbeddedShader& fragment,
        const rdesc::EmbeddedShader& vertex_shader = kEffectStreamVertex
    ) noexcept;

    constexpr GraphBufferRange stream_linear_arguments{4, 32};
    constexpr GraphBufferRange stream_indexed_arguments{40, 44};
    constexpr GraphBufferRange stream_count_word{8, 4};
    constexpr std::uint32_t stream_indexed_stride = 24;

    class TestRasterProducer final : public lux::render::InstanceData
    {
    public:
        struct Configuration final
        {
            GraphPassId pass;
            rdesc::EShaderTarget target;
            GraphResourceId indices, uniform, image, arguments, count;
            std::uint32_t sampler, comparison_sampler;
        };

        struct ResolvedInputs final
        {
            DrawStreamSlot stream;
            rdesc::EShaderTarget target;
            RasterBufferSlot indices, uniform, arguments, count;
            RasterImageSlot image;
            RasterSamplerSlot sampler, comparison_sampler;
            std::uint32_t tint_argument, tiles_argument, sampler_argument;
        };

        static RenderResult<ResolvedInputs> resolve(const Configuration& input, InstanceLinkContext& link) noexcept
        {
            auto stream = link.resolveDrawStream(input.pass);
            auto index = link.resolveRasterBuffer(input.pass, input.indices);
            auto uniform = link.resolveRasterBuffer(input.pass, input.uniform);
            auto arguments = link.resolveRasterBuffer(input.pass, input.arguments);
            auto count = link.resolveRasterBuffer(input.pass, input.count);
            auto image = link.resolveRasterImage(input.pass, input.image);
            auto sampler = link.resolveRasterSampler(input.pass, input.sampler);
            auto comparison = link.resolveRasterSampler(input.pass, input.comparison_sampler);
            auto tint_argument = link.resolveRasterArgument(input.pass, "tint");
            auto tiles_argument = link.resolveRasterArgument(input.pass, "tiles");
            auto sampler_argument = link.resolveRasterArgument(input.pass, "sampler_state");
            const bool missing_buffer = !index || !uniform || !arguments || !count;
            const bool missing_texture_binding = !image || !sampler || !comparison;
            const bool missing_argument = !tint_argument || !tiles_argument || !sampler_argument;
            if (!stream || missing_buffer || missing_texture_binding || missing_argument)
                return lux::cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            return ResolvedInputs{
                *stream, input.target, *index, *uniform, *arguments, *count, *image, *sampler, *comparison,
                *tint_argument, *tiles_argument, *sampler_argument
            };
        }

        static InstanceDataResult
        instantiate(const ResolvedInputs& input, InstanceInstantiationContext&) noexcept
        {
            return std::make_unique<TestRasterProducer>(input);
        }

        explicit TestRasterProducer(ResolvedInputs selection) noexcept : selection_(selection) {}

        std::uint64_t rasterProgramRevision() const noexcept override { return stream_program_revision.load(); }

        RenderResult<void> selectRasterPrograms(RasterProgramSelection& selection) noexcept override
        {
            const auto mode = stream_programs.load();
            ParameterBinding parameters;
            parameters.setInlineConstant(ProducerConstants{0});
            auto uniform = streamShader(selection_.target, kEffectStreamUniform);
            if (mode == ETestStreamPrograms::INVALID)
                uniform.target = selection_.target == rdesc::EShaderTarget::SPIRV ?
                    rdesc::EShaderTarget::DXIL : rdesc::EShaderTarget::SPIRV;
            std::array programs{
                RuntimeRasterProgram{
                    std::make_shared<const rdesc::ShaderProgramArtifact>(streamShader(selection_.target, kEffectStreamInline)),
                    {}, std::move(parameters)
                },
                RuntimeRasterProgram{std::make_shared<const rdesc::ShaderProgramArtifact>(std::move(uniform)), {}, {}}
            };
            if (mode == ETestStreamPrograms::PENDING) programs[0].graphics.blend.enabled = true;
            if (mode == ETestStreamPrograms::REVERSED)
            {
                for (auto& program : programs) program.graphics.front_counter_clockwise = false;
                std::swap(programs[0], programs[1]);
            }
            return selection.setPrograms(selection_.stream, programs);
        }

        RenderResult<void> prepare(FramePreparation& frame) noexcept override
        {
            const auto mode = stream_mode.exchange(ETestStreamMode::VALID);
            if (mode == ETestStreamMode::EMPTY) return frame.setRasterStream(selection_.stream, {});
            const bool reversed = stream_programs.load() == ETestStreamPrograms::REVERSED;
            auto first_program = frame.rasterProgram(selection_.stream, reversed ? 1 : 0);
            auto second_program = frame.rasterProgram(selection_.stream, reversed ? 0 : 1);
            if (!first_program || !second_program) return lux::cxx::unexpected(RenderError{kFrameContract});
            if (!first_slot_) first_slot_ = *first_program;
            if (mode == ETestStreamMode::OLD_PROGRAM_SLOT) first_program = *first_slot_;
            const std::array values{ProducerConstants{241}, ProducerConstants{83}};
            constexpr GraphImageRange first_tile{1, 0, 1, 0, 1};
            constexpr GraphImageRange second_tile{1, 1, 1, 1, 1};
            constexpr GraphImageRange outside_envelope{1, 1, 1, 0, 1};
            const bool swap_tiles = mode == ETestStreamMode::SWAP_TILES;
            const auto first_range = mode == ETestStreamMode::BAD_IMAGE ? outside_envelope :
                swap_tiles ? second_tile : first_tile;
            const auto second_range = swap_tiles ? first_tile : second_tile;
            const auto sampler = mode == ETestStreamMode::BAD_SAMPLER ? selection_.comparison_sampler :
                selection_.sampler;
            // Physical descriptor order is compiler-owned. Publish in a different order.
            const std::array bindings{
                RasterStreamBinding{selection_.tiles_argument, RasterImageSlice{selection_.image, second_range},
                    mode == ETestStreamMode::DUPLICATE_ELEMENT ? 0u : 1u},
                RasterStreamBinding{selection_.sampler_argument, sampler},
                RasterStreamBinding{selection_.tint_argument,
                    RasterBufferSlice{selection_.uniform, 0, mode == ETestStreamMode::BAD_BINDING ? 257u : 256u}},
                RasterStreamBinding{selection_.tiles_argument, RasterImageSlice{selection_.image, first_range}, 0}
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
                    RasterStreamIndexed{{selection_.indices, offset, 24 - offset}, EIndexType::UINT16, 6}
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
                        selection_.count, stream_count_word.offset + skip, stream_count_word.size - skip
                    };
                }
                draws[0].draw = RasterStreamIndirect{
                    {selection_.arguments, stream_linear_arguments.offset, stream_linear_arguments.size},
                    count,
                    {},
                    2
                };
                auto arguments = selection_.arguments;
                if (mode == ETestStreamMode::WRONG_INDIRECT_USE) arguments = selection_.uniform;
                const auto shortfall = mode == ETestStreamMode::SHORT_INDIRECT_ARGS ? 4u : 0u;
                draws[2].draw = RasterStreamIndirect{
                    {arguments, stream_indexed_arguments.offset, stream_indexed_arguments.size - shortfall},
                    count,
                    RasterStreamIndexBinding{{selection_.indices, 0, 24}, EIndexType::UINT16},
                    2,
                    mode == ETestStreamMode::BAD_INDIRECT_STRIDE ? 18u : stream_indexed_stride
                };
            }
            const float viewport_width = 64.0f / frame.target().width;
            const std::array sections{
                RasterStreamSection{{0, 1}, 1, RasterViewport{viewport_width / 2, 0, viewport_width / 2, 0.5f}},
                RasterStreamSection{
                    {1, mode == ETestStreamMode::UNCOVERED_DRAW ? 1u : 2u},
                    mode == ETestStreamMode::DUPLICATE_LAYER ? 1u : 0u,
                    RasterViewport{0, 0, viewport_width, 1}
                }
            };
            const auto section_count = mode == ETestStreamMode::MISSING_LAYER ? 1u : 2u;
            return frame.setRasterStream(selection_.stream, {
                draws, bindings, {}, std::as_bytes(std::span{values}), std::span{sections}.first(section_count)
            });
        }

    private:
        ResolvedInputs selection_;
        std::optional<RasterProgramSlot> first_slot_;
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

    std::uint32_t addEffectRasterProgram(EffectBuildContext& context, const rdesc::EmbeddedShader& fragment) noexcept
    {
        return take(context.builder().addProgram(GraphProgram{streamShader(context.shaderTarget(), fragment)}));
    }

    RenderResult<void> buildRasterStreamOutput(EffectBuildContext& context, std::string_view) noexcept
    {
        auto imported = context.importResource(
            "readback",
            {ResourceName{EffectCaptureDescriptor::descriptor.type.name()}},
            {"", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION}
        );
        if (!imported) return lux::cxx::unexpected(imported.error());
        auto& builder = context.builder();
        GraphImageDesc color_image{0, 0, 1, 2};
        color_image.extent.source = EImageExtentSource::TARGET;
        const auto color = take(builder.declareResource({"raster stream color", color_image}));
        const auto indices = take(builder.declareResource({"raster stream indices", GraphBufferDesc{24}}));
        const auto arguments = take(builder.declareResource({"raster stream arguments", GraphBufferDesc{84}}));
        const auto count = take(builder.declareResource({"raster stream count", GraphBufferDesc{16}}));
        auto indirect_shader = kEffectStreamIndirect.load(context.shaderTarget());
        ComputeWork generate_args;
        generate_args.interface.program = take(builder.addProgram(GraphProgram{std::move(indirect_shader)}));
        generate_args.interface.resources = {
            {"arguments", {{arguments, 0}, {}, EGraphAccess::WRITE}},
            {"count", {{count, 0}, {}, EGraphAccess::WRITE}}
        };
        generate_args.dispatch = DispatchArguments{{1, 1, 1}};
        const auto generated = take(builder.addComputePass("generate indirect commands", std::move(generate_args)));
        CHECK(generated.written.size() == 2);
        const auto arguments_ready = generated.written[0];
        const auto count_ready = generated.written[1];
        CHECK(arguments_ready.resource == arguments && count_ready.resource == count);
        const auto uniform = take(builder.declareResource({"raster stream uniform", GraphBufferDesc{256}}));
        const auto shade = take(builder.addTransferPass("fill uniform", TransferWork{{FillBuffer{{{uniform, 0}}, 197}}}));
        GraphImageDesc sampled_image{32, 32, 2, 2};
        sampled_image.extent.source = EImageExtentSource::TARGET;
        const auto tiles = take(builder.declareResource({"raster stream tiles", sampled_image}));
        constexpr GraphImageRange first_tile{1, 0, 1, 0, 1};
        constexpr GraphImageRange second_tile{1, 1, 1, 1, 1};
        const auto tiled = take(builder.addTransferPass("fill first tile", TransferWork{{
            ClearImage{{{tiles, 0}, first_tile}, {65.0f / 255, 0, 97.0f / 255, 1}}
        }}));
        const auto mipped = take(builder.addTransferPass("fill second tile", TransferWork{{
            ClearImage{{tiled.written.front(), second_tile}, {129.0f / 255, 0, 161.0f / 255, 1}}
        }}));
        const auto sampler = builder.addSampler({ESamplerFilter::NEAREST});
        const auto comparison_sampler = builder.addSampler({
            ESamplerFilter::NEAREST, ESamplerAddress::CLAMP_TO_EDGE, EDepthCompare::LESS
        });
        ResourceVersion index_data{indices, 0};
        for (std::uint32_t i = 0; i < 6; ++i)
        {
            const auto start = (i < 3 ? 6u : 0u) + (i % 3) * 2;
            const auto pair = start | ((start + 1) << 16);
            const auto init = take(builder.addTransferPass(
                "fill index word " + std::to_string(i),
                TransferWork{{FillBuffer{{index_data, GraphBufferRange{i * 4, 4}}, pair}}}
            ));
            index_data = init.written.front();
        }

        RasterPassTemplate stage;
        stage.purpose = "effect-stream";
        stage.inputs = {
            {"tint", rdesc::EDescriptorType::UNIFORM_BUFFER, EGraphUse::STORAGE_READ, 2},
            {"tiles", rdesc::EDescriptorType::SAMPLED_IMAGE, EGraphUse::SAMPLED, 2},
            {"sampler_state", rdesc::EDescriptorType::SAMPLER, EGraphUse::SAMPLED, 2}
        };
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
        stage.runtime_stream_programs = true;
        RasterWork raster;
        raster.colors.push_back({{{color, 0}}});
        raster.program = std::move(stage);
        const auto drawn = take(builder.addRasterPass("two-program raster stream", std::move(raster)));
        constexpr std::uint32_t layer_bytes = effect_capture_bytes / 2;
        const auto first_copy = take(builder.addTransferPass(
            "read raster stream layer 0",
            TransferWork{{CopyImageBuffer{
                {drawn.written.front(), GraphImageRange{1, 0, 1, 0, 1}},
                {*imported, GraphBufferRange{0, layer_bytes}}, 256, layer_bytes, ImageCopyRegion{0, 0, 64, 32}
            }}}
        ));
        const auto copied = take(builder.addTransferPass(
            "read raster stream layer 1",
            TransferWork{{CopyImageBuffer{
                {drawn.written.front(), GraphImageRange{1, 0, 1, 1, 1}},
                {first_copy.written.front(), GraphBufferRange{layer_bytes, layer_bytes}},
                256, layer_bytes, ImageCopyRegion{0, 0, 64, 32}
            }}}
        ));
        CHECK(context.addPreparation({
            InstanceDataFactory::make<TestRasterProducer>(),
            take(ConfigValue::make(TestRasterProducer::Configuration{
                drawn.pass, context.shaderTarget(), indices, uniform, tiles, arguments, count, sampler, comparison_sampler
            })),
            {}, {}, {}, {drawn.pass}
        }));
        return context.publish(node_bytes, {{node_bytes, copied.written.front()}});
    }

    std::atomic_uint32_t runtime_layer_count{2};
    std::atomic_bool runtime_layer_conflict{false};
    enum class ELayerBias { VALID, MISSING, NONFINITE, UNDECLARED };
    std::atomic runtime_layer_bias{ELayerBias::VALID};

    class RuntimeLayerProducer final : public lux::render::InstanceData
    {
    public:
        struct Configuration final
        {
            GraphPassId pass;
            std::uint32_t program;
            bool preserve;
        };

        struct ResolvedInputs final
        {
            DrawStreamSlot stream;
            RasterProgramSlot program;
            bool preserve;
        };

        static RenderResult<ResolvedInputs> resolve(const Configuration& input, InstanceLinkContext& link) noexcept
        {
            auto stream = link.resolveDrawStream(input.pass);
            if (!stream) return lux::cxx::unexpected(stream.error());
            auto program = link.resolveRasterProgram(input.pass, input.program);
            if (!program) return lux::cxx::unexpected(program.error());
            return ResolvedInputs{*stream, *program, input.preserve};
        }

        static InstanceDataResult instantiate(const ResolvedInputs& input, InstanceInstantiationContext&) noexcept
        {
            return std::make_unique<RuntimeLayerProducer>(input);
        }

        explicit RuntimeLayerProducer(ResolvedInputs input) noexcept : input_(input) {}

        RenderResult<void> prepare(FramePreparation& frame) noexcept override
        {
            const auto requested = runtime_layer_count.load();
            const auto count = input_.preserve && runtime_layer_conflict.load() ? requested % 3 + 1 : requested;
            constexpr std::array values{ProducerConstants{40}, ProducerConstants{80}, ProducerConstants{120}};
            constexpr std::array biases{RasterDepthBias{1048576}, RasterDepthBias{}, RasterDepthBias{-1048576}};
            std::array<RasterStreamSection, values.size()> sections{};
            std::array<RasterStreamDraw, values.size()> draws{};
            auto layer_sections = std::span{sections}.first(count);
            for (std::uint32_t i = 0; i < layer_sections.size(); ++i)
            {
                layer_sections[i] = {input_.preserve ? RasterStreamRange{} : RasterStreamRange{i, 1}, i};
                const auto selected = runtime_layer_bias.load();
                if (input_.preserve)
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
                    input_.program, {}, {}, {i * std::uint32_t{sizeof(ProducerConstants)}, sizeof(ProducerConstants)},
                    DrawVertices{6}
                };
            }
            return frame.setRasterStream(input_.stream, {
                std::span{draws}.first(input_.preserve ? 0 : count),
                {}, {}, std::as_bytes(std::span{values}), layer_sections
            });
        }

    private:
        ResolvedInputs input_;
    };

    RenderResult<void> buildRuntimeLayersOutput(EffectBuildContext& context, std::string_view) noexcept
    {
        auto imported = context.importResource(
            "readback",
            {ResourceName{EffectCaptureDescriptor::descriptor.type.name()}},
            {"", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION}
        );
        if (!imported) return lux::cxx::unexpected(imported.error());
        auto& builder = context.builder();
        GraphImageDesc image{64, 32, 1, 2};
        image.runtime_layers = true;
        const auto layers = take(builder.declareResource({"runtime raster layers", image}));
        image.format = rdesc::ETextureFormat::D32_SFLOAT;
        const auto depth = take(builder.declareResource({"runtime depth layers", image}));
        const auto graphics = addEffectRasterProgram(context, kEffectStreamInline);
        const auto raster = [&](ResourceVersion color_input, ResourceVersion depth_input, bool preserve)
        {
            RasterPassTemplate stage;
            stage.purpose = "runtime raster layers";
            ParameterBinding parameters;
            parameters.setInlineConstant(ProducerConstants{0});
            GraphicsState state;
            state.depth_test = state.depth_write = true;
            stage.stream_programs = {{graphics, std::move(state), std::move(parameters)}};
            stage.section_depth_bias = !preserve;
            RasterWork work;
            const auto load = preserve ? EAttachmentLoad::LOAD : EAttachmentLoad::CLEAR;
            work.colors.push_back({{color_input}, load});
            work.depth = RasterAttachment{{depth_input}, load};
            work.program = std::move(stage);
            const auto pass = take(builder.addRasterPass(preserve ? "load dynamic layers" : "draw dynamic layers",
                std::move(work)));
            CHECK(context.addPreparation({
                InstanceDataFactory::make<RuntimeLayerProducer>(),
                take(ConfigValue::make(RuntimeLayerProducer::Configuration{pass.pass, graphics, preserve})),
                {}, {}, {}, {pass.pass}
            }));
            return std::array{pass.written[0], pass.written[1]};
        };
        const auto drawn = raster({layers, 0}, {depth, 0}, false);
        const auto preserved = raster(drawn[0], drawn[1], true);
        const auto words = take(
            builder.declareResource({"runtime layer capture", GraphBufferDesc{effect_capture_bytes}})
        );
        ComputeWork read;
        auto shader = kEffectStreamLayers.load(context.shaderTarget());
        read.interface.program = take(builder.addProgram(GraphProgram{std::move(shader)}));
        read.interface.resources = {
            {"layers", {preserved[0]}},
            {"depth_layers", {preserved[1]}},
            {"generated", {{words, 0}, {}, EGraphAccess::WRITE}}
        };
        read.dispatch = DispatchArguments{{effect_capture_bytes / (sizeof(std::uint32_t) * 64), 1, 1}};
        const auto values = take(builder.addComputePass("read dynamic layers", std::move(read)));
        const auto copied = take(builder.addTransferPass(
            "publish dynamic layers readback", TransferWork{{CopyBuffer{{values.written.front()}, {*imported}}}}
        ));
        return context.publish(node_bytes, {{node_bytes, copied.written.front()}});
    }

    constexpr auto tone_left = effectPortSemantic("tone_left");
    constexpr auto tone_right = effectPortSemantic("tone_right");

    RenderResult<void> buildToneCapture(EffectBuildContext& context, std::string_view) noexcept
    {
        const auto left = context.inputs(tone_left);
        const auto right = context.inputs(tone_right);
        CHECK(left.size() == 1 && left.front().size() == 1);
        CHECK(right.size() == 1 && right.front().size() == 1);
        auto imported = context.importResource(
            "readback",
            {ResourceName{EffectCaptureDescriptor::descriptor.type.name()}},
            {"", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION}
        );
        if (!imported) return lux::cxx::unexpected(imported.error());
        constexpr std::uint32_t bytes = effect_capture_bytes / 2;
        auto& builder = context.builder();
        const auto first = take(builder.addTransferPass("copy left tone", TransferWork{{CopyImageBuffer{
            {left.front().front().version}, {*imported, GraphBufferRange{0, bytes}}, 256, bytes, ImageCopyRegion{0, 0, 64, 32}
        }}}));
        const auto second = take(builder.addTransferPass("copy right tone", TransferWork{{CopyImageBuffer{
            {right.front().front().version}, {first.written.front(), GraphBufferRange{bytes, bytes}},
            256, bytes, ImageCopyRegion{0, 0, 64, 32}
        }}}));
        return context.publish(node_bytes, {{node_bytes, second.written.front()}});
    }

    RenderResult<void> buildNodeTest(EffectBuildContext& context, std::string_view args) noexcept
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
        auto& builder = context.builder();
        auto color = builder.declareResource({"node.color", image});
        if (!color) return lux::cxx::unexpected(color.error());
        if (args == "dim" || args == "tone input")
        {
            auto cleared = builder.addTransferPass(
                "dim source", TransferWork{{ClearImage{{{*color, 0}}, {.125f, .375f, .625f, 1}}}}
            );
            if (!cleared) return lux::cxx::unexpected(cleared.error());
            return context.publish(node_color, {{node_color, cleared->written.front()}});
        }
        auto intermediate = builder.declareResource({"node.intermediate", image});
        if (!intermediate) return lux::cxx::unexpected(intermediate.error());
        auto program = builder.addProgram(GraphProgram{kEffectGraphColor.load(context.shaderTarget())});
        if (!program) return lux::cxx::unexpected(program.error());
        ComputeWork compute;
        compute.interface.program = *program;
        compute.interface.resources = {{"generated", {{*intermediate, 0}, {}, EGraphAccess::WRITE}}};
        compute.dispatch = ImageDispatchArguments{*intermediate};
        auto generated = builder.addComputePass("node generate", std::move(compute));
        if (!generated) return lux::cxx::unexpected(generated.error());
        auto copied = builder.addTransferPass(
            "node publish", TransferWork{{lux::render::CopyImage{{generated->written.front()}, {{*color, 0}}}}}
        );
        if (!copied) return lux::cxx::unexpected(copied.error());
        return context.publish(node_color, {{node_color, copied->written.front()}});
    }
}

static void effectGraphOutput(bool d3d12, bool async_uniform, bool uniform_faults)
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

    take(server->registerResource<EffectCaptureBuffer>());
    constexpr std::string_view storage_schema = "test.render.effect.storage";
    CapabilityDefinition storage_capability;
    storage_capability.identity = {graph::nodeTypeId(storage_schema), std::string{storage_schema}, 1};
    storage_capability.ports = {{node_bytes, "bytes", {"test.readback_bytes", 1}, graph::EPinDirection::OUTPUT}};
    CHECK(server->registerEffectCapability(storage_capability));
    EffectImplementation storage_implementation;
    storage_implementation.capability = storage_capability.identity.id;
    storage_implementation.identity = {"test.node.storage_import", 1};
    storage_implementation.supported_outputs = {node_bytes};
    storage_implementation.initialize = [](EffectInitializationContext&, std::string_view) noexcept -> RenderResult<void>
    {
        return {};
    };
    storage_implementation.build = buildImportedStorageNode;
    const auto storage_selection = storage_implementation.identity;
    auto producer_implementation = storage_implementation;
    producer_implementation.identity = {"test.node.preparation", 1};
    producer_implementation.build = buildPreparedOutput;
    const auto producer_selection = producer_implementation.identity;
    auto stream_implementation = storage_implementation;
    stream_implementation.identity = {"test.node.raster_stream", 1};
    stream_implementation.build = buildRasterStreamOutput;
    const auto stream_selection = stream_implementation.identity;
    CHECK(server->registerEffectImplementation(std::move(stream_implementation)));
    auto runtime_layers = storage_implementation;
    runtime_layers.identity = {"test.node.runtime_raster_layers", 1};
    runtime_layers.build = buildRuntimeLayersOutput;
    const auto runtime_selection = runtime_layers.identity;
    CHECK(server->registerEffectImplementation(std::move(runtime_layers)));
    CHECK(server->registerEffectImplementation(std::move(storage_implementation)));
    CHECK(server->registerEffectImplementation(std::move(producer_implementation)));

    const auto uniform_command = take(server->registerNodeEffectCommand(
        CommandBinding::declareForEffect<SetTestUniformBlock, UniformNodeState, setTestUniformBlock>()
    ));
    auto uniform_capability = storage_capability;
    constexpr std::string_view uniform_schema = "test.render.effect.uniform";
    uniform_capability.identity = {graph::nodeTypeId(uniform_schema), std::string{uniform_schema}, 1};
    uniform_capability.commands = {uniform_command.command};
    CHECK(server->registerEffectCapability(uniform_capability));
    EffectImplementation uniform_implementation;
    uniform_implementation.capability = uniform_capability.identity.id;
    uniform_implementation.identity = {"test.node.uniform", 1};
    uniform_implementation.supported_outputs = {node_bytes};
    uniform_implementation.command_bindings = {uniform_command};
    uniform_implementation.initialize = [](EffectInitializationContext& context, std::string_view) noexcept
    {
        return context.initializeState<UniformNodeState>();
    };
    uniform_implementation.build = buildUniformOutput;
    const auto uniform_selection = uniform_implementation.identity;
    CHECK(server->registerEffectImplementation(std::move(uniform_implementation)));

    const auto command = take(server->registerNodeEffectCommand(
        CommandBinding::declareForEffect<SetNodeTestValue, int, setNodeTestValue>()
    ));
    constexpr std::string_view schema = "test.render.effect.color";
    CapabilityDefinition capability;
    capability.identity = {graph::nodeTypeId(schema), std::string{schema}, 1};
    capability.ports = {
        {node_input, "source", {std::string{HdrColorBundle::type}, HdrColorBundle::version}, graph::EPinDirection::INPUT},
        {node_color, "color", {std::string{HdrColorBundle::type}, HdrColorBundle::version}, graph::EPinDirection::OUTPUT, 2}
    };
    capability.commands = {command.command};
    CHECK(server->registerEffectCapability(capability));

    EffectImplementation implementation;
    implementation.capability = capability.identity.id;
    implementation.identity = {"test.node.clear_or_forward", 1};
    implementation.supported_outputs = {node_color};
    implementation.command_bindings = {command};
    implementation.initialize = initializeNodeTest;
    implementation.build = buildNodeTest;
    auto selected = implementation.identity;
    auto import_implementation = implementation;
    import_implementation.identity = {"test.node.target_import", 1};
    import_implementation.build = buildImportedTargetNode;
    const auto imported_selection = import_implementation.identity;
    CHECK(server->registerEffectImplementation(std::move(implementation)));
    CHECK(server->registerEffectImplementation(std::move(import_implementation)));

    const auto tone_node = ToneMapping::definition();
    const EffectValueType display_type{std::string{ToneMapping::display_type}, 1};
    CapabilityDefinition tone_capture;
    constexpr std::string_view tone_capture_name = "test.render.effect.tone_capture";
    tone_capture.identity = {graph::nodeTypeId(tone_capture_name), std::string{tone_capture_name}, 1};
    tone_capture.ports = {
        {tone_left, "tone_left", display_type, graph::EPinDirection::INPUT, 1, true},
        {tone_right, "tone_right", display_type, graph::EPinDirection::INPUT, 1, true},
        {node_bytes, "bytes", {"test.readback_bytes", 1}, graph::EPinDirection::OUTPUT}
    };
    CHECK(server->registerEffectCapability(tone_capture));
    EffectImplementation tone_copy;
    tone_copy.capability = tone_capture.identity.id;
    tone_copy.identity = {std::string{tone_capture_name}, 1};
    tone_copy.supported_outputs = {node_bytes};
    tone_copy.initialize = [](EffectInitializationContext&, std::string_view) noexcept -> RenderResult<void> { return {}; };
    tone_copy.build = buildToneCapture;
    const auto tone_capture_selection = tone_copy.identity;
    CHECK(server->registerEffectImplementation(std::move(tone_copy)));
    graph::NodeId tone_left_node, tone_right_node;
    CreateEffectRequest tone_request;
    tone_request.graph = std::make_unique<EffectGraph>();
    {
        EffectGraphEdit tone_edit{*tone_request.graph};
        const auto hdr_input = take(tone_edit.addNode(capability, selected, "tone input"));
        const auto left_tone = take(tone_edit.addNode(tone_node, {std::string{ToneMapping::linear}, 1}, "exposure=-1"));
        const auto right_tone = take(tone_edit.addNode(tone_node, {std::string{ToneMapping::srgb}, 1}, "exposure=1"));
        tone_left_node = left_tone;
        tone_right_node = right_tone;
        const auto sink = take(tone_edit.addNode(tone_capture, tone_capture_selection, ""));
        for (const auto [node, port] : {std::pair{left_tone, tone_left}, std::pair{right_tone, tone_right}})
        {
            CHECK(tone_edit.connect(tone_edit.pinId(hdr_input, node_color), tone_edit.pinId(node, ToneMapping::hdr_color)));
            CHECK(tone_edit.connect(tone_edit.pinId(node, ToneMapping::color), tone_edit.pinId(sink, port)));
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
    const auto start_command = std::ranges::find(commands.commands, StartRenderJob::name, &CommandInfo::name);
    CHECK(start_command != commands.commands.end());
    const auto send_start = [&](RenderJobId job, TargetId output)
    {
        ipc::IpcMessage message;
        message.emplace<StartRenderJob::Request>(StartRenderJob::Request{device, job, output});
        const auto request_id = nextSceneRequest();
        CHECK(client->trySend(commandTag(start_command->id), request_id, message));
        return request_id;
    };
    const auto start_pair = [&](RenderJobId start_first, RenderJobId start_peer, unsigned expected_builds)
    {
        node_build_gate.store(ENodeBuildGate::PAUSE);
        const auto first_request = send_start(start_first, target);
        const auto deadline = Clock::now() + 10s;
        while (node_builds.load() == expected_builds)
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
        const auto peer_request = send_start(start_peer, peer_target);
        // A no-op resize reports Busy once the second Start reserves the target.
        for (;;)
        {
            const auto probe = call<ResizeTarget>(*client, commands, {peer_target, peer_desc}, false);
            if (probe.error.type == kDeviceBusy) break;
            CHECK(!probe.error.type && Clock::now() < deadline);
        }
        return std::array{first_request, peer_request};
    };
    const auto finish_pair = [&](const std::array<std::uint64_t, 2>& requests, ENodeBuildGate outcome)
    {
        node_build_gate.store(outcome, std::memory_order_release);
        node_build_gate.notify_all();
        std::array<std::optional<RenderError>, 2> results;
        auto& notifications = ipc::IpcCore::instance();
        const auto deadline = Clock::now() + 10s;
        while (!results[0] || !results[1])
        {
            const auto epoch = notifications.workEpoch();
            ipc::IpcMessage message;
            std::uint64_t correlation = 0;
            const auto status = client->tryReceive(correlation, message);
            CHECK(status != ipc::EIpcReceive::CLOSED && Clock::now() < deadline);
            if (status != ipc::EIpcReceive::RECEIVED)
            {
                notifications.waitUntil(epoch, deadline);
                continue;
            }
            const auto found = std::ranges::find(requests, correlation);
            CHECK(found != requests.end());
            auto& result = results[found - requests.begin()];
            CHECK(!result);
            result = message.get<AckResponse>().error;
        }
        return std::array{*results[0], *results[1]};
    };

    const auto failed = finish_pair(start_pair(first, peer, 0), ENodeBuildGate::FAIL);
    CHECK(failed[0].type == kFrameContract && failed[1] == failed[0]);
    CHECK(node_builds.load() == 1);

    const auto retry = start_pair(first, peer, 1);
    call<StopRenderJob>(*client, commands, first);
    const auto started = finish_pair(retry, ENodeBuildGate::PASS);
    CHECK(started[0].type == kJobStopped && !started[1].type);
    CHECK(node_builds.load() == 3);
    call<StartRenderJob>(*client, commands, {device, first, target});
    renderFrame(*client, commands, first);
    renderFrame(*client, commands, peer);
    verifyTargetInput(*client, commands, device, scene, {}, effect);
    CHECK(node_builds.load() == 3);
    CreateEffectRequest imported_request;
    imported_request.graph = std::make_unique<EffectGraph>();
    EffectGraphEdit imported_edit{*imported_request.graph};
    const auto imported_node = take(imported_edit.addNode(capability, imported_selection, "producer"));
    CHECK(imported_edit.setOutput("display", imported_edit.pinId(imported_node, node_color), true));
    CHECK(imported_edit.commit());
    const auto imported_effect = call<CreateEffect>(*client, commands, std::move(imported_request)).effect;
    const auto imported_target = call<CreateTarget>(*client, commands, {device, peer_desc}).target;
    const auto imported_job =
        call<CreateRenderJob>(*client, commands, {scene, view, imported_effect, EJobCadence::REQUESTED}).job;
    const auto bound = [](TargetId a, TargetId b)
    {
        JobBinding binding;
        // Deliberately out of name order.
        binding.target_images = {{b, "second"}, {a, "first"}};
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
        *client, commands, {device, imported_job, imported_target, bound(imported_target, target)}, false
    ).error.type == kCommandContract);
    call<StartRenderJob>(*client, commands, {device, imported_job, imported_target, bound(target, target)});
    CHECK(aliased_target_builds.load() == 1 && distinct_target_builds.load() == 0);
    CHECK(call<RemoveTarget>(*client, commands, target, false).error.type == kRuntimeState);
    const TargetDesc resized_desc{128, 64, hdr, 1, ETargetKind::OFFSCREEN, true};
    const auto resized = call<ResizeTarget>(*client, commands, {target, resized_desc});
    call<SetTargetActive>(*client, commands, {target, true, resized.epoch});
    renderFrame(*client, commands, first);
    renderFrame(*client, commands, imported_job);
    CreateEffectRequest dim_request;
    dim_request.graph = std::make_unique<EffectGraph>();
    EffectGraphEdit dim_edit{*dim_request.graph};
    const auto dim_node = take(dim_edit.addNode(capability, selected, "dim"));
    CHECK(dim_edit.setOutput("display", dim_edit.pinId(dim_node, node_color), true));
    CHECK(dim_edit.commit());
    const auto dim_effect = call<CreateEffect>(*client, commands, std::move(dim_request)).effect;
    const auto dim_target = call<CreateTarget>(*client, commands, {device, peer_desc}).target;
    const auto dim_job = startRenderJob(*client, commands, device, scene, view, dim_target, dim_effect);
    renderFrame(*client, commands, dim_job);

    verifyTargetInput(*client, commands, device, scene, {}, imported_effect,
        bound(dim_target, dim_target), {.125, .375, .625});
    CHECK(aliased_target_builds.load() == 1);
    verifyTargetInput(*client, commands, device, scene, {}, imported_effect,
        bound(target, dim_target), {.1875, .4375, .6875});
    CHECK(distinct_target_builds.load() == 1);
    stopAndRemoveJob(*client, commands, dim_job);
    call<RemoveTarget>(*client, commands, dim_target);
    call<RemoveEffect>(*client, commands, {dim_effect});
    stopAndRemoveJob(*client, commands, imported_job);
    call<RemoveTarget>(*client, commands, imported_target);
    call<RemoveEffect>(*client, commands, {imported_effect});
    std::puts("NAMED TARGET IMPORT PASS: native readback, alias key, binding checks and source resize");

    const auto make_storage_effect = [&](const ImplementationSelection& selection, std::string_view arguments)
    {
        CreateEffectRequest request;
        request.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*request.graph};
        const auto node = take(edit.addNode(storage_capability, selection, std::string{arguments}));
        CHECK(edit.setOutput("readback", edit.pinId(node, node_bytes), false));
        CHECK(edit.commit());
        return call<CreateEffect>(*client, commands, std::move(request)).effect;
    };
    const auto storage_effect = make_storage_effect(storage_selection, "");
    const auto wrong_shape_effect = make_storage_effect(storage_selection, "wrong-shape");
    const auto wrong_type_effect = make_storage_effect(storage_selection, "wrong-type");
    const auto make_storage_job = [&](EffectId selected_effect)
    {
        return call<CreateRenderJob>(*client, commands, {scene, view, selected_effect, EJobCadence::REQUESTED}).job;
    };
    const auto owner_view = call<CreateView>(*client, commands, {scene}).view;
    const auto other_view = call<CreateView>(*client, commands, {scene}).view;
    const auto make_resource = [&](ViewId owner, ResourceNameView name)
    {
        return call<GetOrCreateResource>(*client, commands, {device, ViewRequest{scene, owner}, name}).resource_id;
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
    ).error.type == kStorageContract);
    for (auto rejected : {wrong_shape_job, wrong_type_job})
    {
        CHECK(call<StartRenderJob>(
            *client, commands, {device, rejected, storage_output, storage_binding(owner, owner)}, false
        ).error.type == kStorageMismatch);
        call<RemoveRenderJob>(*client, commands, {rejected});
    }
    call<StartRenderJob>(*client, commands, {device, storage_job, storage_output, storage_binding(owner, owner)});
    // A second Job binds another physical owner with the same import contract.
    const auto other_storage_output = call<CreateTarget>(*client, commands, {device, peer_desc}).target;
    call<StartRenderJob>(
        *client, commands, {device, other_storage_job, other_storage_output, storage_binding(other_owner, other_owner)}
    );
    CHECK(storage_node_builds.load() == 1);
    CHECK(call<RemoveView>(*client, commands, {scene, owner_view}, false).error.type == kDeviceBusy);
    const auto storage_receipt = call<RequestRenderJob>(*client, commands, {storage_job});
    const auto other_receipt = call<RequestRenderJob>(*client, commands, {other_storage_job});
    stopAndRemoveJob(*client, commands, storage_job);
    stopAndRemoveJob(*client, commands, other_storage_job);
    const auto distinct_storage_job = make_storage_job(storage_effect);
    call<StartRenderJob>(
        *client, commands, {device, distinct_storage_job, storage_output, storage_binding(owner, other_owner)}
    );
    CHECK(storage_node_builds.load() == 2);
    const auto distinct_receipt = call<RequestRenderJob>(*client, commands, {distinct_storage_job});
    stopAndRemoveJob(*client, commands, distinct_storage_job);
    call<RemoveView>(*client, commands, {scene, owner_view});
    const auto stale_job = make_storage_job(storage_effect);
    CHECK(call<StartRenderJob>(
        *client, commands, {device, stale_job, storage_output, storage_binding(owner, owner)}, false
    ).error.type == kStorageIdentity);
    call<RemoveRenderJob>(*client, commands, {stale_job});
    call<RemoveView>(*client, commands, {scene, other_view});
    const auto deadline = Clock::now() + 10s;
    const std::array receipts{storage_receipt, other_receipt};
    for (std::size_t i = 0; i < receipts.size(); ++i)
    {
        auto& capture = EffectCaptureBuffer::pixels[i];
        while (!capture.ready.load(std::memory_order_acquire)) CHECK(Clock::now() < deadline);
        const auto check_capture = [&](std::uint32_t slot, std::byte expected)
        {
            CHECK(slot < capture.frames.size());
            const auto frame = std::span{capture.frames[slot]}.first(effect_capture_bytes);
            CHECK(std::ranges::all_of(frame, [expected](std::byte byte) noexcept { return byte == expected; }));
        };
        if (receipts[i].slot_index != distinct_receipt.slot_index)
            check_capture(receipts[i].slot_index, std::byte{0x5a});
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
        {"missing", kGraphUndefinedRead},
        {"cycle", kGraphDependencyCycle},
        {"type", kGraphInvalidDescriptor},
        {"duplicate", kGraphAmbiguousProducer},
        {"joint-closure", kGraphUndefinedRead}
    };
    for (const auto [mode, error] : invalid_preparations)
    {
        const auto invalid = make_storage_effect(producer_selection, mode);
        const auto invalid_job = make_storage_job(invalid);
        const auto response = call<StartRenderJob>(
            *client, commands, {device, invalid_job, producer_target, producer_binding(first_data)}, false
        );
        CHECK(response.error.type == error);
        call<RemoveRenderJob>(*client, commands, {invalid_job});
        call<RemoveEffect>(*client, commands, {invalid});
    }
    const auto source_before = source_links.load();
    const auto sink_before = sink_links.load();
    const auto prepared_job = call<CreateRenderJob>(
        *client, commands, {scene, producer_view, producer_effect, EJobCadence::REQUESTED}
    ).job;
    const auto prepared_peer = call<CreateRenderJob>(
        *client, commands, {scene, producer_peer_view, producer_effect, EJobCadence::REQUESTED}
    ).job;
    const auto first_view_data = make_resource(producer_view, kViewParameterResourceName);
    const auto peer_view_data = make_resource(producer_peer_view, kViewParameterResourceName);
    auto first_binding = producer_binding(first_data);
    first_binding.view_parameters = peer_view_data;
    const auto mismatched = call<StartRenderJob>(
        *client, commands, {device, prepared_job, producer_target, first_binding}, false
    );
    CHECK(mismatched.error.type == kStorageMismatch);
    first_binding.view_parameters = first_view_data;
    auto peer_binding = producer_binding(peer_data);
    peer_binding.view_parameters = peer_view_data;
    ViewGpuParameters first_view_parameters, peer_view_parameters;
    first_view_parameters.background[0] = std::bit_cast<float>(0x3c3c3c3cu);
    peer_view_parameters.background[0] = std::bit_cast<float>(0x5b5b5b5bu);
    call<ViewParameterCommands::Update>(*client, commands, {first_view_data, std::span{&first_view_parameters, 1}});
    call<ViewParameterCommands::Update>(*client, commands, {peer_view_data, std::span{&peer_view_parameters, 1}});
    call<StartRenderJob>(*client, commands, {device, prepared_job, producer_target, first_binding});
    call<StartRenderJob>(*client, commands, {device, prepared_peer, producer_peer_target, peer_binding});
    CHECK(source_links.load() == source_before + 1 && sink_links.load() == sink_before + 1);
    CHECK(source_instances.load() == 2);
    const auto view_schema = call<QueryRenderJobInputLayout>(*client, commands, prepared_job).layout;
    auto packet = take(FrameData::create<std::tuple<ViewParameterResource>>(view_schema));
    auto changed_view = first_view_parameters;
    changed_view.background[0] = std::bit_cast<float>(0x4d4d4d4du);
    auto invalid_view = changed_view;
    invalid_view.direction_far[3] = 0;
    CHECK(packet.addData<ViewParameterResource>(changed_view));
    CHECK(packet.addData<ViewParameterResource>(invalid_view));
    CHECK(packet.seal());
    CHECK(call<UpdateFrameData>(*client, commands, {prepared_job, &packet}, false).error.type == kFrameContract);
    packet.release();
    const auto prepared_first = call<RequestRenderJob>(*client, commands, {prepared_job});
    producer_result = ETestProducerResult::FAIL_AFTER_WRITE;
    CHECK(call<RequestRenderJob>(*client, commands, {prepared_job}, false).error.type == kFrameContract);
    producer_result = ETestProducerResult::OMIT_OUTPUT;
    CHECK(call<RequestRenderJob>(*client, commands, {prepared_job}, false).error.type == kGraphUndefinedRead);
    CHECK(sink_preparations.load() == 1);
    packet = take(FrameData::create<std::tuple<ViewParameterResource>>(view_schema));
    CHECK(packet.addData<ViewParameterResource>(changed_view));
    CHECK(packet.seal());
    const auto prepared_recovered = call<RequestRenderJob>(*client, commands, {prepared_job, &packet});
    packet.release();
    const auto prepared_peer_first = call<RequestRenderJob>(*client, commands, {prepared_peer});
    CHECK(sink_preparations.load() == 3);
    stopAndRemoveJob(*client, commands, prepared_job);
    stopAndRemoveJob(*client, commands, prepared_peer);
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
    if (prepared_first.slot_index != prepared_recovered.slot_index)
        verify_prepared(2, prepared_first.slot_index, std::byte{0x3c});
    verify_prepared(2, prepared_recovered.slot_index, std::byte{0x4d});
    verify_prepared(3, prepared_peer_first.slot_index, std::byte{0x5b});
    call<RemoveTarget>(*client, commands, producer_target);
    call<RemoveTarget>(*client, commands, producer_peer_target);
    call<RemoveEffect>(*client, commands, {producer_effect});
    std::puts("PRODUCER PASS: joint liveness, shared prototype, failure propagation, and native View parameter readback");

    const auto stream_effect = make_storage_effect(stream_selection, "");
    const auto stream_view = call<CreateView>(*client, commands, {scene}).view;
    const auto stream_data = make_resource(stream_view, EffectCaptureDescriptor::descriptor.type);
    const auto stream_target = call<CreateTarget>(*client, commands, {device, peer_desc}).target;
    const auto stream_job = make_storage_job(stream_effect);
    call<StartRenderJob>(*client, commands, {device, stream_job, stream_target, producer_binding(stream_data)});
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
    for (const auto mode : {ETestStreamMode::DUPLICATE_LAYER, ETestStreamMode::MISSING_LAYER,
                            ETestStreamMode::UNCOVERED_DRAW})
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
    stream_programs = ETestStreamPrograms::INVALID;
    ++stream_program_revision;
    CHECK(call<RequestRenderJob>(*client, commands, {stream_job}, false).error.type == kDeviceUnsupported);
    CHECK(call<QueryRenderStats>(*client, commands, {}).submissions == before_bad_program);

    stream_programs = ETestStreamPrograms::PENDING;
    ++stream_program_revision;
    using PipelineGate = UniformUploadProbe::EPipelineGate;
    UniformUploadProbe::pipeline_gate = PipelineGate::ARMED;
    auto requester = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(requester);
    auto preparing = std::async(std::launch::async, [&]
    {
        return call<RequestRenderJob>(*requester, commands, {stream_job});
    });
    const auto replacement_deadline = Clock::now() + 10s;
    while (UniformUploadProbe::pipeline_gate != PipelineGate::HELD)
    {
        CHECK(Clock::now() < replacement_deadline);
        std::this_thread::yield();
    }
    stream_programs = ETestStreamPrograms::REVERSED;
    ++stream_program_revision;
    UniformUploadProbe::pipeline_gate = PipelineGate::OPEN;
    UniformUploadProbe::pipeline_gate.notify_all();
    preparing.get();
    CHECK(call<QueryRenderStats>(*client, commands, {}).submissions == before_bad_program + 1);

    stream_mode = ETestStreamMode::OLD_PROGRAM_SLOT;
    CHECK(call<RequestRenderJob>(*client, commands, {stream_job}, false).error.type == kFrameContract);
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
    const auto uncounted_job = make_storage_job(stream_effect);
    call<StartRenderJob>(*client, commands, {device, uncounted_job, uncounted_target, producer_binding(uncounted_data)});
    stream_mode = ETestStreamMode::NO_COUNT;
    const auto uncounted = call<RequestRenderJob>(*client, commands, {uncounted_job}).slot_index;
    stopAndRemoveJob(*client, commands, uncounted_job);
    call<RemoveView>(*client, commands, {scene, uncounted_view});
    const auto& uncounted_capture = EffectCaptureBuffer::pixels[7];
    const auto uncounted_deadline = Clock::now() + 10s;
    while (!uncounted_capture.ready.load(std::memory_order_acquire)) CHECK(Clock::now() < uncounted_deadline);
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
    const auto uniform_effect = call<CreateEffect>(*client, commands, std::move(uniform_request)).effect;
    call<SetTestUniformBlock>(*client, commands, {uniform_effect, uniform_node, 0x41414141});
    const auto uniform_view_a = call<CreateView>(*client, commands, {scene}).view;
    const auto uniform_view_b = call<CreateView>(*client, commands, {scene}).view;
    const auto uniform_bytes_a = make_resource(uniform_view_a, EffectCaptureDescriptor::descriptor.type);
    const auto uniform_bytes_b = make_resource(uniform_view_b, EffectCaptureDescriptor::descriptor.type);
    const auto uniform_target_a = call<CreateTarget>(*client, commands, {device, peer_desc}).target;
    auto distinct_target = peer_desc;
    distinct_target.format = rdesc::ETextureFormat::RGBA8_UNORM;
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
    response_gate.store(EResponseGate::ARMED);
    const auto paired_before = count_submits();
    const auto pair_deadline = Clock::now() + 10s;
    while (response_gate.load() != EResponseGate::BLOCKED) CHECK(Clock::now() < pair_deadline);
    const auto request_command = std::ranges::find(commands.commands, RequestRenderJob::name, &CommandInfo::name);
    CHECK(request_command != commands.commands.end());
    for (auto job : {uniform_job_a, uniform_job_b})
    {
        ipc::IpcMessage message;
        message.emplace<RequestRenderJob::Request>(RequestRenderJob::Request{job});
        CHECK(client->trySend(commandTag(request_command->id), nextSceneRequest(), message));
    }
    response_gate.store(EResponseGate::OPEN);
    response_gate.notify_all();
    std::array<JobSubmitted, 2> paired;
    for (std::size_t received = 0; received < paired.size();)
    {
        ipc::IpcMessage message;
        std::uint64_t correlation = 0;
        CHECK(Clock::now() < pair_deadline);
        if (client->tryReceive(correlation, message) != ipc::EIpcReceive::RECEIVED)
        {
            std::this_thread::yield();
            continue;
        }
        const auto reply = message.get<JobSubmitted>();
        CHECK(!reply.error.type);
        CHECK(reply.job == uniform_job_a || reply.job == uniform_job_b);
        paired[reply.job == uniform_job_a ? 0 : 1] = reply;
        ++received;
    }
    CHECK(paired[0].job == uniform_job_a && paired[1].job == uniform_job_b);
    CHECK(paired[0].frame_index == paired[1].frame_index && paired[0].slot_index == paired[1].slot_index);
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
    if (uniform_faults && UniformUploadProbe::hasSeparateComputeQueue())
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
        CHECK(unknown.job.isNull() && unknown.frame_index == 0 && unknown.slot_index == 0);
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
    stopAndRemoveJob(*client, commands, first);
    stopAndRemoveJob(*client, commands, peer);

    CreateEffectRequest shutdown_request{std::make_unique<EffectGraph>()};
    EffectGraphEdit shutdown_edit{*shutdown_request.graph};
    const auto shutdown_node = take(shutdown_edit.addNode(capability, selected, "producer"));
    CHECK(shutdown_edit.setOutput("display", shutdown_edit.pinId(shutdown_node, node_color), true));
    CHECK(shutdown_edit.commit());
    const auto shutdown_effect = call<CreateEffect>(*client, commands, std::move(shutdown_request)).effect;
    const auto shutdown_first =
        call<CreateRenderJob>(*client, commands, {scene, view, shutdown_effect, EJobCadence::REQUESTED}).job;
    const auto shutdown_peer =
        call<CreateRenderJob>(*client, commands, {scene, view, shutdown_effect, EJobCadence::REQUESTED}).job;
    const auto shutdown = start_pair(shutdown_first, shutdown_peer, node_builds.load());
    server->stop();
    const auto cancelled = finish_pair(shutdown, ENodeBuildGate::PASS);
    CHECK(cancelled[0].type == kCommandCancelled && cancelled[1].type == kCommandCancelled);
    render.join();
    CHECK(server_result);
    CHECK(server->stats().validation_errors == 0);
    std::printf("EFFECT GRAPH PASS backend=%s pending compile sharing, stopped leader, retry, shutdown and pixel readback\n",
        d3d12 ? "D3D12" : "Vulkan");
}

static void preparedBuffers(bool d3d12)
{
    enum class EPhase { SMALL, GROWN, REPLACEMENT, FAIL_AFTER_WRITE, OMIT, OVERSIZED };
    static std::atomic phase{EPhase::SMALL};
    static std::atomic_uint graph_builds{};
    class Producer final : public lux::render::InstanceData
    {
    public:
        struct Configuration final
        {
            GraphResourceId destination;
            GraphStorageSnapshotId lights;
        };

        struct ResolvedInputs final
        {
            PreparedBufferWriter writer;
            StorageSnapshotSlot<ShadowLightSnapshot> lights;
        };

        static RenderResult<ResolvedInputs> resolve(const Configuration& source, InstanceLinkContext& link) noexcept
        {
            auto writer = link.resolvePreparedBuffer(source.destination);
            if (!writer) return lux::cxx::unexpected(writer.error());
            auto lights = link.resolveStorageSnapshot<ShadowLightSnapshot>(source.lights);
            if (!lights) return lux::cxx::unexpected(lights.error());
            return ResolvedInputs{*writer, *lights};
        }

        static InstanceDataResult instantiate(const ResolvedInputs& inputs, InstanceInstantiationContext&) noexcept
        {
            return std::make_unique<Producer>(inputs);
        }

        explicit Producer(ResolvedInputs inputs) noexcept : inputs_(inputs) {}

        RenderResult<void> prepare(FramePreparation& frame) noexcept override
        {
            const auto selected = phase.load();
            if (selected == EPhase::OMIT) return {};
            const auto lights = frame.readStorage(inputs_.lights);
            if (!lights) return lux::cxx::unexpected(lights.error());
            const auto& snapshot = lights->value.get();
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
                return frame.writeBuffer(inputs_.writer, std::as_bytes(std::span{oversized}));
            if (selected == EPhase::SMALL)
                return frame.writeBuffer(inputs_.writer, std::as_bytes(std::span{initial}), lights->revision);
            const auto revision = lights->revision * 2 + (selected == EPhase::REPLACEMENT);
            auto written = frame.writeBuffer(inputs_.writer, std::as_bytes(std::span{grown}), revision);
            if (selected == EPhase::FAIL_AFTER_WRITE && written)
                return lux::cxx::unexpected(RenderError{kFrameContract});
            return written;
        }

    private:
        ResolvedInputs inputs_;
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

    CapabilityDefinition definition;
    constexpr std::string_view node_name = "test.prepared_buffers";
    definition.identity = {graph::nodeTypeId(node_name), std::string{node_name}, 1};
    definition.ports = {{node_bytes, "bytes", {"test.readback_bytes", 1}, graph::EPinDirection::OUTPUT}};
    CHECK(server->registerEffectCapability(definition));
    EffectImplementation implementation;
    implementation.capability = definition.identity.id;
    implementation.identity = {"test.prepared_buffers.compute", 1};
    implementation.supported_outputs = {node_bytes};
    implementation.initialize = [](EffectInitializationContext&, std::string_view) noexcept -> RenderResult<void>
    {
        return {};
    };
    implementation.build = [](EffectBuildContext& context, std::string_view mode) noexcept -> RenderResult<void>
    {
        ++graph_builds;
        auto selected = context.declarePreparedBuffer("selection", 16, 1024);
        if (!selected) return lux::cxx::unexpected(selected.error());
        const ResourceRequirement light_owner{ResourceName{kLightResourceName.name()}};
        auto lights = mode == "wrong-type" ? context.importStorageSnapshot<std::uint32_t>("lights", light_owner) :
            context.importStorageSnapshot<ShadowLightSnapshot>("lights", light_owner);
        if (!lights) return lux::cxx::unexpected(lights.error());
        const auto add_producer = [&]() noexcept -> RenderResult<void>
        {
            return context.addPreparation({
                InstanceDataFactory::make<Producer>(), take(ConfigValue::make(Producer::Configuration{
                    selected->resource, *lights
                })),
                {}, {}, {}, {}, {selected->resource}, {*lights}
            });
        };
        if (mode != "missing") CHECK(add_producer());
        if (mode == "duplicate") CHECK(add_producer());
        const ResourceRequirement owner{ResourceName{EffectCaptureDescriptor::descriptor.type.name()}};
        const auto readback = take(context.importResource("readback", owner, {
            "", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION
        }));
        auto& builder = context.builder();
        const auto output = take(builder.declareResource({"generated", GraphBufferDesc{effect_capture_bytes}}));
        ComputeWork compute;
        compute.interface.program = take(builder.addProgram(GraphProgram{kEffectPreparedBuffer}));
        compute.interface.resources = {
            {"prepared", {*selected}},
            {"generated", {{output, 0}, {}, EGraphAccess::WRITE}}
        };
        compute.dispatch = DispatchArguments{{effect_capture_bytes / (sizeof(std::uint32_t) * 64), 1, 1}};
        const auto dispatched = take(builder.addComputePass("read producer buffer", std::move(compute)));
        const auto result = take(builder.addTransferPass(
            "capture", TransferWork{{CopyBuffer{{dispatched.written.front()}, {readback}}}}
        ));
        return context.publish(node_bytes, {{node_bytes, result.written.front()}});
    };
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
        device, ViewRequest{scene, view}, EffectCaptureDescriptor::descriptor.type
    }).resource_id;
    binding.resources.push_back({"readback", capture});
    const auto light_owner = call<GetOrCreateResource>(*client, commands, {device, scene, kLightResourceName}).resource_id;
    const std::array<VDirectLight, 1> values{DirectionalLightGpu{}};
    std::array settings{LightShadowSettings{.enabled = true, .depth_bias_constant = 54}};
    const auto light = call<LightingCommands::CreateLights>(*client, commands, {light_owner, {1, values, settings}})
        .lights.front();
    binding.resources.push_back({"lights", light_owner, 0, EJobResourceUse::CPU_SNAPSHOT});
    const auto start = [&](EffectId effect)
    {
        return call<CreateRenderJob>(*client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
    };
    const auto missing = start(make_effect("missing"));
    const auto duplicate = start(make_effect("duplicate"));
    const auto wrong_type = start(make_effect("wrong-type"));
    CHECK(call<StartRenderJob>(*client, commands, {device, missing, target, binding}, false).error.type ==
          kGraphUndefinedRead);
    CHECK(call<StartRenderJob>(*client, commands, {device, duplicate, target, binding}, false).error.type ==
          kGraphAmbiguousProducer);
    CHECK(call<StartRenderJob>(*client, commands, {device, wrong_type, target, binding}, false).error.type ==
          kStorageMismatch);
    call<RemoveRenderJob>(*client, commands, missing);
    call<RemoveRenderJob>(*client, commands, duplicate);
    call<RemoveRenderJob>(*client, commands, wrong_type);
    const auto effect = make_effect("");
    const auto job = start(effect);
    call<StartRenderJob>(*client, commands, {device, job, target, binding});
    const auto published_builds = graph_builds.load();
    const auto submissions = [&] { return call<QueryRenderStats>(*client, commands, {}).submissions; };
    const auto before = submissions();
    call<RequestRenderJob>(*client, commands, {job});
    const auto other_initial = call<RequestRenderJob>(*client, commands, {job});
    CHECK(submissions() - before == 4);
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
        device, ViewRequest{recovery_scene, recovery_view}, EffectCaptureDescriptor::descriptor.type
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
    stopAndRemoveJob(*client, commands, recovery_job);
    call<RemoveView>(*client, commands, {recovery_scene, recovery_view});
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
    std::printf("PREPARED BUFFER PASS backend=%s typed Storage snapshots, revised owners and rejected suffix retry\n",
        d3d12 ? "D3D12" : "Vulkan");
}

static int sharedRasterTextures(bool d3d12, bool use_materials = false)
{
    static std::array<TextureId, 2> textures;
    static std::array<MaterialId, 2> materials;
    static MaterialProgramId selected_material_program;
    static std::atomic_uint selected_texture{};
    class Producer final : public lux::render::InstanceData
    {
    public:
        struct Configuration final { GraphPassId pass; rdesc::EShaderTarget target; bool materials; };
        struct ResolvedInputs final { DrawStreamSlot stream; rdesc::EShaderTarget target; bool materials; };

        static RenderResult<ResolvedInputs> resolve(const Configuration& input, InstanceLinkContext& context) noexcept
        {
            auto stream = context.resolveDrawStream(input.pass);
            if (!stream) return lux::cxx::unexpected(stream.error());
            return ResolvedInputs{*stream, input.target, input.materials};
        }

        static InstanceDataResult instantiate(const ResolvedInputs& input, InstanceInstantiationContext&) noexcept
        {
            return std::make_unique<Producer>(input);
        }

        explicit Producer(ResolvedInputs input) noexcept : input_(input) {}

        RenderResult<void> selectRasterPrograms(RasterProgramSelection& selection) noexcept override
        {
            RuntimeRasterProgram program;
            if (input_.materials)
            {
                auto lease = selection.materialProgram(input_.stream, selected_material_program);
                if (!lease) return lux::cxx::unexpected(lease.error());
                const auto* shader = (*lease)->find(rdesc::EMaterialPurpose::FORWARD);
                CHECK(shader);
                program.shader = std::make_shared<const rdesc::ShaderProgramArtifact>(
                    prependStreamVertex(shader->shader, kEffectStreamVertex)
                );
                program.parameters.setInlineConstant(rdesc::MaterialGpuReference{});
            }
            else
            {
                program.shader = std::make_shared<const rdesc::ShaderProgramArtifact>(
                    streamShader(input_.target, kEffectStreamSharedTexture)
                );
                program.parameters.setInlineConstant(TestSharedTextureIndices{});
            }
            return selection.setPrograms(input_.stream, std::span{&program, 1});
        }

        RenderResult<void> prepare(FramePreparation& frame) noexcept override
        {
            const auto index = selected_texture.load();
            auto program = frame.rasterProgram(input_.stream, 0);
            if (!program) return lux::cxx::unexpected(program.error());
            if (input_.materials)
            {
                auto value = frame.selectMaterial(input_.stream, *program, materials[index]);
                if (!value) return lux::cxx::unexpected(value.error());
                auto repeated = frame.selectMaterial(input_.stream, *program, materials[index]);
                if (!repeated) return lux::cxx::unexpected(repeated.error());
                const RasterStreamDraw draw{*program, {}, {}, {0, sizeof(*value)}, DrawVertices{12}};
                return frame.setRasterStream(input_.stream, {
                    std::span{&draw, 1}, {}, {}, std::as_bytes(std::span{&*value, 1})
                });
            }
            const auto id = textures[index];
            auto texture = frame.sampleTexture(input_.stream, id);
            if (!texture) return lux::cxx::unexpected(texture.error());
            auto repeated = frame.sampleTexture(input_.stream, id);
            if (!repeated) return lux::cxx::unexpected(repeated.error());
            const TestSharedTextureIndices indices{texture->image_index, texture->sampler_index};
            const RasterStreamDraw draw{*program, {}, {}, {0, sizeof(indices)}, DrawVertices{12}};
            return frame.setRasterStream(input_.stream, {
                std::span{&draw, 1}, {}, {}, std::as_bytes(std::span{&indices, 1})
            });
        }

    private:
        ResolvedInputs input_;
    };

    RenderServerConfig config;
    config.channel_name = use_materials ? "lux.render.test.material-stream" : "lux.render.test.shared-texture";
    config.frames_in_flight = 2;
    config.validation = nativeValidationEnabled();
    config.uploads.emplace();
    auto server = take(RenderServer::create(config, lux::execution::ExecutionRuntime::instance()));
    take(server->registerResource<EffectCaptureBuffer>());
    CapabilityDefinition definition;
    constexpr std::string_view name = "test.shared_raster_texture";
    definition.identity = {graph::nodeTypeId(name), std::string{name}, 1};
    definition.ports = {{node_bytes, "bytes", {"test.readback_bytes", 1}, graph::EPinDirection::OUTPUT}};
    CHECK(server->registerEffectCapability(definition));
    EffectImplementation implementation;
    implementation.capability = definition.identity.id;
    implementation.identity = {"test.shared_raster_texture.draw", 1};
    implementation.supported_outputs = {node_bytes};
    implementation.initialize = [](EffectInitializationContext&, std::string_view) noexcept -> RenderResult<void>
    {
        return {};
    };
    implementation.build = [](EffectBuildContext& context, std::string_view mode) noexcept -> RenderResult<void>
    {
        auto& builder = context.builder();
        const auto readback = take(context.importResource(
            "readback", {ResourceName{EffectCaptureDescriptor::descriptor.type.name()}},
            {"", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION}
        ));
        RasterPassTemplate stage;
        stage.purpose = "shared-texture";
        stage.runtime_stream_programs = true;
        if (mode != "undeclared")
        {
            auto source = context.importRasterTextures("textures");
            if (!source) return lux::cxx::unexpected(source.error());
            stage.shared_textures = *source;
        }
        stage.inputs = {
            {"uTex", rdesc::EDescriptorType::SAMPLED_IMAGE, EGraphUse::SAMPLED, 2},
            {"uTexSamplers", rdesc::EDescriptorType::SAMPLER, EGraphUse::SAMPLED, 2}
        };
        const bool material = mode == "material";
        if (material)
        {
            auto source = context.importRasterMaterials("materials");
            if (!source) return lux::cxx::unexpected(source.error());
            stage.shared_materials = *source;
            stage.inputs.push_back({
                std::string{rdesc::kMaterialTableName}, rdesc::EDescriptorType::STORAGE_BUFFER,
                EGraphUse::STORAGE_READ, 2
            });
        }
        const auto color = take(builder.declareResource({"shared texture output", GraphImageDesc{64, 64}}));
        RasterWork raster;
        raster.program = std::move(stage);
        raster.colors.push_back({{{color, 0}}});
        const auto drawn = take(builder.addRasterPass("sample a shared texture", std::move(raster)));
        auto producer = take(ConfigValue::make(Producer::Configuration{drawn.pass, context.shaderTarget(), material}));
        CHECK(context.addPreparation({InstanceDataFactory::make<Producer>(), std::move(producer), {}, {}, {}, {drawn.pass}}));
        const auto captured = take(builder.addTransferPass("copy texture pixels", TransferWork{{CopyImageBuffer{
            {drawn.written.front()}, {readback}, 256, effect_capture_bytes, ImageCopyRegion{0, 0, 64, 64}
        }}}));
        return context.publish(node_bytes, {{node_bytes, captured.written.front()}});
    };
    const auto selection = implementation.identity;
    CHECK(server->registerEffectImplementation(std::move(implementation)));

    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread worker([&] { CHECK(server->run()); });
    const auto commands = queryCommands(*client);
    DeviceOptions options;
    options.texture_descriptors = true;
    options.descriptor_initial_counts[0] = 2;
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
        device, ViewRequest{scene, view}, EffectCaptureDescriptor::descriptor.type
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
    for (std::size_t i = 0; i < colors.size(); ++i)
        textures[i] = call<TextureCommands::Upload>(*client, commands, {owner, {info, colors[i]}}).texture;

    ResourceInstanceId material_owner;
    MaterialProgramId material_program;
    std::uint32_t material_bytes = 0;
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
        program.texture_count = 1;
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
        selected_material_program = call<MaterialCommands::RegisterProgram>(*client, commands, {
            material_owner, std::move(program)
        }).program;
        for (std::size_t i = 0; i < materials.size(); ++i)
        {
            materials[i] = call<MaterialCommands::CreateInstance>(*client, commands, {
                material_program, {{{0, 0, 0, 1}}}, {textures[i]}
            }).material;
        }
        const std::array updates{
            MaterialColorUpdate{materials[0], 0, {1, 1, 1, 1}}, MaterialColorUpdate{materials[1], 0, {1, 1, 1, 1}}
        };
        call<MaterialCommands::UpdateColor>(*client, commands, {material_owner, updates});
    }

    const auto create_job = [&](std::string mode)
    {
        CreateEffectRequest request;
        request.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*request.graph};
        const auto node = take(edit.addNode(definition, selection, std::move(mode)));
        CHECK(edit.setOutput("bytes", edit.pinId(node, node_bytes)));
        CHECK(edit.commit());
        const auto effect = call<CreateEffect>(*client, commands, std::move(request)).effect;
        return call<CreateRenderJob>(*client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
    };
    JobBinding binding;
    binding.resources.push_back({"readback", readback});
    const auto undeclared = create_job("undeclared");
    call<StartRenderJob>(*client, commands, {device, undeclared, target, binding});
    CHECK(call<RequestRenderJob>(*client, commands, {undeclared}, false).error.type == kRuntimeUnsupported);
    stopAndRemoveJob(*client, commands, undeclared);

    const auto job = create_job(use_materials ? "material" : "");
    CHECK(call<StartRenderJob>(*client, commands, {device, job, target, binding}, false).error.type == kStorageIdentity);
    binding.resources.push_back({"textures", readback, 0, EJobResourceUse::RASTER_TEXTURES});
    CHECK(call<StartRenderJob>(*client, commands, {device, job, target, binding}, false).error.type == kStorageMismatch);
    binding.resources.back().resource = owner;
    if (use_materials)
    {
        binding.resources.push_back({"materials", owner, 0, EJobResourceUse::RASTER_MATERIALS});
        CHECK(call<StartRenderJob>(*client, commands, {device, job, target, binding}, false).error.type == kStorageMismatch);
        binding.resources.back().resource = material_owner;
    }
    call<StartRenderJob>(*client, commands, {device, job, target, binding});

    const auto before = server->developmentStats();
    using Gate = UniformUploadProbe::EPipelineGate;
    if (use_materials) UniformUploadProbe::pipeline_gate = Gate::ARMED;
    UniformUploadProbe::submission_gate = Gate::ARMED;
    auto held_client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(held_client);
    auto held = std::async(std::launch::async, [&]
    {
        return call<RequestRenderJob>(*held_client, commands, {job});
    });
    const auto deadline = Clock::now() + 10s;
    if (use_materials)
    {
        while (UniformUploadProbe::pipeline_gate != Gate::HELD)
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
        const auto borrowed = call<MaterialCommands::UnregisterProgram>(
            *client, commands, {selected_material_program}, false
        );
        UniformUploadProbe::pipeline_gate = Gate::OPEN;
        UniformUploadProbe::pipeline_gate.notify_all();
        CHECK(borrowed.error.type == kDeviceBusy);
    }
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
    const bool exercise_growth = !use_materials && !d3d12 && UniformUploadProbe::can_grow_texture_table;
    std::future<TextureCommands::Upload::Response> growth;
    if (exercise_growth)
    {
        auto growth_client = ipc::IpcCore::instance().createRequester(config.channel_name);
        CHECK(growth_client);
        UniformUploadProbe::watch_texture_pool = true;
        growth = std::async(std::launch::async, [&, requester = std::move(*growth_client)]() mutable
        {
            return call<TextureCommands::Upload>(requester, commands, {owner, {info, colors.front()}});
        });
        const auto growth_deadline = Clock::now() + 10s;
        while (!UniformUploadProbe::texture_pool_grown)
        {
            CHECK(Clock::now() < growth_deadline);
            std::this_thread::yield();
        }
        CHECK(call<TextureCommands::Release>(*client, commands, {textures[0]}, false).error.type == kDeviceBusy);
    }
    UniformUploadProbe::submission_gate = Gate::OPEN;
    UniformUploadProbe::submission_gate.notify_all();
    CHECK(busy.error.type == kDeviceBusy);
    const auto first = held.get();
    if (exercise_growth)
    {
        const auto third = growth.get();
        call<TextureCommands::Release>(*client, commands, {third.texture});
        CHECK(!UniformUploadProbe::pool_destroyed_before_submit);
        UniformUploadProbe::watch_texture_pool = false;
    }
    call<TextureCommands::Release>(*client, commands, {textures[0]});
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
            CHECK(reads->raster_buffer_bytes == before->raster_buffer_bytes + 2 * material_bytes);
        }
    }
    selected_texture = 0;
    CHECK(call<RequestRenderJob>(*client, commands, {job}, false).error.type == kStorageIdentity);
    CHECK(server->stats().submissions == after.submissions);
    CHECK(first.slot_index != second.slot_index);

    stopAndRemoveJob(*client, commands, job);
    call<RemoveView>(*client, commands, {scene, view});
    if (use_materials)
    {
        for (auto material : materials) call<MaterialCommands::ReleaseInstance>(*client, commands, {material});
        call<MaterialCommands::UnregisterProgram>(*client, commands, {material_program});
        call<MaterialCommands::UnregisterProgram>(*client, commands, {selected_material_program});
    }
    call<TextureCommands::Release>(*client, commands, {textures[1]});
    server->stop();
    worker.join();
    CHECK(server->stats().validation_errors == 0);
    const auto& capture = EffectCaptureBuffer::pixels.front();
    CHECK(capture.ready.load() == 1 && capture.frames.size() == 2);
    for (const auto [slot, color] : {std::pair{first.slot_index, colors[0]}, std::pair{second.slot_index, colors[1]}})
    {
        const auto pixel = std::span<const std::byte>{capture.frames[slot]}.subspan(capture.pitch * 32 + 16 * 4, 4);
        CHECK(std::ranges::equal(pixel, color));
    }
    std::printf("%s PASS backend=%s native pixels, deduplicated reads, release protection, growth=%s\n",
        use_materials ? "SHARED MATERIAL" : "SHARED TEXTURE", d3d12 ? "D3D12" : "Vulkan",
        exercise_growth ? "verified" : "not exercised");
    return 0;
}

static void meshRasterStream(bool d3d12)
{
    enum class EDraw { DIRECT, INDIRECT, ARENA, INDEX_WIDTH, VERTEX_LAYOUT, STALE, SHORT_BUFFER, RELEASED };
    constexpr auto arena_bytes = 2 * sizeof(nri::DrawIndexedDesc);
    constexpr std::uint64_t minimum_argument_bytes = 84;
    constexpr std::uint64_t maximum_argument_bytes = 168;
    constexpr std::uint64_t tail_indirect_offset = 120;
    static std::atomic draw_mode{EDraw::DIRECT};
    static std::atomic_uint32_t program_selections;
    static std::array<MeshId, 2> meshes;
    static std::uint32_t vertex_stride;
    class Producer final : public lux::render::InstanceData
    {
    public:
        struct Configuration final
        {
            GraphPassId pass;
            GraphResourceId arguments;
            GraphResourceId arena_arguments;
            GraphStorageSnapshotId instances;
        };

        struct ResolvedInputs final
        {
            DrawStreamSlot stream;
            RasterBufferSlot arguments;
            RasterBufferSlot arena_arguments;
            FrameBufferSizer argument_sizer;
            PreparedBufferWriter arena_writer;
            StorageSnapshotSlot<MeshInstanceSnapshot> instances;
        };

        static RenderResult<ResolvedInputs> resolve(const Configuration& input, InstanceLinkContext& link) noexcept
        {
            return ResolvedInputs{
                take(link.resolveDrawStream(input.pass)),
                take(link.resolveRasterBuffer(input.pass, input.arguments)),
                take(link.resolveRasterBuffer(input.pass, input.arena_arguments)),
                take(link.resolveFrameBuffer(input.arguments)),
                take(link.resolvePreparedBuffer(input.arena_arguments)),
                take(link.resolveStorageSnapshot<MeshInstanceSnapshot>(input.instances))
            };
        }

        static InstanceDataResult instantiate(const ResolvedInputs& input, InstanceInstantiationContext&) noexcept
        {
            return std::make_unique<Producer>(input);
        }

        explicit Producer(ResolvedInputs input) noexcept : input_(input) {}

        RenderResult<void> selectRasterPrograms(RasterProgramSelection& selection) noexcept override
        {
            const auto snapshot = selection.readStorage(input_.instances);
            if (!snapshot) return lux::cxx::unexpected(snapshot.error());
            const auto& instances = snapshot->value.get().instances;
            CHECK(instances.size() == 1);
            const auto& selected = instances.front();
            auto layout = selection.meshLayout(input_.stream, selected.mesh);
            if (!layout) return lux::cxx::unexpected(layout.error());
            const auto material = selection.materialProgramId(input_.stream, selected.material);
            if (!material) return lux::cxx::unexpected(material.error());
            const auto material_program = selection.materialProgram(input_.stream, *material);
            if (!material_program) return lux::cxx::unexpected(material_program.error());
            const auto* fragment = (*material_program)->find(rdesc::EMaterialPurpose::FORWARD);
            CHECK(fragment);
            const auto& position = layout->attributes.front();
            GraphicsState graphics;
            graphics.vertex_bindings = {{0, layout->vertex_stride}};
            graphics.vertex_attributes = {{position.location, 0, position.offset, position.format}};
            ParameterBinding parameters;
            parameters.setInlineConstant(ProducerConstants{});
            auto shader = std::make_shared<const rdesc::ShaderProgramArtifact>(
                prependStreamVertex(fragment->shader, kEffectStreamMesh)
            );
            std::array programs{
                RuntimeRasterProgram{shader, graphics, parameters},
                RuntimeRasterProgram{shader, graphics, parameters}
            };
            programs.back().graphics.vertex_attributes.front().offset += sizeof(float);
            program_selection_revision_ = snapshot->revision;
            ++program_selections;
            return selection.setPrograms(input_.stream, programs);
        }

        RenderResult<void> prepare(FramePreparation& frame) noexcept override
        {
            auto snapshot = frame.readStorage(input_.instances);
            if (!snapshot) return lux::cxx::unexpected(snapshot.error());
            CHECK(snapshot->revision == program_selection_revision_);
            const auto mode = draw_mode.load();
            const auto bytes = mode == EDraw::INDIRECT ? maximum_argument_bytes : minimum_argument_bytes;
            auto sized = frame.sizeBuffer(input_.argument_sizer, bytes);
            if (!sized) return sized;
            auto program = frame.rasterProgram(input_.stream, mode == EDraw::VERTEX_LAYOUT ? 1 : 0);
            if (!program) return lux::cxx::unexpected(program.error());
            std::array<nri::DrawIndexedDesc, 2> commands{};
            if (mode == EDraw::ARENA)
            {
                const ProducerConstants constants{191};
                std::array<RasterStreamVertex, 2> vertices;
                std::array<RasterStreamDraw, 2> draws;
                for (std::uint32_t i = 0; i < draws.size(); ++i)
                {
                    auto mesh = frame.selectMesh(input_.stream, meshes[i]);
                    if (!mesh) return lux::cxx::unexpected(mesh.error());
                    commands[i] = {6, 1, mesh->arena_first_index, mesh->arena_vertex_offset, 0};
                    vertices[i] = {0, mesh->vertices};
                    draws[i] = {
                        *program, {}, {i, 1}, {0, sizeof(constants)}, RasterStreamIndirect{
                            {input_.arena_arguments, i * sizeof(nri::DrawIndexedDesc), sizeof(nri::DrawIndexedDesc)},
                            {},
                            RasterStreamIndexBinding{mesh->indices, mesh->index_type, ERasterIndexOrigin::MESH_ARENA}
                        }
                    };
                }
                auto written = frame.writeBuffer(input_.arena_writer, std::as_bytes(std::span{commands}), 2);
                if (!written) return written;
                return frame.setRasterStream(input_.stream, {
                    draws, {}, vertices, std::as_bytes(std::span{&constants, 1})
                });
            }
            auto written = frame.writeBuffer(input_.arena_writer, std::as_bytes(std::span{commands}), 1);
            if (!written) return written;
            const auto mesh = mode == EDraw::RELEASED ? meshes[0] : snapshot->value.get().instances.front().mesh;
            auto geometry = frame.selectMesh(input_.stream, mesh);
            if (!geometry) return lux::cxx::unexpected(geometry.error());
            if (!original_) original_ = *geometry;
            if (mode == EDraw::STALE) geometry = *original_;
            if (mode == EDraw::INDEX_WIDTH) geometry->index_type = EIndexType::UINT16;

            const RasterStreamVertex vertex{0, geometry->vertices};
            const ProducerConstants constants{191};
            RasterStreamDraw draw{
                *program,
                {}, {0, 1}, {0, sizeof(constants)}, RasterStreamIndexed{geometry->indices, geometry->index_type, 6}
            };
            if (mode == EDraw::INDIRECT || mode == EDraw::SHORT_BUFFER)
            {
                draw.draw = RasterStreamIndirect{
                    {input_.arguments, tail_indirect_offset, sizeof(nri::DrawIndexedDesc)}, {},
                    RasterStreamIndexBinding{geometry->indices, geometry->index_type}
                };
            }
            const std::array draws{draw, draw};
            return frame.setRasterStream(input_.stream, {
                draws, {}, std::span{&vertex, 1}, std::as_bytes(std::span{&constants, 1})
            });
        }

    private:
        ResolvedInputs input_;
        std::optional<RasterMeshGeometry> original_;
        std::uint64_t program_selection_revision_ = 0;
    };

    RenderServerConfig config;
    config.channel_name = "lux.render.test.mesh-stream";
    config.frames_in_flight = 3;
    config.validation = nativeValidationEnabled();
    config.uploads.emplace();
    auto server = take(RenderServer::create(config, lux::execution::ExecutionRuntime::instance()));
    take(server->registerResource<EffectCaptureBuffer>());
    CapabilityDefinition definition;
    constexpr std::string_view name = "test.mesh_raster_stream";
    definition.identity = {graph::nodeTypeId(name), std::string{name}, 1};
    definition.ports = {{node_bytes, "bytes", {"test.readback_bytes", 1}, graph::EPinDirection::OUTPUT}};
    CHECK(server->registerEffectCapability(definition));
    EffectImplementation implementation;
    implementation.capability = definition.identity.id;
    implementation.identity = {"test.mesh_raster_stream.draw", 1};
    implementation.supported_outputs = {node_bytes};
    implementation.initialize = [](EffectInitializationContext&, std::string_view) noexcept -> RenderResult<void>
    {
        return {};
    };
    implementation.build = [](EffectBuildContext& context, std::string_view) noexcept -> RenderResult<void>
    {
        auto& builder = context.builder();
        auto owner = context.importRasterMeshes("meshes");
        if (!owner) return lux::cxx::unexpected(owner.error());
        const auto materials = take(context.importRasterMaterials("materials"));
        const auto instances = take(context.importStorageSnapshot<MeshInstanceSnapshot>(
            "instances", {ResourceName{kInstanceResourceName.name()}}
        ));
        const auto readback = take(context.importResource(
            "readback", {ResourceName{EffectCaptureDescriptor::descriptor.type.name()}},
            {"", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION}
        ));
        const auto arguments = take(context.declareFrameBuffer(
            "mesh indirect arguments", minimum_argument_bytes, maximum_argument_bytes
        ));
        const auto count = take(builder.declareResource({"mesh indirect count scratch", GraphBufferDesc{16}}));
        const auto arena = take(context.declarePreparedBuffer(
            "arena arguments", arena_bytes, arena_bytes, EGraphUse::INDIRECT
        ));
        ComputeWork generate;
        generate.interface.program = take(builder.addProgram(GraphProgram{
            kEffectStreamIndirect.load(context.shaderTarget())
        }));
        generate.interface.resources = {
            {"arguments", {arguments, {}, EGraphAccess::WRITE}},
            {"count", {{count, 0}, {}, EGraphAccess::WRITE}}
        };
        generate.dispatch = DispatchArguments{{1, 1, 1}};
        const auto generated = take(builder.addComputePass("generate mesh draw", std::move(generate)));

        RasterPassTemplate stage;
        stage.purpose = "mesh-stream";
        stage.mesh_geometry = *owner;
        stage.shared_materials = materials;
        stage.runtime_stream_programs = true;
        stage.resources = {
            {.version = generated.written.front(), .usage = EGraphUse::INDIRECT},
            {.version = arena, .usage = EGraphUse::INDIRECT}
        };
        const auto color = take(builder.declareResource({"mesh output", GraphImageDesc{64, 64}}));
        RasterWork raster;
        raster.program = std::move(stage);
        raster.colors.push_back({{{color, 0}}});
        const auto drawn = take(builder.addRasterPass("draw uploaded mesh", std::move(raster)));
        auto producer = take(ConfigValue::make(Producer::Configuration{
            drawn.pass, arguments.resource, arena.resource, instances
        }));
        CHECK(context.addPreparation({
            InstanceDataFactory::make<Producer>(), std::move(producer), {}, {}, {}, {drawn.pass},
            {arguments.resource, arena.resource}, {instances}, {instances}
        }));
        const auto captured = take(builder.addTransferPass("copy mesh pixels", TransferWork{{CopyImageBuffer{
            {drawn.written.front()}, {readback}, 256, effect_capture_bytes, ImageCopyRegion{0, 0, 64, 64}
        }}}));
        return context.publish(node_bytes, {{node_bytes, captured.written.front()}});
    };
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
        device, ViewRequest{scene, view}, EffectCaptureDescriptor::descriptor.type
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
    vertex_stride = take(describeIndexedMesh(mesh)).vertex_stride;
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
    MeshInstanceData selected_instance{meshes[1], material, 0, transform, true, false};
    const auto instance = call<InstanceCommands::CreateInstance>(*client, commands, {instances, selected_instance})
        .instance;
    const auto select_mesh = [&](MeshId selected)
    {
        selected_instance.mesh = selected;
        const MeshInstanceUpdate update{instance, selected_instance};
        call<InstanceCommands::Update>(*client, commands, {instances, std::span{&update, 1}});
    };

    CreateEffectRequest request;
    request.graph = std::make_unique<EffectGraph>();
    EffectGraphEdit edit{*request.graph};
    const auto node = take(edit.addNode(definition, implementation_id));
    CHECK(edit.setOutput("bytes", edit.pinId(node, node_bytes)));
    CHECK(edit.commit());
    const auto effect = call<CreateEffect>(*client, commands, std::move(request)).effect;
    const auto job = call<CreateRenderJob>(*client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
    JobBinding binding;
    binding.resources = {
        {"readback", readback}, {"instances", instances, 0, EJobResourceUse::CPU_SNAPSHOT},
        {"materials", materials, 0, EJobResourceUse::RASTER_MATERIALS},
        {"meshes", readback, 0, EJobResourceUse::RASTER_MESHES}
    };
    CHECK(call<StartRenderJob>(*client, commands, {device, job, target, binding}, false).error.type == kStorageMismatch);
    binding.resources.back().resource = owner;
    using Gate = UniformUploadProbe::EPipelineGate;
    UniformUploadProbe::pipeline_gate = Gate::ARMED;
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
    while (UniformUploadProbe::pipeline_gate != Gate::HELD)
    {
        CHECK(Clock::now() < deadline);
        std::this_thread::yield();
    }
    CHECK(program_selections == 1);
    select_mesh(meshes[0]);
    UniformUploadProbe::pipeline_gate = Gate::OPEN;
    UniformUploadProbe::pipeline_gate.notify_all();
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
    CHECK(program_selections == 2);
    select_mesh(meshes[1]);
    draw_mode = EDraw::INDIRECT;
    const auto second = call<RequestRenderJob>(*client, commands, {job});
    CHECK(program_selections == 3);
    matrix[0] = 2;
    const TransformRecordUpdate transform_update{transform, matrix};
    call<TransformCommands::Update>(*client, commands, {transforms, std::span{&transform_update, 1}});
    const auto indirect_calls = UniformUploadProbe::indexed_indirect_calls.load();
    const auto indirect_draws = UniformUploadProbe::indexed_indirect_draws.load();
    draw_mode = EDraw::ARENA;
    const auto third = call<RequestRenderJob>(*client, commands, {job});
    CHECK(program_selections == 3);
    const auto expected_calls = UniformUploadProbe::can_batch_indexed ? 1u : 2u;
    CHECK(UniformUploadProbe::indexed_indirect_calls == indirect_calls + expected_calls);
    CHECK(UniformUploadProbe::indexed_indirect_draws == indirect_draws + 2);
    CHECK(first.slot_index != second.slot_index && first.slot_index != third.slot_index);
    CHECK(second.slot_index != third.slot_index);
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
    stopAndRemoveJob(*client, commands, job);
    call<RemoveView>(*client, commands, {scene, view});
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
    const auto& shared = capture.frames[third.slot_index];
    CHECK(shared[capture.pitch * 32 + 16 * 4] == std::byte{191});
    CHECK(shared[capture.pitch * 32 + 48 * 4] == std::byte{191});
    std::printf("MESH STREAM PASS backend=%s direct, indirect and arena pixels, indexed arena calls=%u\n",
        d3d12 ? "D3D12" : "Vulkan", expected_calls);
}

static int meshGraphImports(bool d3d12, bool paged, bool visibility, bool gbuffer_visibility)
{
    constexpr auto result_bytes = sizeof(std::uint32_t) * 4;
    static std::atomic_uint64_t observed_revision{};
    static MeshId expected_mesh;
    class Producer final : public lux::render::InstanceData
    {
    public:
        struct Configuration final { GraphStorageSnapshotId instances; GraphResourceId output; };
        struct ResolvedInputs final
        {
            StorageSnapshotSlot<MeshInstanceSnapshot> instances;
            PreparedBufferWriter output;
        };

        static RenderResult<ResolvedInputs> resolve(const Configuration& source, InstanceLinkContext& link) noexcept
        {
            return ResolvedInputs{
                take(link.resolveStorageSnapshot<MeshInstanceSnapshot>(source.instances)),
                take(link.resolvePreparedBuffer(source.output))
            };
        }

        static InstanceDataResult instantiate(const ResolvedInputs& input, InstanceInstantiationContext&) noexcept
        {
            return std::make_unique<Producer>(input);
        }

        explicit Producer(ResolvedInputs input) noexcept : input_(input) {}

        RenderResult<void> prepare(FramePreparation& frame) noexcept override
        {
            auto selected = frame.readStorage(input_.instances);
            if (!selected) return lux::cxx::unexpected(selected.error());
            const auto& snapshot = selected->value.get();
            CHECK(snapshot.revision == selected->revision && snapshot.instances.size() <= 1);
            if (!snapshot.instances.empty()) CHECK(snapshot.instances.front().mesh == expected_mesh);
            observed_revision = selected->revision;
            const auto count = static_cast<std::uint32_t>(snapshot.instances.size());
            return frame.writeBuffer(input_.output, std::as_bytes(std::span{&count, 1}), selected->revision);
        }

    private:
        ResolvedInputs input_;
    };

    RenderServerConfig config;
    config.channel_name = "lux.render.test.mesh-imports";
    config.frames_in_flight = 3;
    config.validation = nativeValidationEnabled();
    config.uploads.emplace();
    auto server = take(RenderServer::create(config, lux::execution::ExecutionRuntime::instance()));
    take(server->registerResource<EffectCaptureBuffer>());
    CapabilityDefinition definition;
    constexpr std::string_view name = "test.mesh_graph_imports";
    definition.identity = {graph::nodeTypeId(name), std::string{name}, 1};
    definition.ports = {{node_bytes, "bytes", {"test.readback_bytes", 1}, graph::EPinDirection::OUTPUT}};
    if (visibility)
    {
        definition.ports.push_back({MeshVisibility::draws, "draws", {std::string{MeshVisibility::draws_type}, 1},
            graph::EPinDirection::INPUT, 1, true});
    }
    CHECK(server->registerEffectCapability(definition));
    EffectImplementation implementation;
    implementation.capability = definition.identity.id;
    implementation.identity = {"test.mesh_graph_imports.read", 1};
    implementation.supported_outputs = {node_bytes};
    implementation.initialize = [](EffectInitializationContext&, std::string_view) noexcept -> RenderResult<void>
    {
        return {};
    };
    implementation.build = [](EffectBuildContext& context, std::string_view mode) noexcept -> RenderResult<void>
    {
        if (mode == "visibility")
        {
            const auto input = context.inputs(MeshVisibility::draws);
            CHECK(input.size() == 1);
            const auto visible = std::ranges::find(input.front(), MeshVisibility::commands, &EffectResourceValue::member);
            CHECK(visible != input.front().end());
            const auto readback = take(context.importResource(
                "readback", {ResourceName{EffectCaptureDescriptor::descriptor.type.name()}},
                {"", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION}
            ));
            constexpr auto bytes = 2 * sizeof(nri::DrawIndexedDesc);
            auto& graph = context.builder();
            const auto initialized = take(graph.addTransferPass("initialize mesh command readback", TransferWork{{
                FillBuffer{{readback}, 0}
            }}));
            const auto capture = take(graph.addTransferPass("capture real mesh commands", TransferWork{{
                CopyBuffer{{visible->version}, {initialized.written.front(), GraphBufferRange{0, bytes}}}
            }}));
            return context.publish(node_bytes, {{node_bytes, capture.written.front()}});
        }
        const bool paged = mode == "paged" || mode == "paged-write";
        const bool writes = mode.ends_with("write");
        auto& builder = context.builder();
        const auto read = [&](std::string_view key, ResourceNameView resource, std::uint64_t bytes, auto projection)
        {
            return context.importResource(key, {ResourceName{resource.name()}}, {
                "", GraphBufferDesc{bytes}, EGraphOrigin::IMPORTED,
                {GraphBufferRange{0, bytes}}, EGraphUse::STORAGE_READ
            }, static_cast<std::uint32_t>(projection));
        };
        const auto transform_role = paged ? ETransformGraphProjection::PAGE_ADDRESSES :
            ETransformGraphProjection::RECORDS;
        const auto transform_bytes = paged ? kTransformInitialPageAddresses * sizeof(std::uint64_t) :
            sizeof(TransformMatrix);
        auto transforms = read("transforms", kTransformResourceName, transform_bytes, transform_role);
        if (!transforms) return lux::cxx::unexpected(transforms.error());
        const auto properties_role = paged ? EMeshInstanceBinding::PAGED_ROOT : EMeshInstanceBinding::PROPERTIES;
        auto properties = read("properties", kInstanceResourceName, meshInstanceProjectionBytes(properties_role),
            properties_role);
        if (!properties) return lux::cxx::unexpected(properties.error());
        const auto alive_role = EMeshInstanceBinding::ALIVE;
        auto alive = paged ? properties : read("alive", kInstanceResourceName, meshInstanceProjectionBytes(alive_role),
            alive_role);
        if (!alive) return lux::cxx::unexpected(alive.error());
        if (writes)
        {
            take(builder.addTransferPass("overwrite storage-owned transform", TransferWork{{
                FillBuffer{{*transforms}, 0}
            }}));
        }
        const auto storage = take(context.importStorageSnapshot<MeshInstanceSnapshot>(
            "instances", {ResourceName{kInstanceResourceName.name()}}
        ));
        const auto snapshot = take(context.declarePreparedBuffer(
            "mesh selection count", sizeof(std::uint32_t), sizeof(std::uint32_t)
        ));
        const auto result = take(builder.declareResource({"verified instance fields", GraphBufferDesc{result_bytes}}));
        const auto readback = take(context.importResource(
            "readback", {ResourceName{EffectCaptureDescriptor::descriptor.type.name()}},
            {"", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION}
        ));
        ComputeWork work;
        const auto& shader = paged ? kEffectMeshImportsPaged : kEffectMeshImports;
        work.interface.program = take(builder.addProgram(GraphProgram{shader.load(context.shaderTarget())}));
        work.interface.resources = {
            {"transforms", {*transforms}}, {"properties", {*properties}}, {"alive", {*alive}},
            {"selections", {snapshot}}, {"result", {{result, 0}, {}, EGraphAccess::WRITE}}
        };
        work.dispatch = DispatchArguments{{1, 1, 1}};
        const auto measured = take(builder.addComputePass("load storage-owned mesh fields", std::move(work)));
        auto producer = take(ConfigValue::make(Producer::Configuration{storage, snapshot.resource}));
        CHECK(context.addPreparation({
            InstanceDataFactory::make<Producer>(), std::move(producer), {}, {}, {}, {},
            {snapshot.resource}, {storage}
        }));
        const auto initialized = take(builder.addTransferPass("initialize readback", TransferWork{{
            FillBuffer{{readback}, 0}
        }}));
        const auto captured = take(builder.addTransferPass("copy mesh fields", TransferWork{{CopyBuffer{
            {measured.written.front()}, {initialized.written.front(), GraphBufferRange{0, result_bytes}}
        }}}));
        return context.publish(node_bytes, {{node_bytes, captured.written.front()}});
    };
    const auto implementation_id = implementation.identity;
    CHECK(server->registerEffectImplementation(std::move(implementation)));

    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread worker([&] { CHECK(server->run()); });
    const auto commands = queryCommands(*client);
    DeviceOptions options;
    options.buffer_device_address = paged;
    if (d3d12) options.backend = EGraphicsBackend::D3D12;
    const auto opened = call<CreateDeviceSession>(*client, commands, options, false);
    if (paged && opened.error.type == kDeviceUnsupported)
    {
        server->stop();
        worker.join();
        std::puts("Paged mesh imports unsupported on this adapter");
        return 77;
    }
    CHECK(!opened.error.type);
    const auto device = opened.device;
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto view = call<CreateView>(*client, commands, {scene}).view;
    const auto target = call<CreateTarget>(*client, commands, {
        device, {64, 64, rdesc::ETextureFormat::RGBA8_UNORM}
    }).target;
    const auto instances = call<GetOrCreateResource>(*client, commands, {device, scene, kInstanceResourceName})
        .resource_id;
    if (paged) EffectCaptureBuffer::watched_instances = instances;
    const auto readback = call<GetOrCreateResource>(*client, commands, {
        device, ViewRequest{scene, view}, EffectCaptureDescriptor::descriptor.type
    }).resource_id;
    const auto transforms = call<QueryResource>(*client, commands, {device, scene, kTransformResourceName}).resource_id;
    const auto meshes =
        call<GetOrCreateResource>(*client, commands, {device, DeviceScope{}, kMeshResourceName}).resource_id;
    const auto materials =
        call<GetOrCreateResource>(*client, commands, {device, DeviceScope{}, kMaterialResourceName}).resource_id;
    rdesc::Mesh mesh;
    mesh.indices = {0, 1, 2};
    mesh.vertices.resize(3);
    for (auto& vertex : mesh.vertices)
    {
        vertex.position.setZero();
        vertex.normal = {0, 0, 1};
        vertex.tangent = {1, 0, 0};
        vertex.bitangent = {0, 1, 0};
        vertex.uv.setZero();
    }
    if (visibility)
    {
        mesh.vertices[0].position = {-.125F, -.125F, .5F};
        mesh.vertices[1].position = {.125F, -.125F, .5F};
        mesh.vertices[2].position = {0, .125F, .5F};
        mesh.bounds = lux::math::AABB{{-.125F, -.125F, .5F}, {.125F, .125F, .5F}};
        call<MeshCommands::UploadMesh>(*client, commands, {meshes, &mesh});
    }
    expected_mesh = call<MeshCommands::UploadMesh>(*client, commands, {meshes, &mesh}).mesh;
    rdesc::MaterialProgramSet program;
    program.target = d3d12 ? rdesc::EShaderTarget::DXIL : rdesc::EShaderTarget::SPIRV;
    program.programs.push_back({rdesc::EMaterialPurpose::FORWARD, "", kEffectStreamInline.load(program.target)});
    const auto registered = call<MaterialCommands::RegisterProgram>(*client, commands, {materials, std::move(program)});
    const auto material = call<MaterialCommands::CreateInstance>(*client, commands, {registered.program, {}, {}}).material;
    auto matrix = kIdentityTransform;
    matrix[0] = 3;
    const auto transform = call<TransformCommands::Create>(*client, commands, {
        transforms, std::span{&matrix, 1}
    }).transforms.front();
    const auto instance = call<InstanceCommands::CreateInstance>(*client, commands, {
        instances, {expected_mesh, material, 0, transform, true, false}
    }).instance;

    const auto create_job = [&](std::string mode)
    {
        CreateEffectRequest request;
        request.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*request.graph};
        const auto node = take(edit.addNode(definition, implementation_id, std::move(mode)));
        if (visibility)
        {
            const auto visibility_definition = gbuffer_visibility ? GBuffer::definition() : MeshVisibility::definition();
            const auto selection = gbuffer_visibility ? ImplementationSelection{std::string{GBuffer::indexed}, GBuffer::version}
                : ImplementationSelection{std::string{MeshVisibility::camera}, MeshVisibility::version};
            const auto visible = take(edit.addNode(
                visibility_definition, selection
            ));
            const auto output = gbuffer_visibility ? GBuffer::draws : MeshVisibility::draws;
            CHECK(edit.connect(edit.pinId(visible, output), edit.pinId(node, MeshVisibility::draws)));
        }
        CHECK(edit.setOutput("bytes", edit.pinId(node, node_bytes)));
        CHECK(edit.commit());
        const auto effect = call<CreateEffect>(*client, commands, std::move(request)).effect;
        return call<CreateRenderJob>(*client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
    };
    const auto transform_role = paged ? ETransformGraphProjection::PAGE_ADDRESSES : ETransformGraphProjection::RECORDS;
    const auto properties_role = paged ? EMeshInstanceBinding::PAGED_ROOT : EMeshInstanceBinding::PROPERTIES;
    JobBinding binding;
    if (visibility)
    {
        binding.view_parameters = call<GetOrCreateResource>(*client, commands, {
            device, ViewRequest{scene, view}, kViewParameterResourceName
        }).resource_id;
        ViewGpuParameters parameters;
        call<ViewParameterCommands::Update>(*client, commands, {binding.view_parameters, std::span{&parameters, 1}});
        binding.resources = {
            {"readback", readback},
            {std::string{MeshVisibility::instances_binding}, instances, 0, EJobResourceUse::CPU_SNAPSHOT},
            {std::string{MeshVisibility::transforms_binding}, transforms, static_cast<std::uint32_t>(transform_role)},
            {std::string{MeshVisibility::properties_binding}, instances, static_cast<std::uint32_t>(properties_role)}
        };
        if (!paged)
        {
            binding.resources.push_back({
                std::string{MeshVisibility::alive_binding}, instances, static_cast<std::uint32_t>(EMeshInstanceBinding::ALIVE)
            });
        }
        const auto job = create_job("visibility");
        auto mirrored_matrix = matrix;
        mirrored_matrix[0] = -mirrored_matrix[0];
        const auto mirrored_transform = call<TransformCommands::Create>(*client, commands, {
            transforms, std::span{&mirrored_matrix, 1}
        }).transforms.front();
        const auto reflected = call<InstanceCommands::CreateInstance>(*client, commands, {
            instances, {expected_mesh, material, 0, mirrored_transform, true, false}
        }).instance;
        call<StartRenderJob>(*client, commands, {device, job, target, binding});
        const auto inside = call<RequestRenderJob>(*client, commands, {job});
        parameters.view[12] = 5;
        call<ViewParameterCommands::Update>(*client, commands, {binding.view_parameters, std::span{&parameters, 1}});
        const auto outside = call<RequestRenderJob>(*client, commands, {job});
        call<InstanceCommands::ReleaseInstance>(*client, commands, {instance});
        call<InstanceCommands::ReleaseInstance>(*client, commands, {reflected});
        const auto empty = call<RequestRenderJob>(*client, commands, {job});
        stopAndRemoveJob(*client, commands, job);
        call<RemoveView>(*client, commands, {scene, view});
        server->stop();
        worker.join();
        CHECK(server->stats().validation_errors == 0);
        const auto& capture = EffectCaptureBuffer::pixels.front();
        CHECK(capture.ready.load() == 1 && capture.frames.size() == 3);
        const auto verify = [&](std::uint32_t slot, std::uint32_t indices, std::uint32_t direct, std::uint32_t mirrored)
        {
            std::array<nri::DrawIndexedDesc, 2> draws{};
            std::memcpy(draws.data(), capture.frames[slot].data(), sizeof(draws));
            CHECK(draws[0].indexNum == indices && draws[1].indexNum == indices);
            CHECK((draws[0].baseIndex > 0) == (indices != 0) && draws[1].baseIndex == draws[0].baseIndex);
            if (draws[0].instanceNum != direct || draws[1].instanceNum != mirrored)
                std::fprintf(stderr, "Visibility slot %u expected %u/%u, got %u/%u\n",
                    slot, direct, mirrored, draws[0].instanceNum, draws[1].instanceNum);
            CHECK(draws[0].instanceNum == direct && draws[1].instanceNum == mirrored);
        };
        verify(inside.slot_index, 3, 1, 1);
        verify(outside.slot_index, 3, 0, 0);
        verify(empty.slot_index, 0, 0, 0);
        return 0;
    }
    binding.resources = {
        {"readback", readback}, {"instances", instances, 0, EJobResourceUse::CPU_SNAPSHOT},
        {"transforms", transforms, static_cast<std::uint32_t>(transform_role)},
        {"properties", instances, static_cast<std::uint32_t>(properties_role)}
    };
    if (!paged) binding.resources.push_back({"alive", instances, static_cast<std::uint32_t>(EMeshInstanceBinding::ALIVE)});
    const auto bad = create_job(paged ? "paged-write" : "write");
    CHECK(call<StartRenderJob>(*client, commands, {device, bad, target, binding}, false).error.type ==
        kGraphInvalidDescriptor);
    call<RemoveRenderJob>(*client, commands, {bad});
    const auto job = create_job(paged ? "paged" : "");
    auto wrong = binding;
    wrong.resources[3].projection = static_cast<std::uint32_t>(
        paged ? EMeshInstanceBinding::PROPERTIES : EMeshInstanceBinding::PAGED_ROOT
    );
    CHECK(call<StartRenderJob>(*client, commands, {device, job, target, wrong}, false).error.type == kStorageMismatch);
    call<StartRenderJob>(*client, commands, {device, job, target, binding});
    const auto first = call<RequestRenderJob>(*client, commands, {job});
    const auto revision = observed_revision.load();
    CHECK(revision != 0);
    matrix[0] = 11;
    const TransformRecordUpdate update{transform, matrix};
    call<TransformCommands::Update>(*client, commands, {transforms, std::span{&update, 1}});
    const auto changed = call<RequestRenderJob>(*client, commands, {job});
    CHECK(observed_revision == revision);
    call<InstanceCommands::ReleaseInstance>(*client, commands, {instance});
    const auto removed = call<RequestRenderJob>(*client, commands, {job});
    CHECK(observed_revision > revision);
    stopAndRemoveJob(*client, commands, job);
    call<RemoveView>(*client, commands, {scene, view});
    server->stop();
    worker.join();
    CHECK(server->stats().validation_errors == 0);
    CHECK(first.slot_index != changed.slot_index && first.slot_index != removed.slot_index);
    CHECK(changed.slot_index != removed.slot_index);
    const auto& capture = EffectCaptureBuffer::pixels.front();
    CHECK(capture.ready.load() == 1 && capture.frames.size() == 3);
    const auto verify = [&](std::uint32_t slot, std::array<std::uint32_t, 4> expected)
    {
        std::array<std::uint32_t, 4> actual;
        std::memcpy(actual.data(), capture.frames[slot].data(), sizeof(actual));
        CHECK(actual == expected);
    };
    verify(first.slot_index, {1, 1, 1, std::bit_cast<std::uint32_t>(3.f)});
    verify(changed.slot_index, {1, 1, 1, std::bit_cast<std::uint32_t>(11.f)});
    verify(removed.slot_index, {});
    if (paged)
    {
        for (const auto& count : EffectCaptureBuffer::published_paged_reads) CHECK(count == 3);
    }
    std::printf("MESH IMPORT PASS backend=%s paged=%u native fields, snapshot revision, and read-only qualification\n",
        d3d12 ? "D3D12" : "Vulkan", static_cast<unsigned>(paged));
    return 0;
}

static void lightingGraphOutput(bool d3d12)
{
    RenderServerConfig config;
    config.channel_name = "lux.render.test.lighting-graph";
    config.frames_in_flight = 2;
    config.validation = nativeValidationEnabled();
    config.uploads.emplace();
    DeviceOptions options;
    if (d3d12) options.backend = EGraphicsBackend::D3D12;
    auto server = take(RenderServer::create(config, lux::execution::ExecutionRuntime::instance()));
    take(server->registerResource<EffectCaptureBuffer>());

    const auto initialize = [](EffectInitializationContext&, std::string_view) noexcept -> RenderResult<void>
    {
        return {};
    };
    CapabilityDefinition source;
    constexpr std::string_view source_name = "test.lighting.gbuffer";
    source.identity = {graph::nodeTypeId(source_name), std::string{source_name}, 1};
    source.ports = {{Lighting::surface, "surface", {std::string{GBufferBundle::type}, GBufferBundle::version},
        graph::EPinDirection::OUTPUT}};
    CHECK(server->registerEffectCapability(source));
    EffectImplementation source_implementation;
    source_implementation.capability = source.identity.id;
    source_implementation.identity = {"test.lighting.gbuffer.clear", 1};
    source_implementation.supported_outputs = {Lighting::surface};
    source_implementation.initialize = initialize;
    source_implementation.build = [](EffectBuildContext& context, std::string_view) noexcept -> RenderResult<void>
    {
        constexpr std::array<std::array<float, 4>, 4> clear{{
            {.75f, .75f, .75f, 0}, {0, 0, 1, .5f}, {0, 0, 0, 1}, {}
        }};
        auto& builder = context.builder();
        EffectResourceBundle surface;
        for (std::size_t i = 0; i < GBufferBundle::members.size(); ++i)
        {
            const auto name = GBufferBundle::members[i];
            const bool depth = i == 3;
            const auto format = depth ? rdesc::GBufferLayout::depth_format : rdesc::GBufferLayout::color_formats[i];
            const auto resource = take(builder.declareResource({std::string{name}, GraphImageDesc{64, 64, 1, 1, format}}));
            const auto pass = take(builder.addTransferPass(std::string{name} + ".clear", TransferWork{{ClearImage{
                {{resource, 0}, GraphImageRange{depth ? 2u : 1u}}, clear[i], .25f
            }}}));
            surface.push_back({effectPortSemantic(name), pass.written.front()});
        }
        return context.publish(Lighting::surface, std::move(surface));
    };
    const auto source_selection = source_implementation.identity;
    CHECK(server->registerEffectImplementation(std::move(source_implementation)));

    CapabilityDefinition shadow_source;
    constexpr std::string_view shadow_name = "test.lighting.shadow";
    shadow_source.identity = {graph::nodeTypeId(shadow_name), std::string{shadow_name}, 1};
    shadow_source.ports = {{Lighting::shadows, "shadows", {std::string{ShadowBundle::type}, ShadowBundle::version},
        graph::EPinDirection::OUTPUT}};
    CHECK(server->registerEffectCapability(shadow_source));
    EffectImplementation shadow_implementation;
    shadow_implementation.capability = shadow_source.identity.id;
    shadow_implementation.identity = {"test.lighting.shadow.occluder", 1};
    shadow_implementation.supported_outputs = {Lighting::shadows};
    shadow_implementation.initialize = initialize;
    shadow_implementation.build = [](EffectBuildContext& context, std::string_view) noexcept -> RenderResult<void>
    {
        using Word = std::pair<std::uint64_t, std::uint32_t>;
        constexpr auto one = std::bit_cast<std::uint32_t>(1.0f);
        constexpr std::array views{Word{0, one}, Word{20, one}, Word{40, one}, Word{60, one}};
        constexpr std::array slices{
            Word{offsetof(ShadowSliceGpu, uv_scale_bias), one},
            Word{offsetof(ShadowSliceGpu, uv_scale_bias) + 4, one},
            Word{offsetof(ShadowSliceGpu, inner_uv) + 8, one},
            Word{offsetof(ShadowSliceGpu, inner_uv) + 12, one},
            Word{offsetof(ShadowSliceGpu, identity) + 4, 1}
        };
        constexpr std::array ranges{Word{sizeof(ShadowMappingHeader) + offsetof(LightShadowRange, count), 1}};
        constexpr std::array<std::uint64_t, 3> sizes{
            sizeof(MeshVisibilityView), sizeof(ShadowSliceGpu), sizeof(ShadowMappingHeader) + sizeof(LightShadowRange)
        };
        const std::array<std::span<const Word>, 3> patches{views, slices, ranges};
        auto& builder = context.builder();
        EffectResourceBundle shadow;
        for (std::size_t i = 0; i < sizes.size(); ++i)
        {
            const std::string name{ShadowBundle::members[i]};
            const auto buffer = take(builder.declareResource({name, GraphBufferDesc{sizes[i]}}));
            const auto initial = take(builder.addTransferPass(
                name + ".zero", TransferWork{{FillBuffer{{{buffer, 0}}, 0}}}
            ));
            TransferWork words;
            for (auto [offset, value] : patches[i])
            {
                words.operations.push_back(FillBuffer{{initial.written.front(), GraphBufferRange{offset, 4}}, value});
            }
            const auto filled = take(builder.addTransferPass(name + ".records", std::move(words)));
            shadow.push_back({effectPortSemantic(name), filled.written.front()});
        }
        const auto depth = take(builder.declareResource({
            "occluder", GraphImageDesc{16, 16, 1, 2, rdesc::ETextureFormat::D32_SFLOAT}
        }));
        const auto cleared = take(builder.addTransferPass(
            "clear occluder", TransferWork{{ClearImage{{{depth, 0}, GraphImageRange{2, 0, 1, 0, 2}}, {}, 0.f}}}
        ));
        shadow.push_back({effectPortSemantic(ShadowBundle::members.back()), cleared.written.front()});
        return context.publish(Lighting::shadows, std::move(shadow));
    };
    const auto shadow_selection = shadow_implementation.identity;
    CHECK(server->registerEffectImplementation(std::move(shadow_implementation)));

    CapabilityDefinition sink;
    constexpr std::string_view sink_name = "test.lighting.readback";
    sink.identity = {graph::nodeTypeId(sink_name), std::string{sink_name}, 1};
    sink.ports = {
        {ToneMapping::color, "color", {std::string{ToneMapping::display_type}, 1}, graph::EPinDirection::INPUT, 1, true},
        {node_bytes, "bytes", {"test.readback_bytes", 1}, graph::EPinDirection::OUTPUT}
    };
    CHECK(server->registerEffectCapability(sink));
    EffectImplementation sink_implementation;
    sink_implementation.capability = sink.identity.id;
    sink_implementation.identity = {"test.lighting.readback.full", 1};
    sink_implementation.supported_outputs = {node_bytes};
    sink_implementation.initialize = initialize;
    sink_implementation.build = [](EffectBuildContext& context, std::string_view) noexcept -> RenderResult<void>
    {
        const auto input = context.inputs(ToneMapping::color);
        CHECK(input.size() == 1 && input.front().size() == 1);
        const ResourceRequirement requirement{ResourceName{EffectCaptureDescriptor::descriptor.type.name()}};
        const auto buffer = take(context.importResource("readback", requirement, {
            "", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION
        }));
        const auto copied = take(context.builder().addTransferPass("copy lighting result", TransferWork{{CopyImageBuffer{
            {input.front().front().version}, {buffer}, 256, effect_capture_bytes, ImageCopyRegion{0, 0, 64, 64}
        }}}));
        return context.publish(node_bytes, {{node_bytes, copied.written.front()}});
    };
    const auto sink_selection = sink_implementation.identity;
    CHECK(server->registerEffectImplementation(std::move(sink_implementation)));

    const auto make_effect = [&](bool with_shadow, std::string_view implementation = Lighting::direct,
                                 std::string grid = {})
    {
        CreateEffectRequest request;
        request.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*request.graph};
        const auto surface = take(edit.addNode(source, source_selection));
        const auto lighting = take(edit.addNode(
            Lighting::definition(), {std::string{implementation}, Lighting::version}, std::move(grid)
        ));
        const auto tone = take(edit.addNode(ToneMapping::definition(), {
            std::string{ToneMapping::linear}, ToneMapping::version
        }, "exposure=0"));
        const auto capture = take(edit.addNode(sink, sink_selection));
        CHECK(edit.connect(edit.pinId(surface, Lighting::surface), edit.pinId(lighting, Lighting::surface)));
        CHECK(edit.connect(edit.pinId(lighting, Lighting::hdr), edit.pinId(tone, ToneMapping::hdr_color)));
        CHECK(edit.connect(edit.pinId(tone, ToneMapping::color), edit.pinId(capture, ToneMapping::color)));
        if (with_shadow)
        {
            const auto shadow = take(edit.addNode(shadow_source, shadow_selection));
            CHECK(edit.connect(edit.pinId(shadow, Lighting::shadows), edit.pinId(lighting, Lighting::shadows)));
        }
        CHECK(edit.setOutput("readback", edit.pinId(capture, node_bytes)));
        CHECK(edit.commit());
        return std::pair{std::move(request), lighting};
    };

    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread worker([&] { CHECK(server->run()); });
    const auto commands = queryCommands(*client);
    auto [request, lighting] = make_effect(false);
    const auto effect = call<CreateEffect>(*client, commands, std::move(request)).effect;
    const auto shadow_effect = call<CreateEffect>(*client, commands, make_effect(true).first).effect;
    const auto clustered_shadow = call<CreateEffect>(*client, commands, make_effect(true, Lighting::clustered).first).effect;
    const auto clustered = call<CreateEffect>(*client, commands,
        make_effect(false, Lighting::clustered, "depth=logarithmic y=5 x=7 index-capacity=65536 z=9").first).effect;
    const auto overflow = call<CreateEffect>(*client, commands,
        make_effect(false, Lighting::clustered, "x=1 y=1 z=1 index-capacity=1 depth=linear").first).effect;
    const auto invalid_grid = call<CreateEffect>(*client, commands,
        make_effect(false, Lighting::clustered, "x=1 y=0 z=1 index-capacity=1 depth=linear").first, false);
    CHECK(invalid_grid.error.type == kInvalidEffectArguments);
    const auto invalid_depth = call<CreateEffect>(
        *client, commands, make_effect(false, Lighting::clustered, "depth=unknown").first, false
    );
    CHECK(invalid_depth.error.type == kInvalidEffectArguments);
    CHECK(invalid_depth.error.args[1] != 0 && invalid_depth.error.args[2] == 0);
    call<SetLightingEnvironment>(*client, commands, {effect, lighting, {}});
    const auto device = call<CreateDeviceSession>(*client, commands, options).device;
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto view = call<CreateView>(*client, commands, {scene}).view;
    const TargetDesc output{64, 64, rdesc::ETextureFormat::RGBA8_UNORM};
    const auto target = call<CreateTarget>(*client, commands, {device, output}).target;
    const auto light_resource = call<GetOrCreateResource>(*client, commands, {device, scene, kLightResourceName}).resource_id;
    DirectionalLightGpu directional;
    directional.radiance = {3, 0, 0, 0};
    const std::array<VDirectLight, 1> authored{directional};
    const auto lights = call<LightingCommands::CreateLights>(*client, commands, {light_resource, {1, authored}});
    CHECK(lights.lights.size() == 1);
    const auto view_resource = [&](ViewId owner, ResourceNameView type)
    {
        return call<GetOrCreateResource>(*client, commands, {device, ViewRequest{scene, owner}, type}).resource_id;
    };
    JobBinding binding;
    binding.view_parameters = view_resource(view, kViewParameterResourceName);
    binding.resources.push_back({"readback", view_resource(view, EffectCaptureDescriptor::descriptor.type)});
    for (std::uint32_t i = 0; i < Lighting::light_bindings.size(); ++i)
        binding.resources.push_back({std::string{Lighting::light_bindings[i]}, light_resource, i});
    auto parameters = ViewGpuParameters{};
    parameters.position_near[2] = 2;
    parameters.direction_far[3] = 4;
    // A forward depth ray runs from world z=1 to z=0, in the view's -Z direction.
    parameters.inverse_view_projection[10] = -1;
    parameters.inverse_view_projection[14] = 1;
    call<ViewParameterCommands::Update>(*client, commands, {binding.view_parameters, std::span{&parameters, 1}});
    const auto job = call<CreateRenderJob>(*client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
    auto malformed = binding;
    malformed.resources[1].projection = static_cast<std::uint32_t>(EDirectLightingBuffer::COUNTS);
    CHECK(call<StartRenderJob>(*client, commands, {device, job, target, malformed}, false).error.type == kStorageMismatch);
    call<StartRenderJob>(*client, commands, {device, job, target, binding});
    const auto red = call<RequestRenderJob>(*client, commands, {job});
    directional.radiance = {0, 0, 3, 0};
    const DirectLightUpdate change{lights.lights.front(), directional};
    call<LightingCommands::Update>(*client, commands, {light_resource, std::span{&change, 1}});
    CHECK(call<SetLightingEnvironment>(*client, commands, {effect, lighting, {-1, 0, 0}}, false).error.type
        == kFrameContract);
    call<SetLightingEnvironment>(*client, commands, {effect, lighting, {0, .5f, 0}});
    const auto blue = call<RequestRenderJob>(*client, commands, {job});
    CHECK(red.slot_index != blue.slot_index);
    stopAndRemoveJob(*client, commands, job);
    call<RemoveView>(*client, commands, {scene, view});
    const auto draw = [&](EffectId selected)
    {
        const auto selected_view = call<CreateView>(*client, commands, {scene}).view;
        binding.view_parameters = view_resource(selected_view, kViewParameterResourceName);
        binding.resources.front().resource = view_resource(selected_view, EffectCaptureDescriptor::descriptor.type);
        call<ViewParameterCommands::Update>(*client, commands, {binding.view_parameters, std::span{&parameters, 1}});
        const auto selected_job = call<CreateRenderJob>(*client, commands, {
            scene, selected_view, selected, EJobCadence::REQUESTED
        }).job;
        call<StartRenderJob>(*client, commands, {device, selected_job, target, binding});
        const auto receipt = call<RequestRenderJob>(*client, commands, {selected_job});
        stopAndRemoveJob(*client, commands, selected_job);
        call<RemoveView>(*client, commands, {scene, selected_view});
        return receipt.slot_index;
    };
    const auto occluded = draw(shadow_effect);
    const auto clustered_occluded = draw(clustered_shadow);
    directional.radiance = {};
    const DirectLightUpdate disable_directional{lights.lights.front(), directional};
    call<LightingCommands::Update>(*client, commands, {light_resource, std::span{&disable_directional, 1}});
    const std::array<VDirectLight, 2> local{
        PointLightGpu{{0, 0, 1.5f, 10}, {0, 2, 0, 0}, {1, 0, 0, 0}},
        SpotLightGpu{{0, 0, 1.5f, 10}, {0, 0, 3, 0}, {0, 0, -1, .65f}, {1, 0, 0, .9f}}
    };
    call<LightingCommands::CreateLights>(*client, commands, {light_resource, {2, local}});
    call<SetLightingEnvironment>(*client, commands, {effect, lighting, {}});
    const auto direct_slot = draw(effect);
    const auto clustered_slot = draw(clustered);
    const auto overflow_slot = draw(overflow);
    server->stop();
    worker.join();
    CHECK(server->stats().validation_errors == 0);
    const auto& pixels = EffectCaptureBuffer::pixels.front();
    CHECK(pixels.ready.load() == 1 && pixels.frames.size() == 2);
    constexpr std::size_t center = 32 * 256 + 32 * 4;
    const auto* first = pixels.frames[red.slot_index].data() + center;
    const auto* second = pixels.frames[blue.slot_index].data() + center;
    CHECK(first[0] > std::byte{100} && first[1] < std::byte{5} && first[2] < std::byte{5});
    CHECK(second[0] < std::byte{5} && second[1] > std::byte{40} && second[2] > std::byte{100});
    CHECK(first[3] == std::byte{255} && second[3] == std::byte{255});
    const auto captured = [](std::size_t index, std::uint32_t slot) noexcept -> std::span<const std::byte>
    {
        const auto& result = EffectCaptureBuffer::pixels[index];
        CHECK(result.ready.load() == 1 && slot < result.frames.size());
        return result.frames[slot];
    };
    for (const auto dark : {captured(1, occluded), captured(2, clustered_occluded)})
    {
        CHECK(dark[center] < std::byte{5} && dark[center + 1] < std::byte{5} && dark[center + 2] < std::byte{5});
        CHECK(dark[center + 3] == std::byte{255});
    }
    const auto reference = captured(3, direct_slot);
    CHECK(reference[center + 1] > std::byte{50} && reference[center + 2] > std::byte{50});
    for (const auto candidate : {captured(4, clustered_slot), captured(5, overflow_slot)})
    {
        CHECK(reference.size() == candidate.size());
        for (std::size_t i = 0; i < reference.size(); ++i)
            CHECK(std::abs(std::to_integer<int>(reference[i]) - std::to_integer<int>(candidate[i])) <= 2);
    }
    std::printf("LIGHTING GRAPH PASS backend=%s typed updates, linked shadow, clustered lists and overflow parity\n",
        d3d12 ? "D3D12" : "Vulkan");
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

    const auto initialize = [](EffectInitializationContext&, std::string_view) noexcept -> RenderResult<void>
    {
        return {};
    };
    CapabilityDefinition depth_source;
    constexpr std::string_view depth_name = "test.depth.clear";
    depth_source.identity = {graph::nodeTypeId(depth_name), std::string{depth_name}, 1};
    depth_source.ports = {{DepthPyramid::depth, "depth", {std::string{DepthPyramid::depth_type}, 1},
        graph::EPinDirection::OUTPUT}};
    CHECK(server->registerEffectCapability(depth_source));
    EffectImplementation clear_depth;
    clear_depth.capability = depth_source.identity.id;
    clear_depth.identity = {"test.depth.clear.fixed", 1};
    clear_depth.supported_outputs = {DepthPyramid::depth};
    clear_depth.initialize = initialize;
    clear_depth.build = [](EffectBuildContext& context, std::string_view) noexcept -> RenderResult<void>
    {
        GraphImageDesc depth{64, 64, 1, 1, rdesc::ETextureFormat::D32_SFLOAT};
        depth.extent.source = EImageExtentSource::TARGET;
        auto& builder = context.builder();
        const auto image = take(builder.declareResource({"forward depth", depth}));
        const auto cleared = take(builder.addTransferPass(
            "clear forward depth", TransferWork{{ClearImage{{{image, 0}, GraphImageRange{2}}, {}, .25f}}}
        ));
        return context.publish(DepthPyramid::depth, {{DepthPyramid::depth, cleared.written.front()}});
    };
    const auto depth_selection = clear_depth.identity;
    CHECK(server->registerEffectImplementation(std::move(clear_depth)));

    CapabilityDefinition read_pyramid;
    static constexpr auto pyramid_input = effectPortSemantic("pyramid");
    constexpr std::string_view read_name = "test.depth.pyramid.readback";
    read_pyramid.identity = {graph::nodeTypeId(read_name), std::string{read_name}, 1};
    read_pyramid.ports = {
        {pyramid_input, "pyramid", {std::string{DepthPyramid::pyramid_type}, 1}, graph::EPinDirection::INPUT, 1, true},
        {node_bytes, "bytes", {"test.readback_bytes", 1}, graph::EPinDirection::OUTPUT}
    };
    CHECK(server->registerEffectCapability(read_pyramid));
    EffectImplementation readback;
    readback.capability = read_pyramid.identity.id;
    readback.identity = {"test.depth.pyramid.readback.texel", 1};
    readback.supported_outputs = {node_bytes};
    readback.initialize = initialize;
    readback.build = [](EffectBuildContext& context, std::string_view key) noexcept -> RenderResult<void>
    {
        const auto source = context.inputs(pyramid_input);
        CHECK(source.size() == 1 && source.front().size() == 1);
        const ResourceRequirement resource{ResourceName{EffectCaptureDescriptor::descriptor.type.name()}};
        const auto buffer = take(context.importResource(key, resource, {
            "", GraphBufferDesc{effect_capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION
        }));
        auto& builder = context.builder();
        const auto setup = take(builder.addTransferPass(
            std::string{key} + ".initialize", TransferWork{{FillBuffer{{buffer}, 0}}}
        ));
        const CopyImageBuffer copy{
            {source.front().front().version, GraphImageRange{1, 3, 1, 0, 1}},
            {setup.written.front(), {}, EGraphAccess::READ_WRITE}, 256, 256, ImageCopyRegion{0, 0, 1, 1}
        };
        const auto done = take(builder.addTransferPass(std::string{key} + ".copy", TransferWork{{copy}}));
        return context.publish(node_bytes, {{node_bytes, done.written.front()}});
    };
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
            DepthPyramid::definition(), {std::string{DepthPyramid::forward_zo}, DepthPyramid::version},
            "mips=" + std::string{mips}
        ));
        CHECK(edit.connect(edit.pinId(depth, DepthPyramid::depth), edit.pinId(pyramid, DepthPyramid::depth)));
        const auto connect = [&](graph::PinSemanticId port, std::string key)
        {
            const auto sink = take(edit.addNode(read_pyramid, read_selection, key));
            CHECK(edit.connect(edit.pinId(pyramid, port), edit.pinId(sink, pyramid_input)));
            CHECK(edit.setOutput(std::move(key), edit.pinId(sink, node_bytes), false));
        };
        connect(DepthPyramid::current, "current");
        if (previous) connect(DepthPyramid::previous, "previous");
        CHECK(edit.commit());
        return result;
    };
    CHECK(call<CreateEffect>(*client, commands, request(true, "0"), false).error.type == kInvalidEffectArguments);
    const auto history = call<CreateEffect>(*client, commands, request(true, "4")).effect;
    const auto current_only = call<CreateEffect>(*client, commands, request(false, "4")).effect;
    const auto session = call<CreateDeviceSession>(*client, commands, options).device;
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto view = call<CreateView>(*client, commands, {scene}).view;
    const TargetDesc target_desc{64, 64, rdesc::ETextureFormat::RGBA8_UNORM};
    const auto target = call<CreateTarget>(*client, commands, {session, target_desc}).target;
    const auto make_capture = [&](ViewId owner)
    {
        return call<GetOrCreateResource>(*client, commands, {
            session, ViewRequest{scene, owner}, EffectCaptureDescriptor::descriptor.type
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
    verifyTonemapOutput(*client, commands, session, scene,
        {rdesc::ETextureFormat::RGBA8_UNORM}, EColorEncoding::LINEAR);
    verifyTonemapOutput(*client, commands, session, scene,
        {rdesc::ETextureFormat::RGBA8_UNORM}, EColorEncoding::SRGB);
    verifyTonemapOutput(*client, commands, session, scene,
        {rdesc::ETextureFormat::RGBA8_SRGB}, EColorEncoding::SRGB);
    verifyTargetInput(*client, commands, session, scene);
    const auto secondary = call<CreateDeviceSession>(*client, commands, options).device;
    verifyTargetInput(*client, commands, secondary, scene);
    std::puts("DEVICE HISTORY PASS: sampled and read back published pixels on both sessions");
    std::printf("TONE MAPPING PASS backend=%s linear, shader sRGB, target sRGB and node exposure\n",
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
        using Ops = void;
        static inline const ResourceDefinition descriptor{ResourceNameView{"test.wsi.failure"}, 0, 0, {}};
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
        return makeResourceRegistration<Descriptor, WsiFailureProbe, &create>(nullptr);
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
    const auto effect = call<CreateEffect>(*client, commands, clearOutputRequest()).effect;
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto view = call<CreateView>(*client, commands, {scene}).view;
    const auto first = startRenderJob(*client, commands, session, scene, view, target.target, effect);
    const auto second = call<CreateRenderJob>(*client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
    CHECK(call<StartRenderJob>(*client, commands, {session, second, target.target}, false).error.type == kTargetInUse);
    const auto layout = call<QueryRenderJobInputLayout>(*client, commands, first).layout;
    auto foreign = take(FrameLayout::create(layout.channels(), layout.revision()));
    CHECK(layout == foreign && !layout.hasSameIdentity(foreign));
    auto stale = take(FrameData::create<std::tuple<>>(foreign));
    CHECK(stale.seal());
    // The packet is qualified after successful Acquire and before the first GPU submission.
    CHECK(call<RequestRenderJob>(*client, commands, {first, &stale}, false).error.type == kFrameContract);
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
    const auto resized = call<ResizeTarget>(*client, commands, {target.target, requested});
    CHECK(resized.output_available && resized.epoch == target.epoch + 1);
    CHECK(resized.description.width == changed_width && resized.description.height == changed_height);
    CHECK(call<SetTargetActive>(*client, commands, {target.target, true, target.epoch}, false).error.type ==
          kRuntimeState);
    call<SetTargetActive>(*client, commands, {target.target, true, resized.epoch});
    call<RebindRenderJobEffect>(*client, commands, {session, second, effect});
    CHECK(call<StopRenderJob>(*client, commands, first, false).error.type == kJobIdentity);
    for (int i = 0; i < 6; ++i) renderFrame(*client, commands, second);
    waitPresented(12);
    const auto submitted = server->stats().submissions;
    CHECK(call<RequestRenderJob>(*client, commands, {second, &stale}, false).error.type == kFrameContract);
    CHECK(server->stats().submissions == submitted);
    stale.release();
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

// A real timeline wait on the retiring GPU. The submitted work cannot retire until the host signals it.
struct RetirementFence final
{
    explicit RetirementFence(DeviceSession& session) noexcept : core(session.nriCore())
    {
        auto& device = session.device();
        auto* native = core.GetDeviceNativeObject(&device);
        if (session.description().graphicsAPI == nri::GraphicsAPI::D3D12)
        {
            auto& dx_device = *static_cast<ID3D12Device*>(native);
            CHECK(SUCCEEDED(dx_device.CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dx_fence))));
            nri::WrapperD3D12Interface wrapper{};
            CHECK(nri::nriGetInterface(device, NRI_INTERFACE(nri::WrapperD3D12Interface), &wrapper) ==
                nri::Result::SUCCESS);
            CHECK(wrapper.CreateFenceD3D12(device, {dx_fence}, fence) == nri::Result::SUCCESS);
            return;
        }
        vk_device = static_cast<VkDevice>(native);
        nri::WrapperVKInterface wrapper{};
        CHECK(nri::nriGetInterface(device, NRI_INTERFACE(nri::WrapperVKInterface), &wrapper) == nri::Result::SUCCESS);
        const auto proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(wrapper.GetDeviceProcAddrVK(device));
        const auto create = reinterpret_cast<PFN_vkCreateSemaphore>(proc(vk_device, "vkCreateSemaphore"));
        destroy_vk = reinterpret_cast<PFN_vkDestroySemaphore>(proc(vk_device, "vkDestroySemaphore"));
        signal_vk = reinterpret_cast<PFN_vkSignalSemaphore>(proc(vk_device, "vkSignalSemaphore"));
        CHECK(create && destroy_vk && signal_vk);
        const VkSemaphoreTypeCreateInfo timeline{
            VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0
        };
        const VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timeline, 0};
        CHECK(create(vk_device, &info, nullptr, &vk_fence) == VK_SUCCESS);
        const nri::FenceVKDesc native_fence{reinterpret_cast<std::uintptr_t>(vk_fence)};
        CHECK(wrapper.CreateFenceVK(device, native_fence, fence) == nri::Result::SUCCESS);
    }

    ~RetirementFence() noexcept
    {
        core.DestroyFence(fence);
        if (dx_fence) dx_fence->Release();
        else destroy_vk(vk_device, vk_fence, nullptr);
    }

    void signal() const noexcept
    {
        if (dx_fence)
        {
            CHECK(SUCCEEDED(dx_fence->Signal(1)));
            return;
        }
        const VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, vk_fence, 1};
        CHECK(signal_vk(vk_device, &info) == VK_SUCCESS);
    }

    const nri::CoreInterface& core;
    nri::Fence* fence = nullptr;
    ID3D12Fence* dx_fence = nullptr;
    VkDevice vk_device = nullptr;
    VkSemaphore vk_fence = VK_NULL_HANDLE;
    PFN_vkDestroySemaphore destroy_vk = nullptr;
    PFN_vkSignalSemaphore signal_vk = nullptr;
};

struct SignalRetirementFence final
{
    static constexpr std::string_view name{"test.device.signal_retirement"};
    using Request = EmptyRequest;
    using Response = AckResponse;
};

struct DeviceRetirementProbe final
{
    struct Descriptor final
    {
        using Config = void;
        using Ops = void;
        static inline const ResourceDefinition descriptor{ResourceNameView{"test.device.retirement"}, 0, 0, {}};
    };

    static inline std::array<nri::AdapterDesc, 3> adapters;
    static inline std::array<bool, 3> present_retirement{};
    static inline std::array<std::atomic<bool>, 3> destroyed{};
    static inline std::size_t created = 0;
    static inline decltype(nri::CoreInterface::QueueSubmit) submit = nullptr;
    static inline decltype(nri::CoreInterface::QueueWaitIdle) wait_idle = nullptr;
    static inline RetirementFence* gate = nullptr;
    static inline std::atomic<bool> arm = false, held = false;
    static inline std::atomic<bool> hold_queue_idle = false, queue_idle_entered = false;
    std::size_t index;
    std::optional<RetirementFence> fence;

    explicit DeviceRetirementProbe(DeviceSession& session) noexcept : index(created++)
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
            if (!arm.exchange(false)) return submit(queue, input);
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
        return std::make_unique<DeviceRetirementProbe>(context.device);
    }

    static ResourceRegistration factory() noexcept
    {
        return makeResourceRegistration<Descriptor, DeviceRetirementProbe, &create>(nullptr);
    }
};

static bool physicalDeviceLifecycle(bool d3d12)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto backend = d3d12 ? "D3D12" : "Vulkan";
    RenderServerConfig config{"lux.render.test.device_lifecycle", nativeValidationEnabled()};
    config.diagnostics.report = [](void*, const RenderDiagnostic& diagnostic) noexcept
    {
        if (diagnostic.severity == ERenderDiagnosticSeverity::ERROR_MESSAGE)
            std::fprintf(stderr, "Device lifecycle diagnostic: %.*s\n",
                int(diagnostic.message.size()), diagnostic.message.data());
    };
    auto server = take(RenderServer::create(config, lux::execution::ExecutionRuntime::instance()));
    registerOutputNodes(*server);
    take(server->registerResource<DeviceRetirementProbe>());
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
    const auto surface_effect = call<CreateEffect>(client, commands, clearOutputRequest()).effect;
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
    const auto job_b = startRenderJob(client, commands, b, scene, view, surface_b.target, surface_effect);
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
    const auto off_job_a = startRenderJob(client, commands, a, scene, view, off_a.target, surface_effect);
    const auto off_job_b = startRenderJob(client, commands, b, scene, view, off_b.target, surface_effect);
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
    const auto unused_job = startRenderJob(
        client, commands, replacement, scene, view, unused_target.target, surface_effect
    );
    const auto layout = call<QueryRenderJobInputLayout>(client, commands, unused_job).layout;
    auto other_layout = take(FrameLayout::create(layout.channels(), layout.revision()));
    auto stale = take(FrameData::create<std::tuple<>>(other_layout));
    CHECK(stale.seal());
    // The frame rejects the stale layout after Acquire and before submitting its acquire wait.
    CHECK(call<RequestRenderJob>(client, commands, {unused_job, &stale}, false).error.type == kFrameContract);
    stale.release();
    call<RemoveTarget>(client, commands, unused_target.target);
    call<RemoveRenderJob>(client, commands, unused_job);
    std::puts("WSI ACQUIRE PASS: removed and reopened the window after an unconsumed Acquire");

    const auto recreated = reopen_b();
    const auto final_job = startRenderJob(client, commands, replacement, scene, view, recreated.target, surface_effect);
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
    if (argc > 1 && std::string_view{argv[1]} == "--shared-texture")
        return sharedRasterTextures(argc > 2 && std::string_view{argv[2]} == "d3d12");
    if (argc > 1 && std::string_view{argv[1]} == "--material-stream")
        return sharedRasterTextures(argc > 2 && std::string_view{argv[2]} == "d3d12", true);
    if (argc > 1 && std::string_view{argv[1]} == "--mesh-stream")
    {
        meshRasterStream(argc > 2 && std::string_view{argv[2]} == "d3d12");
        return 0;
    }
    const auto probe = argc > 1 ? std::string_view{argv[1]} : std::string_view{};
    const bool mesh_visibility = probe == "--mesh-visibility";
    const bool gbuffer_visibility = probe == "--gbuffer-visibility";
    if (probe == "--mesh-imports" || mesh_visibility || gbuffer_visibility)
    {
        const auto mode = argc > 2 ? std::string_view{argv[2]} : std::string_view{};
        return meshGraphImports(mode == "d3d12", mode == "paged", mesh_visibility || gbuffer_visibility, gbuffer_visibility);
    }
    if (argc > 1 && std::string_view{argv[1]} == "--effect-graph")
    {
        const auto mode = argc > 3 ? std::string_view{argv[3]} : std::string_view{};
        const bool uniform_faults = mode == "uniform-faults";
        const bool async_uniform = uniform_faults || mode == "async-uniform";
        effectGraphOutput(argc > 2 && std::string_view{argv[2]} == "d3d12", async_uniform, uniform_faults);
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
    if (argc > 1 && std::string_view{argv[1]} == "--lighting-graph")
    {
        lightingGraphOutput(argc > 2 && std::string_view{argv[2]} == "d3d12");
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
#endif
    Validation validation;
    if (argc > 1 && std::string_view{argv[1]} == "--device-lost")
    {
        deviceLostDelivery(validation, false);
        CHECK(validation.errors == 0);
        return 0;
    }
    {
        graphOutput(validation);
        importedExtentContracts(validation);
        importedIdentityReuse(validation);
        deviceLostDelivery(validation);
    }
    CHECK(validation.errors == 0);
}
