#include "Support.hpp"
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <array>
#include <charconv>
#include <cstring>
#include <lux/engine/render/execution/OffscreenTarget.hpp>
#include <lux/engine/render/execution/RasterDraws.hpp>
#include <lux/engine/render/execution/RenderGraphInstance.hpp>
#include <lux/engine/render/execution/ResizeResources.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/execution/FrameExecution.hpp>
#include <lux/engine/render/extensions/graph/RenderGraphBuilder.hpp>
#include <lux/engine/render/extensions/storage/ResourceRegistry.hpp>
#include <lux/engine/render/graph/GraphCompiler.hpp>
#include <optional>
#include <string>
using namespace foundation_test;

static void copyBuffer(DeviceSession& session, nri::Buffer& source, nri::Buffer& output, std::uint64_t bytes)
{
    CHECK(session.queue().recordAndSubmit(
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
    auto session = take(DeviceSession::create({}, diagnostics(validation), true, 3));
    auto frames = take(FrameExecution::create(*session, nullptr, 3));
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
    auto graph = take(GraphCompiler::compile(
        session->device(),
        session->nriCore(),
        {&session->queue(EQueueRole::GRAPHICS).properties(),
         &session->queue(EQueueRole::COMPUTE).properties(),
         &session->queue(EQueueRole::TRANSFER).properties()},
        definition,
        analysis,
        &output_description
    ));
    auto ready = stdexec::sync_wait(graph.preparePrograms(
        *session, definition, lux::execution::ExecutionRuntime::instance().cpuScheduler()
    ));
    CHECK(ready && std::get<0>(*ready));
    auto physical = take(OffscreenTarget::create(*session, target, frames->frameCount()));
    auto execution = std::make_unique<RenderGraphInstance>(*session, frames->frameCount(), graph, physical->output());
    CHECK(execution->initialize());
    auto incompatible_output = physical->output();
    incompatible_output.description.format = lux::rdesc::ETextureFormat::BGRA8_UNORM;
    auto rejected_resize = execution->prepareResize(incompatible_output, {});
    CHECK(!rejected_resize && rejected_resize.error().type == kDeviceArgument);
    auto missing_output = execution->prepareResize({});
    CHECK(!missing_output && missing_output.error().type == kDeviceArgument);
    CHECK(execution->target() == target); // Rejection must not withdraw the adopted output.
    const std::array contexts{execution.get()};
    std::vector<StorageFrameUse> storage_preparation;
    nri::Buffer* output = nullptr;
    const nri::BufferDesc description{4096, 0, nri::BufferUsageBits::NONE};
    CHECK(
        session->nriCore().CreatePlacedBuffer(session->device(), NriHostReadbackHeap, description, output) ==
        nri::Result::SUCCESS
    );
    CHECK(take(frames->render(contexts, storage_preparation)) == 1);
    CHECK(take(frames->render(contexts, storage_preparation)) == 1);
    CHECK(take(frames->render(contexts, storage_preparation)) == 1);
    CHECK(take(frames->render(contexts, storage_preparation)) == 0);
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
    CHECK(take(frames->render(contexts, storage_preparation)) == 1);
    execution.reset(); // Physical resources outlive the business Context through Session cleanup.
    physical.reset();
    CHECK(session->drain());
    session->nriCore().DestroyBuffer(output);
    std::puts("Graph output: explicit external copy, correct pixels, Context detached in flight");
}

static void importedIdentityReuse(Validation& validation)
{
    auto session = take(DeviceSession::create({}, diagnostics(validation), true, 3));
    auto frames = take(FrameExecution::create(*session, nullptr, 1));
    RenderGraphBuilder builder;
    const auto color = take(builder.declareResource({"color", GraphImageDesc{16, 16}}));
    RasterWork work;
    work.program = RasterPassTemplate{"identity regression"};
    work.colors = {{{{color, 0}}}};
    const auto pass = take(builder.addRasterPass("clear with dynamic reads", std::move(work)));
    CHECK(builder.exportOutput({pass.written.front()}));
    auto graph = take(std::move(builder).finish());
    auto analysis = take(analyzeRenderGraph(graph));
    auto plan = take(GraphCompiler::compile(
        session->device(),
        session->nriCore(),
        {&session->queue(EQueueRole::GRAPHICS).properties(),
         &session->queue(EQueueRole::COMPUTE).properties(),
         &session->queue(EQueueRole::TRANSFER).properties()},
        graph,
        analysis,
        nullptr
    ));
    auto ready = stdexec::sync_wait(plan.preparePrograms(
        *session, graph, lux::execution::ExecutionRuntime::instance().cpuScheduler()
    ));
    CHECK(ready && std::get<0>(*ready));
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
    instance.emplace(*session, 1, plan);
    CHECK(instance->initialize());
    const auto initial_revision = revision(*instance);
    std::uint64_t creation_revisions = 0;
    {
        RenderGraphInstance probe{*session, 1, plan};
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
        CHECK(take(frames->render(instances, {})) == 1);
        CHECK(!frames->submitting());
        CHECK(session->drain());
    };
    // A viewport can change sampled resources several times, then be detached and recreated
    // at the same allocator address. Local ++ used to collide with the next global identity.
    for (std::uint64_t update = 0; update < 2 * creation_revisions; ++update)
    {
        auto& draws = instance->rasterDraws(pass.pass, 0);
        draws.reads = {{buffer, GraphBufferRange{16 * (update % 2), 16}, EGraphUse::STORAGE_READ, 1}};
        render();
    }
    const auto cached_revision = revision(*instance);
    const auto cached_rebuilds = rebuilds();
    const auto* address = &*instance;
    instance.reset();
    CHECK(session->drain());
    instance.emplace(*session, 1, plan);
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

static void deviceLostDelivery(Validation& validation)
{
    auto session = take(DeviceSession::create({}, diagnostics(validation), true, 2));
    auto& core = const_cast<nri::CoreInterface&>(session->nriCore());
    nri::Buffer* buffer = nullptr;
    const nri::BufferDesc description{16, 0, nri::BufferUsageBits::NONE};
    CHECK(core.CreatePlacedBuffer(session->device(), NriHostReadbackHeap, description, buffer) == nri::Result::SUCCESS);
    auto& queue = session->queue();
    const auto record = [](nri::CommandBuffer&, std::uint32_t) noexcept -> RenderResult<void> { return {}; };
    CHECK(queue.recordAndSubmit(record));
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
    const auto rejected = queue.recordAndSubmit(record);
    core.QueueSubmit = submit;
    CHECK(!rejected && session->deviceLost());
    CHECK(rejected.error().args[0] == static_cast<std::uint32_t>(nri::Result::DEVICE_LOST));
    bool released = false;
    session->deferRelease(queue, [&]() noexcept
    {
        core.DestroyBuffer(buffer);
        released = true;
    });
    const auto drained = session->drain();
    CHECK(!drained && drained.error() == rejected.error());
    CHECK(delivered == rejected.error() && released && !callback.armed());
    session.reset();
    std::puts(
        "DEVICE LOST contract PASS: terminal delivered, cleanup settled; injected only after real GPU completion"
    );
}

static void importedExtentContracts(Validation& validation)
{
    auto session = take(DeviceSession::create({}, diagnostics(validation), true, 1));
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
                "copy", TransferWork{{CopyImage{{{source, 0}}, {{destination, 0}}}}}
            ));
            CHECK(builder.exportOutput({pass.written.front()}));
        }
        auto graph = take(std::move(builder).finish());
        auto analysis = take(analyzeRenderGraph(graph));
        auto plan = take(GraphCompiler::compile(
            session->device(), core,
            {&session->queue(EQueueRole::GRAPHICS).properties(),
             &session->queue(EQueueRole::COMPUTE).properties(),
             &session->queue(EQueueRole::TRANSFER).properties()},
            graph, analysis
        ));
        auto prepared = stdexec::sync_wait(plan.preparePrograms(
            *session, graph, lux::execution::ExecutionRuntime::instance().cpuScheduler()
        ));
        CHECK(prepared && std::get<0>(*prepared));
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
            RenderGraphInstance invalid{*session, 1, plan};
            invalid.bindImage(source, 0, *images[0]);
            invalid.bindImage(destination, 0, *images[1]);
            const auto rejected = invalid.initialize();
            CHECK(!rejected && rejected.error().type == kInvalidTargetExtent);
        }
        {
            RenderGraphInstance instance{*session, 1, plan};
            instance.bindImage(source, 0, *images[0]);
            instance.bindImage(destination, 0, *images[2]);
            CHECK(instance.initialize());
            const std::array<TargetFrame, 1> small{{{images[1]}}};
            const RenderGraphInstance::ImageReplacement replacement{destination, small};
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

int main()
{
    auto execution = lux::execution::ExecutionRuntime::create({2, 128, {128}});
    CHECK(execution);
    Validation validation;
    {
        graphOutput(validation);
        importedExtentContracts(validation);
        importedIdentityReuse(validation);
        deviceLostDelivery(validation);
    }
    CHECK(validation.errors == 0);
}
