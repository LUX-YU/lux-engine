#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RenderGraphCompiler.hpp>

#include <cassert>
#include <cstdio>
#include <sstream>
#include <type_traits>

// Exercise the actual cache commit policy, without exporting its private implementation.
#include "../src/scene/SceneGraphCache.cpp"

namespace
{
    using namespace lux::render;

    template <class T>
    concept HasValidityFlag = requires(T& value) { value.valid; };
    template <class T>
    concept HasCompileError = requires(T& value) { value.compile_error; };
    static_assert(!HasValidityFlag<RGCompiledGraph> && !HasCompileError<RGCompiledGraph>);
    static_assert(!std::is_copy_constructible_v<RGCompileResult>);

    template <class Error> void reject(RGBuilder builder, PipelineManager& manager, std::size_t pass_count)
    {
        auto result = RenderGraphCompiler::compile(std::move(builder).build(), manager);
        assert(!result && isError<Error>(result.error().cause));
        assert(result.error().candidate.original_graph.passes.size() == pass_count);
        auto retained = std::move(result.error().candidate);
        assert(retained.original_graph.passes.size() == pass_count);
    }

    void testCompiler(PipelineManager& manager)
    {
        {
            RGBuilder builder;
            builder.addPass("first", ERGPassType::TRANSFER).after("second");
            builder.addPass("second", ERGPassType::TRANSFER).after("first");
            reject<err::graph::DependencyCycle>(std::move(builder), manager, 2);
        }
        {
            RGBuilder builder;
            (void)builder.importTexture("missing native source", RGTextureDescription{}, {});
            reject<err::graph::ImportedResourceIncomplete>(std::move(builder), manager, 0);
        }
        {
            RGBuilder builder;
            (void)builder.referenceTexture("missing producer", ERGReference::REFERENCE_REQUIRED);
            reject<err::graph::ReferencedResourceHasNoProducer>(std::move(builder), manager, 0);
        }
        {
            RGBuilder builder;
            (void)builder.addPass("missing compute pipeline", ERGPassType::COMPUTE);
            reject<err::graph::ComputePassMissingPipeline>(std::move(builder), manager, 1);
        }
        {
            RGBuilder builder;
            builder.addPass("stale compute pipeline", ERGPassType::COMPUTE).setComputePipeline({123});
            reject<err::graph::ComputePassPipelineStale>(std::move(builder), manager, 1);
        }
        {
            RGBuilder builder;
            const auto texture = builder.createTexture("owned diagnostic", RGTextureDescription{});
            builder.addPass("warning before failure", ERGPassType::COMPUTE)
                .write(texture, ETextureRole::UNORDERED_ACCESS);
            auto result = RenderGraphCompiler::compile(std::move(builder).build(), manager);
            assert(!result && isError<err::graph::ComputePassMissingPipeline>(result.error().cause));
            assert(!result.error().candidate.diagnostics.empty());
            auto retained = std::move(result.error().candidate);
            assert(retained.original_graph.resources[0].name == "owned diagnostic");
            assert(isError<err::graph::UsageUnderdeclared>(retained.diagnostics[0]));
            assert(retained.compiled_passes[0].pass == &retained.original_graph.passes[0]);
            std::ostringstream diagnostic;
            printCompiledGraph(retained, diagnostic);
            assert(diagnostic.str().find("owned diagnostic") != std::string::npos);
        }
        {
            RGBuilder builder;
            builder.addPass("retained side effect", ERGPassType::TRANSFER).markSideEffect();
            auto result = RenderGraphCompiler::compile(std::move(builder).build(), manager);
            assert(result.has_value());
            assert(result->execution_order.size() == 1);
            auto retained = std::move(*result);
            assert(retained.compiled_passes[0].pass == &retained.original_graph.passes[0]);
        }
        {
            RGBuilder builder;
            const auto buffer = builder.createBuffer("cross queue", RGBufferDescription{.size = 64});
            builder.addPass("async writer", ERGPassType::ASYNC_TRANSFER).write(buffer, ERGBufferRole::STORAGE);
            builder.addPass("consumer", ERGPassType::TRANSFER).read(buffer, ERGBufferRole::STORAGE).markSideEffect();
            reject<err::graph::CrossQueueTransferRequired>(std::move(builder), manager, 2);
        }
        {
            RGBuilder builder;
            const auto buffer = builder.createBuffer("conditional write", RGBufferDescription{.size = 64});
            builder.addPass("conditional writer", ERGPassType::TRANSFER)
                .write(buffer, ERGBufferRole::STORAGE)
                .setCondition([] { return false; });
            builder.addPass("unconditional reader", ERGPassType::TRANSFER)
                .read(buffer, ERGBufferRole::STORAGE)
                .markSideEffect();
            reject<err::graph::ConditionalPassWritesUnconditionalRead>(std::move(builder), manager, 2);
        }
        {
            RGBuilder builder;
            const auto texture = builder.createTexture(
                "invalid format",
                RGTextureDescription::Absolute(1, 1, lux::rdesc::ETextureFormat::UNDEFINED)
            );
            builder.addPass("invalid attachment", ERGPassType::GRAPHICS).write(texture);
            reject<err::graph::AttachmentFormatUnknown>(std::move(builder), manager, 1);
        }
        {
            RGBuilder builder;
            (void)builder.addPass("no attachment", ERGPassType::GRAPHICS);
            reject<err::graph::AttachmentPlanInvalid>(std::move(builder), manager, 1);
        }
        {
            RGBuilder builder;
            const auto texture = builder.createTexture("conditional only", RGTextureDescription{});
            builder.addPass("conditional group", ERGPassType::GRAPHICS)
                .write(texture)
                .setCondition([] { return false; })
                .markSideEffect();
            reject<err::graph::AllConditionalPassGroup>(std::move(builder), manager, 1);
        }
        {
            RGBuilder builder;
            const auto first = builder.createTexture("conditional clear", RGTextureDescription{});
            const auto second = builder.createTexture(
                "local read output",
                RGTextureDescription::Absolute(1, 1, lux::rdesc::ETextureFormat::RGBA16_SFLOAT)
            );
            builder.addPass("first clear", ERGPassType::GRAPHICS)
                .write(first)
                .setCondition([] { return false; })
                .markSideEffect();
            builder.addPass("unconditional sibling", ERGPassType::GRAPHICS)
                .write(first)
                .after("first clear")
                .markSideEffect();
            builder.addPass("separate scopes", ERGPassType::TRANSFER).after("unconditional sibling").markSideEffect();
            builder.addPass("later writer", ERGPassType::GRAPHICS)
                .write(first)
                .after("separate scopes")
                .markSideEffect();
            builder.addPass("local reader", ERGPassType::GRAPHICS)
                .inputRead(first, 0)
                .write(second)
                .after("later writer")
                .markSideEffect();
            reject<err::graph::ConditionalPassOwnsClear>(std::move(builder), manager, 5);
        }
        {
            auto lifetime = std::make_shared<int>(7);
            const std::weak_ptr<int> observed = lifetime;
            RGBuilder builder;
            builder.addPass("own rejected callbacks", ERGPassType::COMPUTE)
                .setCondition([lifetime] { return *lifetime == 7; });
            lifetime.reset();
            {
                auto result = RenderGraphCompiler::compile(std::move(builder).build(), manager);
                assert(!result && !observed.expired());
                auto retained = std::move(result);
                assert(!observed.expired());
                assert(retained.error().candidate.original_graph.passes[0].condition());
            }
            assert(observed.expired());
        }
    }

