#include "Support.hpp"
#include <array>
#include <cstring>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/execution/ExecutionContext.hpp>
#include <lux/engine/render/execution/OffscreenTarget.hpp>
#include <lux/engine/render/graph/Builder.hpp>
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
            session.api().CmdBarrier(command, {nullptr, 0, &source_ready, 1});
            session.api().CmdCopyBuffer(command, output, 0, source, 0, bytes);
            return {};
        }
    ));
}

static void graphOutput(Validation& validation)
{
    auto session = take(DeviceSession::create({}, diagnostics(validation), true, 3, 3));
    const TargetDesc target{16, 16, lux::rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    RenderGraphBuilder builder;
    const auto color = take(builder.declareResource({"color", GraphImageDesc{16, 16}}));
    const auto pixels = take(builder.declareResource({"pixels", GraphBufferDesc{1024}}));
    const auto copied = take(builder.declareResource({"copied", GraphBufferDesc{1024}}));
    take(builder.addPass(
        {"clear",
         EGraphPassKind::TRANSFER,
         {{{color, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}
    ));
    take(builder.addPass(
        {"copy",
         EGraphPassKind::TRANSFER,
         {{{color, 1}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
          {{pixels, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}
    ));
    CHECK(builder.exportOutput({{color, 1}, {}, GraphOutputRequirements{target.format, target.samples}}));
    CHECK(builder.exportOutput({{pixels, 1}}));
    take(builder.addPass(
        {"copy-buffer",
         EGraphPassKind::TRANSFER,
         {{{pixels, 1}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
          {{copied, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}
    ));
    CHECK(builder.exportOutput({{copied, 1}}));
    auto definition = take(std::move(builder).finish());
    auto analysis = take(analyze(definition));
    const std::array programs{PassProgram{"clear", {}, {1, 0, 1, 1}}, PassProgram{"copy"}, PassProgram{"copy-buffer"}};
    auto indexed = take(indexPrograms(definition, programs));
    auto graph = take(compileGraph(
        session->device(),
        session->api(),
        session->enabledFeatures(),
        session->formatSupport(),
        {session->vertexStrideLimit(), session->vertexOffsetLimit()},
        session->queue().properties().type,
        0,
        definition,
        analysis,
        {},
        indexed,
        target
    ));
    auto physical = take(OffscreenTarget::create(*session, target));
    auto execution = take(session->prepare(graph, *physical));
    const std::array contexts{execution.get()};
    std::vector<StorageFrameUse> storage_preparation;
    DeviceSession::collectFrameStorages(contexts, storage_preparation);
    nri::Buffer* output = nullptr;
    const nri::BufferDesc description{1024, 0, nri::BufferUsageBits::NONE};
    CHECK(
        session->api().CreatePlacedBuffer(session->device(), NriHostReadbackHeap, description, output) ==
        nri::Result::SUCCESS
    );
    CHECK(take(session->render(contexts, storage_preparation)) == 1);
    CHECK(take(session->render(contexts, storage_preparation)) == 1);
    CHECK(take(session->render(contexts, storage_preparation)) == 1);
    CHECK(take(session->render(contexts, storage_preparation)) == 0);
    CHECK(session->drain());
    for (auto id : {pixels, copied})
    {
        copyBuffer(
            *session,
            *execution->nativeBuffer(id.index, execution->progress().last_submitted_slot),
            *output,
            1024
        );
        CHECK(session->drain());
        auto* mapped = static_cast<const std::byte*>(session->api().MapBuffer(*output, 0, 1024));
        CHECK(mapped);
        CHECK(session->vk().InvalidateBufferRangeVK(*output, 0, 1024) == nri::Result::SUCCESS);
        for (std::size_t i = 0; i < 1024; i += 4)
        {
            CHECK(mapped[i] == std::byte{255} && mapped[i + 1] == std::byte{0});
            CHECK(mapped[i + 2] == std::byte{255} && mapped[i + 3] == std::byte{255});
        }
        session->api().UnmapBuffer(*output);
    }
    CHECK(take(session->render(contexts, storage_preparation)) == 1);
    execution.reset(); // Physical resources outlive the business Context through Session cleanup.
    physical.reset();
    CHECK(session->drain());
    session->api().DestroyBuffer(output);
    std::puts("Graph output: explicit external copy, correct pixels, Context detached in flight");
}

int main()
{
    Validation validation;
    {
        graphOutput(validation);
    }
    CHECK(validation.errors == 0);
}
