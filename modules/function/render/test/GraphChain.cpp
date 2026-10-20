#include "Support.hpp"
#include <cstring>
#include <fstream>
#include <lux/engine/render/execution/FrameExecution.hpp>
#include <lux/engine/render/execution/OffscreenTarget.hpp>
#include <lux/engine/render/execution/RenderGraphInstance.hpp>
#include <lux/engine/render/graph/GraphCompiler.hpp>
#include <lux/engine/render/graph/RenderGraphBuilder.hpp>
#include <lux/engine/render/graph/RenderPlanDump.hpp>
#include <string>

#include "GraphCount.hpp"
#include "GraphFill.hpp"
#include "GraphMrt.hpp"
#include "GraphPresent.hpp"
#include "GraphRed.hpp"
#include "GraphTriangle.hpp"

using namespace foundation_test;
namespace rdesc = lux::rdesc;

static rdesc::SpirvArtifact program(
    rdesc::EShaderType type,
    std::span<const std::uint32_t> words,
    std::span<const std::byte> reflection
)
{
    rdesc::SpirvArtifact result;
    result.stages.push_back({type, "main", {words.begin(), words.end()}});
    CHECK(rdesc::ShaderInfo::deserialize(reflection, result.interface));
    return result;
}

static rdesc::SpirvArtifact rasterProgram(
    std::span<const std::uint32_t> fragment,
    std::span<const std::byte> reflection
)
{
    auto result = program(rdesc::EShaderType::FRAGMENT, fragment, reflection);
    auto vertex = program(rdesc::EShaderType::VERTEX, kGraphTriangle, kGraphTriangleReflection);
    result.stages.insert(result.stages.begin(), std::move(vertex.stages.front()));
    result.interface.entry_points.insert(
        result.interface.entry_points.begin(),
        vertex.interface.entry_points.begin(),
        vertex.interface.entry_points.end()
    );
    return result;
}

static RenderPlan compile(
    DeviceSession& session,
    const RenderGraph& graph,
    const RenderGraphAnalysis& analysis,
    const TargetDesc* target
)
{
    return take(GraphCompiler::compile(
        session.device(),
        session.nriCore(),
        session.enabledFeatures(),
        session.formatSupport(),
        {session.vertexStrideLimit(), session.vertexOffsetLimit()},
        session.queue().properties().type,
        0,
        0,
        graph,
        analysis,
        target
    ));
}

static ResourceVersion written(const PassResult& pass, GraphResourceId resource)
{
    const auto found = std::ranges::find(pass.written, resource, &ResourceVersion::resource);
    CHECK(found != pass.written.end());
    return *found;
}

static void checkPixels(
    DeviceSession& session,
    nri::Buffer& input,
    std::uint32_t width,
    std::uint32_t offset,
    std::array<unsigned, 4> expected,
    std::uint32_t row_pitch = 0
)
{
    const auto row = row_pitch ? row_pitch
                               : (width * 4 + session.description().memoryAlignment.uploadBufferTextureRow - 1) /
                                     session.description().memoryAlignment.uploadBufferTextureRow *
                                     session.description().memoryAlignment.uploadBufferTextureRow;
    const auto bytes = std::uint64_t(row) * width + offset;
    nri::Buffer* mapped_buffer = nullptr;
    CHECK(
        session.nriCore().CreatePlacedBuffer(
            session.device(),
            NriHostReadbackHeap,
            {bytes, 0, nri::BufferUsageBits::NONE},
            mapped_buffer
        ) == nri::Result::SUCCESS
    );
    CHECK(session.queue().recordAndSubmit(
        [&](nri::CommandBuffer& command, std::uint32_t) noexcept -> RenderResult<void>
        {
            const nri::BufferBarrierDesc barrier{
                &input,
                {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY},
                {nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY}
            };
            session.nriCore().CmdBarrier(command, {nullptr, 0, &barrier, 1});
            session.nriCore().CmdCopyBuffer(command, *mapped_buffer, 0, input, 0, bytes);
            return {};
        }
    ));
    CHECK(session.drain());
    const auto* data = static_cast<const unsigned char*>(session.nriCore().MapBuffer(*mapped_buffer, 0, bytes));
    CHECK(data);
    CHECK(session.invalidateMappedRange(*mapped_buffer, 0, bytes) == nri::Result::SUCCESS);
    for (std::uint32_t y = 1; y < width - 1; ++y)
    {
        for (std::uint32_t x = 1; x < width - 1; ++x)
        {
            for (unsigned channel = 0; channel < 4; ++channel)
            {
                const auto actual = data[offset + y * row + x * 4 + channel];
                if (std::abs(int(actual) - int(expected[channel])) > 1)
                {
                    std::fprintf(
                        stderr,
                        "pixel (%u,%u) channel=%u actual=%u expected=%u\n",
                        x,
                        y,
                        channel,
                        actual,
                        expected[channel]
                    );
                    CHECK(false);
                }
            }
        }
    }
    session.nriCore().UnmapBuffer(*mapped_buffer);
    session.nriCore().DestroyBuffer(mapped_buffer);
}