    void testLastGood(RenderContext& context)
    {
        SceneGraphCache cache(context, "compiler result qualification");
        RenderTargetLayout layout;
        layout.slots[0] = defaultTargetSlotDesc(ETargetSlot::SCENE_COLOR);
        layout.slots[0]->format = lux::rdesc::ETextureFormat::RGBA8_UNORM;
        cache.compile(layout, 0, {}, {}, nullptr, 1);
        const auto* original = cache.state().graph.get();
        assert(original && cache.state().valid);
        const auto color = cache.state().final_color_handle;
        auto candidate_layout = layout;
        candidate_layout.slots[0]->format = lux::rdesc::ETextureFormat::RGBA16_SFLOAT;
        cache.compile(
            candidate_layout,
            0,
            [](RGBuilder& builder) { (void)builder.addPass("reject replacement", ERGPassType::COMPUTE); },
            {},
            nullptr,
            2
        );
        assert(cache.state().graph.get() == original && cache.state().valid);
        assert(cache.state().last_layout == layout && cache.state().final_color_handle.index == color.index);
        assert(cache.telemetry().compile_attempts == 2 && cache.telemetry().compile_failures == 1);
        assert(!cache.compileHistory().back().succeeded);
        cache.compile(candidate_layout, 0, {}, {}, nullptr, 3);
        assert(cache.state().graph && cache.state().graph.get() != original);
        assert(cache.state().last_layout == candidate_layout && cache.telemetry().compile_successes == 2);
        cache.collectRetired(3, 0);
        cache.collectRetired(4, 3);
    }
} // namespace

int main()
{
    using namespace lux::render;
    auto instance = InstanceContext::create({});
    assert(instance);
    auto device = DeviceContext::create(**instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
    assert(device);
    auto resources = ResourceContext::create(**device);
    auto layouts = GeneralDescriptorSetLayout::create(**device);
    assert(resources && layouts);
    RenderContext::CreateInfo info{
        std::make_unique<PipelineManager>(**device, true),
        std::move(*layouts),
        std::make_unique<ResourceRegistry>(),
        2
    };
    auto context = RenderContext::create(**resources, std::move(info));
    assert(context);
    testCompiler((*context)->pipelineManager());
    testLastGood(**context);
    std::puts("Actual compiler errors/owned diagnostics/moved graph and last-good cache passed");
}
