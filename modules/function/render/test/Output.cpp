#include "Support.hpp"
#include <array>
#include <cstring>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/execution/FrameExecution.hpp>
#include <lux/engine/render/execution/OffscreenTarget.hpp>
#include <lux/engine/render/execution/RenderGraphInstance.hpp>
#include <lux/engine/render/graph/GraphCompiler.hpp>
#include <lux/engine/render/graph/RenderGraphBuilder.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
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
    auto graph = take(GraphCompiler::compile(
        session->device(),
        session->nriCore(),
        session->enabledFeatures(),
        session->formatSupport(),
        {session->vertexStrideLimit(), session->vertexOffsetLimit()},
        {&session->queue(EQueueRole::GRAPHICS).properties(),
         &session->queue(EQueueRole::COMPUTE).properties(),
         &session->queue(EQueueRole::TRANSFER).properties()},
        0,
        0,
        definition,
        analysis,
        &target
    ));
    auto physical = take(OffscreenTarget::create(*session, target, frames->frameCount()));
    auto execution = std::make_unique<RenderGraphInstance>(*session, frames->frameCount(), graph, physical.get());
    CHECK(execution->initialize());
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

int main()
{
    Validation validation;
    {
        graphOutput(validation);
    }
    CHECK(validation.errors == 0);
}
