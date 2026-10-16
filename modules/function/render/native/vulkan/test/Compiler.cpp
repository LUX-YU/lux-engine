#include "BufferStorageFixture.hpp"
#include "Support.hpp"
#include "SceneFixture.hpp"
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>

using namespace foundation_test;

int main()
{
    Validation validation;
    {
        auto bootstrap = instance(validation);
        DeviceOptions options;
        options.dynamic_rendering = true;
        auto session = take(DeviceSession::create(bootstrap, options));
        CHECK(buffer_fixture::install(*session));
        for (unsigned graphics = 0; graphics < 2; ++graphics)
        {
            const native_fixture::Config config{32, 32, graphics, 1};
            auto assembly = take(native_fixture::assemble(config));
            const auto described = native_fixture::descriptions.load();
            const auto built = native_fixture::builds.load();
            auto programs = native_fixture::programs(config);
            auto compiled = take(compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, programs, native_fixture::target(config)));
            CHECK(compiled.passes().size() == assembly.analysis.execution_order.size());
            CHECK(compiled.passes()[0].pipeline != VK_NULL_HANDLE || graphics == 0);
            CHECK(compiled.hostBarriers().size() == 5);
            CHECK(compiled.resources().size() == 10);
            CHECK(compiled.passes()[1].images[0].native.srcAccessMask ==
                (graphics ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT));
            auto second = take(compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, programs, native_fixture::target(config)));
            CHECK(native_fixture::descriptions == described && native_fixture::builds == built);
            CHECK(second.resources().size() == compiled.resources().size());
            auto wrong_target = native_fixture::target(config);
            wrong_target.samples = 4;
            CHECK(!compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, programs, wrong_target));
            wrong_target = native_fixture::target(config);
            ++wrong_target.width;
            CHECK(!compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, programs, wrong_target));
            auto duplicate = programs;
            duplicate.push_back(programs.front());
            CHECK(!compileGraph(*session, assembly.definition, assembly.analysis, assembly.external_bindings,
                duplicate, native_fixture::target(config)));
            auto unknown = programs;
            unknown.front().pass_key = "unknown.pass";
            CHECK(!compileGraph(*session, assembly.definition, assembly.analysis, assembly.external_bindings,
                unknown, native_fixture::target(config)));
            auto bindings = assembly.external_bindings;
            bindings.front().resource.index = UINT32_MAX;
            CHECK(!compileGraph(*session, assembly.definition, assembly.analysis, bindings,
                programs, native_fixture::target(config)));
            bindings = assembly.external_bindings;
            bindings.push_back(bindings.front());
            CHECK(!compileGraph(*session, assembly.definition, assembly.analysis, bindings,
                programs, native_fixture::target(config)));
            auto missing = programs;
            missing.pop_back();
            CHECK(!compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, missing, native_fixture::target(config)));
            if (graphics)
            {
                auto bad = programs;
                bad[0].shader.interface.bindings = {{0, 0, EDescriptorKind::SAMPLED_IMAGE_ARRAY, 0}};
                CHECK(!compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, bad, native_fixture::target(config)));
                bad = programs;
                bad[0].shader.fragment = bad[0].shader.vertex;
                CHECK(!compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, bad, native_fixture::target(config)));
                bad = programs;
                bad[0].shader.vertex[0] = 0;
                CHECK(!compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, bad, native_fixture::target(config)));
                options.dynamic_rendering = false;
                auto unsupported = take(DeviceSession::create(bootstrap, options));
                CHECK(!compileGraph(*unsupported, (assembly).definition, (assembly).analysis, (assembly).external_bindings, programs, native_fixture::target(config)));
            }
            std::printf("graphics=%u live_passes=%zu resources=%zu G5_callbacks_unchanged=1\n",
                graphics, compiled.passes().size(), compiled.resources().size());
        }
        RenderGraphBuilder builder;
        const auto image = take(builder.declareResource({"color", GraphImageDesc{32, 32}}));
        CHECK(!builder.addPass({"invalid", EGraphPassKind::TRANSFER,
            {{{image, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::COLOR_ATTACHMENT}}}));
        CHECK(!builder.addPass({"invalid", EGraphPassKind::TRANSFER,
            {{{image, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_SOURCE}}}));
        CHECK(builder.passCount() == 0);
    }
    CHECK(validation.errors == 0);
    std::printf("validation_errors=%u warnings=%u\n", validation.errors.load(), validation.warnings.load());
}