static void imageChain(Validation& validation, std::uint32_t fif)
{
    auto session = take(DeviceSession::create({}, diagnostics(validation), true, 3));
    auto frames = take(FrameExecution::create(*session, nullptr, fif));
    RenderGraphBuilder builder;
    GraphImageDesc image{64, 64};
    image.extent.target_relative = true;
    auto seed_description = image;
    seed_description.mips = 2;
    const auto seed = take(builder.declareResource({"seed", seed_description}));
    const auto first = take(builder.declareResource({"first", image}));
    const auto second = take(builder.declareResource({"second", image}));
    const auto final = take(builder.declareResource({"final", image}));
    auto depth_description = image;
    depth_description.format = rdesc::ETextureFormat::D32_SFLOAT;
    const auto depth = take(builder.declareResource({"depth", depth_description}));
    const auto tint = take(builder.declareResource({"tint", GraphBufferDesc{16}}));
    const auto pixels = take(builder.declareResource({"pixels", GraphBufferDesc{128 * 512 + 256}}));
    const auto tint_pass = take(builder.addTransferPass("tint", TransferWork{{FillBuffer{{{tint, 0}}, 0x3f800000u}}}));
    ComputeWork compute;
    compute.interface.program =
        take(builder.addProgram(program(rdesc::EShaderType::COMPUTE, kGraphFill, kGraphFillReflection)));
    compute.interface.resources = {{"generated", {{seed, 0}, GraphImageRange{1, 1, 1}, EGraphAccess::WRITE}}};
    compute.dispatch = DispatchArguments{{8, 8, 1}};
    const auto generated = take(builder.addComputePass("generate mip 1", std::move(compute)));
    RasterWork mrt;
    mrt.interface.program = take(builder.addProgram(rasterProgram(kGraphMrt, kGraphMrtReflection)));
    mrt.interface.resources = {
        {"source_image", {generated.written.front(), GraphImageRange{1, 1, 1}}},
        {"tint", {tint_pass.written.front()}}
    };
    mrt.interface.samplers = {{"source_sampler", {0}}};
    mrt.colors = {{{{first, 0}}}, {{{second, 0}}}};
    mrt.depth = RasterAttachment{{{depth, 0}, GraphImageRange{2}}};
    mrt.graphics.depth_test = mrt.graphics.depth_write = true;
    const auto raster = take(builder.addRasterPass("MRT and depth", std::move(mrt)));
    RasterWork present;
    present.interface.program = take(builder.addProgram(rasterProgram(kGraphPresent, kGraphPresentReflection)));
    present.interface.resources = {
        {"colors", {written(raster, first)}, {{written(raster, second)}}},
        {"depth_image", {written(raster, depth), GraphImageRange{2}}}
    };
    present.interface.samplers = {{"source_sampler", {0}}};
    present.colors = {{{{final, 0}}}};
    const auto resolved = take(builder.addRasterPass("postprocess", std::move(present)));
    const auto copied = take(builder.addTransferPass(
        "copy result",
        TransferWork{{CopyImageBuffer{{resolved.written.front()}, {{pixels, 0}, GraphBufferRange{256, 128 * 512}}}}}
    ));
    const TargetDesc target{64, 64, image.format, 1, ETargetKind::OFFSCREEN};
    CHECK(builder.exportOutput({resolved.written.front(), {}, GraphOutputRequirements{target.format, 1}}));
    CHECK(builder.exportOutput({copied.written.front(), GraphBufferRange{256, 128 * 512}}));
    auto graph = take(std::move(builder).finish());
    auto analysis = take(analyzeRenderGraph(graph));
    auto plan = compile(*session, graph, analysis, &target);
    std::string dump;
    const GraphDumpSink sink{
        &dump,
        [](void* pointer, std::string_view data) noexcept
        {
            static_cast<std::string*>(pointer)->append(data);
            return true;
        }
    };
    CHECK(dumpRenderGraph(graph, analysis, EGraphDumpFormat::JSON, sink));
    CHECK(dumpRenderPlan(plan, EGraphDumpFormat::JSON, sink));
    CHECK(dump.find("\"mip\":1") != std::string::npos);
    CHECK(dump.find("\"colors\":2") != std::string::npos);
    nri::Descriptor* sampler = nullptr;
    nri::SamplerDesc sampler_description{};
    sampler_description.addressModes =
        {nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::CLAMP_TO_EDGE};
    CHECK(session->nriCore().CreateSampler(session->device(), sampler_description, sampler) == nri::Result::SUCCESS);
    auto physical = take(OffscreenTarget::create(*session, target, fif));
    auto instance = std::make_unique<RenderGraphInstance>(*session, fif, plan, physical.get());
    CHECK(instance->bindSampler(0, *sampler));
    CHECK(instance->initialize());
    CHECK(instance->dumpResources(sink));
    CHECK(dump.find("\"allocation_requirement_bytes\":") != std::string::npos);
    if (const auto* path = std::getenv("LUX_GRAPH_DUMP"))
    {
        std::ofstream output(path, std::ios::app);
        output << dump << '\n';
        CHECK(output.good());
    }
    auto larger_target = target;
    larger_target.width = larger_target.height = 128;
    CHECK(plan.compatible(larger_target));
    auto larger = take(OffscreenTarget::create(*session, larger_target, fif));
    auto second_instance = std::make_unique<RenderGraphInstance>(*session, fif, plan, larger.get());
    CHECK(second_instance->bindSampler(0, *sampler));
    CHECK(second_instance->initialize());
    const std::array instances{instance.get(), second_instance.get()};
    for (std::uint32_t slot = 0; slot < fif; ++slot)
    {
        CHECK(take(frames->render(instances, {})) == 1);
    }
    CHECK(take(frames->render(instances, {})) == 0);
    CHECK(session->drain());
    for (std::uint32_t slot = 0; slot < fif; ++slot)
    {
        checkPixels(*session, *instance->nativeBuffer(pixels.index, slot), 64, 256, {64, 64, 64, 255});
        checkPixels(*session, *second_instance->nativeBuffer(pixels.index, slot), 128, 256, {64, 64, 64, 255});
    }
    CHECK(take(frames->render(instances, {})) == 1);
    instance.reset();
    physical.reset();
    CHECK(session->drain());
    const std::array remaining{second_instance.get()};
    CHECK(take(frames->render(remaining, {})) == 1);
    second_instance.reset();
    larger.reset();
    CHECK(session->drain());
    session->nriCore().DestroyDescriptor(sampler);
}

