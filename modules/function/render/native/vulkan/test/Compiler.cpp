#include "BufferStorageFixture.hpp"
#include "Support.hpp"
#include "SceneFixture.hpp"
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>

using namespace foundation_test;

namespace
{
    void renameFixtureEntry(SpirvStage& stage, std::string name)
    {
        // Test-only mutation of the checked-in four-character entry literal. The production boundary
        // still runs SPIRV-Tools and SPIRV-Cross against the resulting real binary and declared entry.
        CHECK(name.size() == 4);
        for (std::size_t word = 5; word < stage.words.size();)
        {
            const auto count = stage.words[word] >> 16;
            CHECK(count != 0 && count <= stage.words.size() - word);
            if ((stage.words[word] & 0xffff) == 15) // OpEntryPoint
            {
                CHECK(count >= 5 && stage.words[word + 3] == 0x6e69616d && stage.words[word + 4] == 0);
                stage.words[word + 3] = std::uint32_t(name[0]) | (std::uint32_t(name[1]) << 8) |
                    (std::uint32_t(name[2]) << 16) | (std::uint32_t(name[3]) << 24);
                stage.entry = std::move(name);
                return;
            }
            word += count;
        }
        CHECK(false);
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
            bindings.global.front().resource.index = UINT32_MAX;
            CHECK(!compileGraph(*session, assembly.definition, assembly.analysis, bindings,
                programs, native_fixture::target(config)));
            bindings = assembly.external_bindings;
            bindings.global.push_back(bindings.global.front());
            CHECK(!compileGraph(*session, assembly.definition, assembly.analysis, bindings,
                programs, native_fixture::target(config)));
            auto missing = programs;
            missing.pop_back();
            CHECK(!compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, missing, native_fixture::target(config)));
            if (graphics)
            {
                auto named = programs;
                renameFixtureEntry(named[0].shader.stages[0], "draw");
                renameFixtureEntry(named[0].shader.stages[1], "frag");
                std::ranges::reverse(named[0].shader.stages); // Stage identity is not vector position.
                auto named_code = std::make_shared<CompiledGraph>(take(compileGraph(*session, assembly.definition,
                    assembly.analysis, assembly.external_bindings, named, native_fixture::target(config))));
                StorageSet scene(session->resources()), view(session->resources());
                auto* named_context = take(session->prepare(named_code, native_fixture::target(config), &scene, &view));
                CHECK(take(named_context->execute()));
                CHECK(take(session->queue().wait(*named_context->lastUse(), UINT64_MAX)));
                auto pixels = take(named_context->readback());
                const auto center = (16 * 32 + 16) * 4;
                CHECK(pixels[center] == std::byte{255} && pixels[center + 1] == std::byte{0} &&
                    pixels[center + 2] == std::byte{0} && pixels[center + 3] == std::byte{255});
                CHECK(session->release(named_context));
                named_code.reset();
                for (unsigned mode = 0; mode != 6; ++mode)
                {
                    auto wrong = named;
                    switch (mode)
                    {
                    case 0: wrong[0].shader.stages[0].entry = "main"; break;
                    case 1: wrong[0].shader.stages[0].entry.clear(); break;
                    case 2: wrong[0].shader.stages[0].entry = std::string("frag\0hidden", 11); break;
                    case 3: wrong[0].shader.stages[0].stage = lux::rdesc::EShaderType::COMPUTE; break;
                    case 4: wrong[0].shader.stages[0].stage = wrong[0].shader.stages[1].stage; break;
                    case 5: wrong[0].shader.stages.push_back(wrong[0].shader.stages[0]); break;
                    }
                    CHECK(!compileGraph(*session, assembly.definition, assembly.analysis, assembly.external_bindings,
                        wrong, native_fixture::target(config)));
                }
                auto bad = programs;
                bad[0].shader.interface.bindings = {{0, 0, EDescriptorKind::SAMPLED_IMAGE_ARRAY, 0}};
                CHECK(!compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, bad, native_fixture::target(config)));
                bad = programs;
                bad[0].shader.stages[1].words = bad[0].shader.stages[0].words;
                CHECK(!compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, bad, native_fixture::target(config)));
                bad = programs;
                bad[0].shader.stages[0].words[0] = 0;
                CHECK(!compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, bad, native_fixture::target(config)));
                options.dynamic_rendering = false;
                auto unsupported = take(DeviceSession::create(bootstrap, options));
                CHECK(!compileGraph(*unsupported, (assembly).definition, (assembly).analysis, (assembly).external_bindings, programs, native_fixture::target(config)));
            }
            std::printf("graphics=%u live_passes=%zu resources=%zu G5_callbacks_unchanged=1\n",
                graphics, compiled.passes().size(), compiled.resources().size());
        }
        for (bool export_dead : {false, true})
        {
            RenderGraphBuilder live_builder;
            const native_fixture::Config config{};
            const auto color = take(live_builder.declareResource({"shared:color", GraphImageDesc{32, 32}}));
            const ResolvedResource resolved{"color", {color, 1}};
            CHECK(native_fixture::build(&config, "main", {&resolved, 1}, live_builder));
            const auto history = take(live_builder.declareResource({"dead.history", GraphBufferDesc{16},
                EGraphOrigin::HISTORY_PREVIOUS, {GraphBufferRange{0, 16}}}));
            const auto dead = take(live_builder.declareResource({"dead.image",
                GraphImageDesc{32, 32, 1, 1, lux::rdesc::ETextureFormat::R8_UNORM}}));
            CHECK(live_builder.addPass({"dead.pass", EGraphPassKind::TRANSFER,
                {{{history, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
                 {{dead, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
            if (export_dead) CHECK(live_builder.exportOutput({{dead, 1}}));
            auto graph = take(std::move(live_builder).finish());
            auto analysis = take(analyze(graph));
            auto programs = native_fixture::programs(config);
            PassProgram unused{"dead.pass"};
            unused.shader.stages = {{lux::rdesc::EShaderType::VERTEX, "main", {0}}};
            unused.shader.interface.bindings = {{999, 999, EDescriptorKind::STORAGE_BUFFER, 99, 99}};
            programs.push_back(std::move(unused));
            GraphStorageBindings bindings;
            bindings.global.push_back({history, {StorageName("test.uninstalled.dead")}});
            auto code = compileGraph(*session, graph, analysis, bindings, programs, native_fixture::target(config));
            if (export_dead)
                CHECK(!code); // The same unsupported resource becomes relevant when exported.
            else
            {
                CHECK(code && code->passes().size() == 2 && code->storageGroups().empty());
                CHECK(code->resources()[history.index].buffer_usage == 0 &&
                    code->resources()[dead.index].image_usage == 0);
            }
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
