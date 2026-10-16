#include "Support.hpp"
#include "SceneFixture.hpp"
#include "shaders/Swizzle.hpp"
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <fstream>

using namespace foundation_test;

namespace
{
    enum class ECase { LOAD, CLEAR, OTHER_ATTACHMENT, TRANSFER_READ, CONDITIONAL };
    ECase selected_case = ECase::LOAD; // only synchronous test-owned assembly, no production state
    struct Counts { unsigned pipeline = 0, bind = 0, begin = 0, end = 0, barrier = 0, draw = 0; } counts;
    unsigned live_pipelines = 0;
    constexpr FeatureTypeIdView type("test.native.plan");

    RenderResult<void> describe(const RetainableEncodedDataView*, std::string_view, std::vector<ResourceClaim>&) noexcept { return {}; }

    RenderResult<void> build(const RetainableEncodedDataView*, std::string_view name,
        std::span<const ResolvedResource>, RenderGraphBuilder& builder) noexcept
    {
        const auto key = [&](std::string_view local) { return featureKey(type, name, local); };
        const auto first = take(builder.declareResource({key("first"), GraphImageDesc{32, 32}}));
        auto color = first;
        CHECK(builder.addPass({key("a"), EGraphPassKind::GRAPHICS,
            {{{first, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::COLOR_ATTACHMENT}}, true}));
        if (selected_case == ECase::OTHER_ATTACHMENT)
            color = take(builder.declareResource({key("other"), GraphImageDesc{32, 32}}));
        if (selected_case == ECase::TRANSFER_READ)
        {
            const auto snapshot = take(builder.declareResource({key("snapshot"), GraphBufferDesc{4096}}));
            CHECK(builder.addPass({key("snapshot"), EGraphPassKind::TRANSFER,
                {{{first, 1}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
                 {{snapshot, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
            CHECK(builder.exportOutput({{snapshot, 1}}));
        }
        const bool other = selected_case == ECase::OTHER_ATTACHMENT;
        const bool clear = selected_case == ECase::CLEAR || other;
        const bool conditional = selected_case == ECase::CONDITIONAL;
        const std::uint32_t version = other ? 1 : 2;
        GraphResourceUse use{{color, version}, {}, clear ? EGraphAccess::WRITE : EGraphAccess::ATTACHMENT_LOAD,
            clear ? 0u : 1u, {}, EGraphUse::COLOR_ATTACHMENT};
        if (conditional) use.fallback = GraphVersion{color, 1};
        CHECK(builder.addPass({key("b"), EGraphPassKind::GRAPHICS, {use}, false, conditional}));
        const auto pixels = take(builder.declareResource({key("pixels"), GraphBufferDesc{4096}}));
        CHECK(builder.addPass({key("copy"), EGraphPassKind::TRANSFER,
            {{{color, version}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
             {{pixels, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
        CHECK(builder.exportOutput({{color, version}, {}, native_fixture::target({})}));
        return builder.exportOutput({{pixels, 1}});
    }

    const FeatureDescriptor definition{type, {1, 0}, nullptr};
    const FeatureGraphOps definition_graph{describe, build};

    FeatureAssembly assembly(ECase value)
    {
        selected_case = value;
        const FeatureInstanceView selected{&definition, &definition_graph, nullptr, "main"};
        return take(assembleFeatures({&selected, 1}, {4096, 4096}));
    }

    std::vector<PassProgram> programs(bool different)
    {
        const SpirvArtifact shader{{native_fixture::vert.begin(), native_fixture::vert.end()},
            {native_fixture::frag.begin(), native_fixture::frag.end()}, {}};
        std::vector<PassProgram> result;
        result.push_back({featureKey(type, "main", "a"), shader, {0.125f, 0.25f, 0.5f, 1}});
        result.push_back({featureKey(type, "main", "b"), shader, {0, 1, 0, 1}});
        if (different) result.back().shader.fragment.assign(native_fixture::swizzle.begin(), native_fixture::swizzle.end());
        result.push_back({featureKey(type, "main", "copy")});
        if (selected_case == ECase::TRANSFER_READ) result.push_back({featureKey(type, "main", "snapshot")});
        return result;
    }

    void pixel(std::span<const std::byte> bytes, unsigned x, unsigned y, std::array<int, 4> expected)
    {
        for (unsigned c = 0; c < 4; ++c)
            CHECK(std::abs(std::to_integer<int>(bytes[(y * 32 + x) * 4 + c]) - expected[c]) <= 1);
    }
}

namespace lux::render::vulkan::test
{
    VkResult before(const char*) noexcept { return VK_SUCCESS; }

    VkResult after(const char* name, VkResult result) noexcept
    {
        if (result != VK_SUCCESS) return result;
        const std::string_view operation(name);
        if (operation == "pipeline") { ++counts.pipeline; ++live_pipelines; }
        if (operation == "cmd_bind") ++counts.bind;
        if (operation == "cmd_begin") ++counts.begin;
        if (operation == "cmd_end") ++counts.end;
        if (operation == "cmd_barrier") ++counts.barrier;
        if (operation == "cmd_draw") ++counts.draw;
        return result;
    }

    void destroyed(const char* name) noexcept
    {
        if (std::string_view(name) == "pipeline") { CHECK(live_pipelines != 0); --live_pipelines; }
    }
}

int main()
{
    Validation validation;
    std::ofstream raw("native-binding-plan.csv");
    raw << "case,different_shader,pipelines,bind,begin,end,barrier,draw\n";
    {
        auto instance = foundation_test::instance(validation);
        DeviceOptions options;
        options.dynamic_rendering = true;
        auto session = take(DeviceSession::create(instance, options));
        for (auto mode : {ECase::LOAD, ECase::CLEAR, ECase::OTHER_ATTACHMENT, ECase::TRANSFER_READ})
            for (bool different : {false, true})
            {
                counts = {};
                auto input = assembly(mode);
                auto code = std::make_shared<CompiledGraph>(take(compileGraph(*session, (input).definition, (input).analysis, (input).external_bindings, programs(different),
                    native_fixture::target({}))));
                auto* context = take(session->prepare(code, native_fixture::target({}), {}));
                context->setColor({1, 0, 0, 1});
                CHECK(take(context->execute()));
                CHECK(take(session->queue().wait(*context->lastUse(), UINT64_MAX)));
                auto bytes = take(context->readback());
                pixel(bytes, 16, 16, different ? std::array{0, 0, 255, 255} : std::array{255, 0, 0, 255});
                const bool clears = mode == ECase::CLEAR || mode == ECase::OTHER_ATTACHMENT;
                pixel(bytes, 0, 0, clears ? std::array{0, 255, 0, 255} : std::array{32, 64, 128, 255});
                CHECK(counts.pipeline == (different ? 2u : 1u) && counts.bind == counts.pipeline);
                CHECK(counts.draw == 2 && counts.begin == (mode == ECase::LOAD ? 1u : 2u));
                CHECK(counts.end == counts.begin);
                CHECK(counts.barrier == (mode == ECase::LOAD ? 3u : mode == ECase::TRANSFER_READ ? 5u : 4u));
                raw << unsigned(mode) << ',' << different << ',' << counts.pipeline << ',' << counts.bind << ','
                    << counts.begin << ',' << counts.end << ',' << counts.barrier << ',' << counts.draw << '\n';
                std::printf("case=%u different=%u pipelines=%u bind=%u begin/end=%u/%u barrier=%u draw=%u\n",
                    unsigned(mode), different, counts.pipeline, counts.bind, counts.begin, counts.end,
                    counts.barrier, counts.draw);
                CHECK(session->release(context));
                CHECK(session->drain());
                code.reset();
                CHECK(live_pipelines == 0);
            }
        auto conditional = assembly(ECase::CONDITIONAL);
        auto rejected = compileGraph(*session, (conditional).definition, (conditional).analysis, (conditional).external_bindings, programs(false), native_fixture::target({}));
        CHECK(!rejected && rejected.error().type == kUnsupported && live_pipelines == 0);
    }
    CHECK(validation.errors == 0 && live_pipelines == 0);
    std::printf("Validation/SyncValidation errors=%u; conditional refused, shared Pipeline destroyed exactly once\n",
        validation.errors.load());
}