static void indirectCounts(Validation& validation)
{
    auto session = take(DeviceSession::create({}, diagnostics(validation), true, 3));
    auto frames = take(FrameExecution::create(*session, nullptr, 3));
    RenderGraphBuilder builder;
    const auto commands = take(builder.declareResource({"commands", GraphBufferDesc{60}}));
    const auto count = take(builder.declareResource({"count", GraphBufferDesc{4}}));
    const auto indices = take(builder.declareResource({"indices", GraphBufferDesc{12}}));
    const auto color = take(builder.declareResource({"color", GraphImageDesc{32, 32}}));
    const auto pixels = take(builder.declareResource({"pixels", GraphBufferDesc{8192}}));
    const auto parameter =
        take(builder.declareResource({"requested count", GraphBufferDesc{4}, EGraphOrigin::IMPORTED}));
    const auto dispatch_arguments = take(builder.declareResource({"dispatch arguments", GraphBufferDesc{12}}));
    const auto dispatch_ready =
        take(builder.addTransferPass("dispatch dimensions", TransferWork{{FillBuffer{{{dispatch_arguments, 0}}, 1}}}));
    ComputeWork compute;
    compute.interface.program =
        take(builder.addProgram(program(rdesc::EShaderType::COMPUTE, kGraphCount, kGraphCountReflection)));
    compute.interface.resources = {
        {"commands", {{commands, 0}, {}, EGraphAccess::WRITE}},
        {"count", {{count, 0}, {}, EGraphAccess::WRITE}},
        {"indices", {{indices, 0}, {}, EGraphAccess::WRITE}}
    };
    compute.interface.parameters.values.resize(4);
    compute.interface.parameters.fields.push_back({"requested", parameter, "requested"});
    compute.dispatch = IndirectDispatchArguments{dispatch_ready.written.front(), 0};
    const auto generated = take(builder.addComputePass("GPU commands and count", std::move(compute)));
    RasterWork draw;
    draw.interface.program = take(builder.addProgram(rasterProgram(kGraphRed, kGraphRedReflection)));
    draw.colors = {{{{color, 0}}, EAttachmentLoad::CLEAR, EAttachmentStore::STORE, {0, 0, 0, 1}}};
    draw.draw = DrawIndexedIndirect{
        written(generated, indices),
        0,
        EIndexType::UINT32,
        written(generated, commands),
        0,
        3,
        20,
        written(generated, count),
        0
    };
    const auto drawn = take(builder.addRasterPass("indirect count", std::move(draw)));
    const auto copied = take(builder.addTransferPass(
        "copy result",
        TransferWork{{CopyImageBuffer{{drawn.written.front()}, {{pixels, 0}}, 256, 8192}}}
    ));
    CHECK(builder.exportOutput({copied.written.front()}));
    auto graph = take(std::move(builder).finish());
    auto analysis = take(analyzeRenderGraph(graph));
    auto plan = compile(*session, graph, analysis, nullptr);
    auto instance = std::make_unique<RenderGraphInstance>(*session, 3, plan);
    std::uint32_t requested_value = 0;
    rdesc::ShaderBlock
        parameter_layout{4, 4, rdesc::EDataLayout::STD430, {{"requested", rdesc::EDataScalar::UINT, 0, 1, 1, 0, 0}}};
    instance->setResource(
        parameter.index,
        UINT32_MAX,
        {nullptr, 0, 0, std::as_bytes(std::span(&requested_value, 1)), &parameter_layout}
    );
    CHECK(instance->initialize());
    const std::array instances{instance.get()};
    for (std::uint32_t requested : {0u, 1u, 3u})
    {
        requested_value = requested;
        CHECK(take(frames->render(instances, {})) == 1);
        CHECK(session->drain());
        checkPixels(
            *session,
            *instance->nativeBuffer(pixels.index, instance->progress().last_submitted_slot),
            32,
            0,
            {requested ? 255u : 0u, 0, 0, 255},
            256
        );
    }
    instance.reset();
    CHECK(session->drain());
}

