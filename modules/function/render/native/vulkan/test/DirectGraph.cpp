#include "Support.hpp"
#include <lux/engine/render/graph/Builder.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>

int main()
{
    using namespace foundation_test;
    Validation validation;
    {
        auto bootstrap = instance(validation);
        auto session = take(DeviceSession::create(bootstrap));
        const TargetDesc target{16, 16, lux::rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
        RenderGraphBuilder builder;
        const auto color = take(builder.declareResource({"color", GraphImageDesc{16, 16}}));
        const auto pixels = take(builder.declareResource({"pixels", GraphBufferDesc{1024}}));
        take(builder.addPass({"clear", EGraphPassKind::TRANSFER,
            {{{color, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
        take(builder.addPass({"readback", EGraphPassKind::TRANSFER,
            {{{color, 1}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
             {{pixels, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
        CHECK(builder.exportOutput({{color, 1}, {}, target}));
        CHECK(builder.exportOutput({{pixels, 1}}));
        auto definition = take(std::move(builder).finish());
        auto analysis = take(analyze(definition));
        const std::array programs{PassProgram{"clear", {}, {1, 0, 1, 1}}, PassProgram{"readback"}};
        auto graph = std::make_shared<CompiledGraph>(take(compileGraph(
            *session, definition, analysis, {}, programs, target
        )));
        CHECK(graph->passes().size() == 2);
        auto scene = take(session->resources().createDomain(session->resources().root(), EResourceScope::SCENE));
        auto view = take(session->resources().createDomain(scene, EResourceScope::VIEW));
        auto* execution = take(session->prepare(graph, target, view));
        CHECK(take(execution->execute()));
        CHECK(take(session->queue().wait(*execution->lastUse(), UINT64_MAX)));
        auto output = take(execution->readback());
        CHECK(output.size() == 1024);
        std::fprintf(stderr, "direct graph RGBA=%u,%u,%u,%u\n", std::to_integer<unsigned>(output[0]),
            std::to_integer<unsigned>(output[1]), std::to_integer<unsigned>(output[2]), std::to_integer<unsigned>(output[3]));
        for (std::size_t i = 0; i < output.size(); i += 4)
        {
            CHECK(output[i] == std::byte{255});
            CHECK(output[i + 1] == std::byte{0});
            CHECK(output[i + 2] == std::byte{255});
            CHECK(output[i + 3] == std::byte{255});
        }
    }
    CHECK(validation.errors == 0);
    std::printf("Direct Graph Definition/Analysis to Native compiler and GPU readback; no Feature dependency\n");
}
