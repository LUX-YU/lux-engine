#include "Support.hpp"
#include "SceneFixture.hpp"
#include <lux/engine/render/native/vulkan/Readback.hpp>

using namespace foundation_test;

namespace
{
    unsigned buffers = 0, images = 0, released_images = 0, draws = 0, barriers = 0;
    bool hold_observation = false;

    void unavailable(void* value, ReadbackOperation::Result result) noexcept
    {
        CHECK(!result && result.error().type == kUnsupported);
        ++*static_cast<unsigned*>(value);
    }
}

namespace lux::render::vulkan::test
{
    VkResult before(const char* label) noexcept
    {
        return hold_observation && std::string_view(label) == "fence_status" ? VK_NOT_READY : VK_SUCCESS;
    }

    VkResult after(const char* label, VkResult result) noexcept
    {
        if (result != VK_SUCCESS) return result;
        const std::string_view name(label);
        if (name == "buffer") ++buffers;
        if (name == "image") ++images;
        if (name == "cmd_draw") ++draws;
        if (name == "cmd_barrier") ++barriers;
        return result;
    }

    void destroyed(const char* label) noexcept
    {
        if (std::string_view(label) == "image") ++released_images;
    }
}

int main()
{
    Validation validation;
    {
        auto bootstrap = instance(validation);
        DeviceOptions options;
        options.dynamic_rendering = true;
        auto session = take(DeviceSession::create(bootstrap, options));
        for (bool graphics : {false, true})
        {
            RenderGraphBuilder builder;
            const auto color = take(builder.declareResource({"color", GraphImageDesc{32, 32}}));
            const auto kind = graphics ? EGraphPassKind::GRAPHICS : EGraphPassKind::TRANSFER;
            const auto usage = graphics ? EGraphUse::COLOR_ATTACHMENT : EGraphUse::TRANSFER_DESTINATION;
            CHECK(builder.addPass({"output", kind, {{{color, 1}, {}, EGraphAccess::WRITE, 0, {}, usage}}}));
            auto target = native_fixture::target({});
            CHECK(builder.exportOutput({{color, 1}, {}, target}));
            auto graph = take(std::move(builder).finish());
            auto analysis = take(analyze(graph));
            PassProgram program{"output"};
            if (graphics)
            {
                program.shader.vertex.assign(native_fixture::vert.begin(), native_fixture::vert.end());
                program.shader.fragment.assign(native_fixture::frag.begin(), native_fixture::frag.end());
            }
            auto code = std::make_shared<CompiledGraph>(take(compileGraph(*session, graph, analysis, {},
                {&program, 1}, target)));
            CHECK(code->resources().size() == 1 && code->passes().size() == 1 && code->hostBarriers().empty());
            CHECK(!code->resources()[0].readback && code->resources()[0].buffer_usage == 0);
            for (bool resized : {false, true})
            {
                if (resized) { target.width = 64; target.height = 16; }
                const auto buffers_before = buffers;
                auto* context = take(session->prepare(code, target, {}));
                CHECK(context->readbackBytes() == 0 && buffers == buffers_before);
                CHECK(!context->lastUse());
                auto bytes = context->readback();
                CHECK(!bytes && bytes.error().type == kUnsupported);
                unsigned completions = 0;
                {
                    ReadbackOperation request(*context, &completions, unavailable, nullptr, nullptr);
                    request.start();
                    CHECK(completions == 1 && !context->lastUse());
                }
                for (unsigned frame = 0; frame != 3; ++frame)
                {
                    const auto prior_barriers = barriers;
                    const auto prior_draws = draws;
                    CHECK(take(context->execute()));
                    CHECK(take(session->queue().wait(*context->lastUse(), UINT64_MAX)));
                    CHECK(barriers == prior_barriers + 1); // Only the color transition; no hidden host/copy barrier.
                    CHECK(draws == prior_draws + unsigned(graphics));
                    CHECK(buffers == buffers_before);
                }
                hold_observation = true;
                CHECK(take(context->execute()));
                const auto ticket = *context->lastUse();
                const auto prior_releases = released_images;
                CHECK(session->release(context));
                CHECK(released_images == prior_releases);
                hold_observation = false;
                CHECK(take(session->queue().wait(ticket, UINT64_MAX)));
                CHECK(session->drain());
                CHECK(released_images == prior_releases + 1);
            }
        }
        CHECK(buffers == 0 && images == 4 && released_images == images);
    }
    CHECK(validation.errors == 0);
    std::printf("Color-only: clear/draw, resize, no buffer/copy/host barrier, explicit readback rejection, "
        "in-flight detach; buffers=%u images=%u released=%u validation_errors=%u\n",
        buffers, images, released_images, validation.errors.load());
}