static void resolveAndCopy(Validation& validation)
{
    auto session = take(DeviceSession::create({}, diagnostics(validation), true, 3));
    auto frames = take(FrameExecution::create(*session, nullptr, 2));
    RenderGraphBuilder builder;
    GraphImageDesc multisample{32, 32};
    multisample.samples = 4;
    const auto source = take(builder.declareResource({"MSAA", multisample}));
    const auto resolved = take(builder.declareResource({"resolve", GraphImageDesc{32, 32}}));
    const auto rectangle = take(builder.declareResource({"rectangle", GraphImageDesc{16, 16}}));
    const auto roundtrip = take(builder.declareResource({"roundtrip", GraphImageDesc{16, 16}}));
    const auto packed = take(builder.declareResource({"packed", GraphBufferDesc{4096}}));
    const auto pixels = take(builder.declareResource({"pixels", GraphBufferDesc{4352}}));
    RasterWork draw;
    draw.interface.program = take(builder.addProgram(rasterProgram(kGraphRed, kGraphRedReflection)));
    RasterAttachment attachment{{{source, 0}}};
    attachment.resolve = ResourceAccess{{resolved, 0}};
    draw.colors.push_back(attachment);
    const auto raster = take(builder.addRasterPass("multisample resolve", std::move(draw)));
    const auto clear =
        take(builder.addTransferPass("initialize rectangle", TransferWork{{ClearImage{{{rectangle, 0}}}}}));
    const auto copied = take(builder.addTransferPass(
        "copy rectangle",
        TransferWork{{CopyImage{
            {written(raster, resolved)},
            {clear.written.front()},
            ImageCopyRegion{8, 8, 16, 16},
            ImageCopyRegion{0, 0, 16, 16}
        }}}
    ));
    const auto stored = take(builder.addTransferPass(
        "texture to buffer",
        TransferWork{{CopyImageBuffer{{copied.written.front()}, {{packed, 0}}, 256, 4096}}}
    ));
    const auto uploaded = take(builder.addTransferPass(
        "buffer to texture",
        TransferWork{{CopyImageBuffer{{stored.written.front()}, {{roundtrip, 0}}, 256, 4096}}}
    ));
    const auto observed = take(builder.addTransferPass(
        "result",
        TransferWork{{CopyImageBuffer{{uploaded.written.front()}, {{pixels, 0}, GraphBufferRange{256, 4096}}, 256, 4096}
        }}
    ));
    CHECK(builder.exportOutput({observed.written.front(), GraphBufferRange{256, 4096}}));
    auto graph = take(std::move(builder).finish());
    auto analysis = take(analyzeRenderGraph(graph));
    auto plan = compile(*session, graph, analysis, nullptr);
    auto instance = std::make_unique<RenderGraphInstance>(*session, 2, plan);
    CHECK(instance->initialize());
    const std::array instances{instance.get()};
    CHECK(take(frames->render(instances, {})) == 1);
    CHECK(session->drain());
    checkPixels(*session, *instance->nativeBuffer(pixels.index, 0), 16, 256, {255, 0, 0, 255}, 256);
    instance.reset();
    CHECK(session->drain());
}

int main()
{
    Validation validation;
    for (std::uint32_t fif : {1u, 2u, 3u})
    {
        imageChain(validation, fif);
    }
    indirectCounts(validation);
    resolveAndCopy(validation);
    CHECK(validation.errors == 0);
    std::printf(
        "Graph: Compute -> mip -> MRT/Depth -> sampled postprocess; FIF=1/2/3; GPU IndirectCount=0/1/3; errors=%u "
        "warnings=%u\n",
        validation.errors.load(),
        validation.warnings.load()
    );
}
