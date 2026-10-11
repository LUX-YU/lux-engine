#include "Allocation.hpp"
#include "F3Tonemap.pass.hpp"
#include "F4Fill.pass.hpp"
#include "F4HostRead.pass.hpp"
#include "NativeGraphSupport.hpp"
#include <lux/engine/render/runtime/NativeAssembly.hpp>
#include <thread>

using namespace native_graph_test;

RuntimeReply await(RuntimeRequest request)
{
    for (unsigned i = 0; i < 2000; ++i)
    {
        auto value = checked(request.poll());
        if (value)
        {
            return *value;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(false);
    return {};
}

void frames(RenderRuntime& runtime, std::uint64_t target)
{
    for (unsigned i = 0; i < 4000; ++i)
    {
        if (runtime.progress().render_frames >= target)
        {
            return;
        }
        if (auto error = runtime.terminalError())
        {
            std::fprintf(stderr, "TERMINAL %llu %llu\n", static_cast<unsigned long long>(error->type), error->args[0]);
            CHECK(false);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(false);
}

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    Validation validation;
    {
        RuntimeNativeOptions native;
        native.instance = {{}, true, diagnostic, &validation};
        native.device.dynamic_rendering = true;
        native.device.multiple_queues = true;
        RuntimeOptions options;
        options.frame_interval_ns = 8'333'333;
        auto assembly = checked(RenderRuntimeBuilder::create(native, options));
        auto& backend = assembly.backend();
        CHECK(checked(backend.retain(checked(Sampler::create(backend.device())))) == GraphSampler{1});
        RenderGraphBuilder graph;
        TextureDesc texture;
        texture.width = texture.height = 4;
        const auto hdr = graph.texture(texture, "runtime.hdr");
        const auto ldr = graph.texture(texture, "runtime.ldr");
        F4Fill fill;
        fill.output.texture = hdr;
        const auto compute = checked(graph.compute("fill", {{"F4Fill", "default"}}, EExecutionScope::SCENE, fill));
        F3Tonemap tonemap;
        tonemap.input.texture = hdr;
        tonemap.nearest.sampler = GraphSampler{1};
        tonemap.output.texture = ldr;
        const auto graphics =
            checked(graph.graphics("tonemap", {{"F3Tonemap", "default"}}, EExecutionScope::VIEW, tonemap));
        F4HostRead read;
        read.source.texture = ldr;
        const auto host = checked(graph.readback("readback", EExecutionScope::VIEW, read));
        CHECK(graph.source(passKey("readback"), "source", PassProducer{passKey("tonemap")}));
        auto definition = checked(std::move(graph).finish());
        auto logical = checked(compileLogicalGraph(definition));
        auto values = makeGraphInvocationData(definition);
        const auto scalar_offset = logical.identity().passes[graphics.value() - 1].scalar_fields[0].offset;
        std::vector<NativeShaderProgram> programs;
        programs.push_back(
            program(backend.device(), logical, compute, "F4Fill", PassSchema<F4Fill>::contract(), argv[1], true)
        );
        programs.push_back(
            program(backend.device(), logical, graphics, "F3Tonemap", PassSchema<F3Tonemap>::contract(), argv[1], false)
        );
        std::vector commands{
            NativePassCommand{compute, DispatchCommand{4, 4, 1}, EQueueRole::COMPUTE},
            NativePassCommand{graphics, DrawCommand{}, EQueueRole::GRAPHICS},
            NativePassCommand{host, CopyCommand{}, EQueueRole::TRANSFER}
        };
        std::array<RuntimeTarget, 4> targets;
        for (auto& target : targets)
        {
            target = checked(backend.addOffscreenTarget(4, 4));
        }
        const auto path = checked(
            backend.addPath(targets[0], {std::move(logical), std::move(programs), std::move(commands), values, {}})
        );
        const auto scene = checked(backend.addScene());
        std::array<RuntimeView, 4> views;
        for (unsigned i = 0; i < views.size(); ++i)
        {
            const float exposure = float(i + 1);
            std::memcpy(
                values.passes[graphics.value() - 1].scalars.data() + scalar_offset,
                &exposure,
                sizeof(exposure)
            );
            values.camera[0] = exposure;
            views[i] = checked(backend.addView(scene, targets[i], path, values));
        }
        auto foreign = scene;
        ++foreign.owner;
        CHECK(!backend.addView(foreign, targets[0], path, values));
        auto runtime = checked(std::move(assembly).finish());
        frames(*runtime, 24);
        const auto first = runtime->progress();
        CHECK(first.native_compilations == 1 && first.native_invocations >= 96 && first.reused_scene_passes >= 72);
        for (unsigned i = 0; i < views.size(); ++i)
        {
            auto result = await(checked(runtime->readback(views[i], host, 0, 0, 16)));
            std::array<float, 4> pixel;
            std::memcpy(pixel.data(), result.bytes.data(), sizeof(pixel));
            const float exposure = float(i + 1);
            const std::array input{1.0f, 2.0f, 3.0f};
            for (unsigned c = 0; c < 3; ++c)
            {
                CHECK(std::abs(pixel[c] - input[c] * exposure / (1 + input[c] * exposure)) < 1e-6f);
            }
            CHECK(pixel[3] == 1);
        }
        // No publications during the measured warmed interval.
        allocation_count = allocation_bytes = 0;
        measuring = true;
        frames(*runtime, runtime->progress().render_frames + 24);
        measuring = false;
        CHECK(allocation_count == 0 && allocation_bytes == 0);
        const auto before_simulation = runtime->progress().render_frames;
        for (unsigned tick = 0; tick < 10; ++tick)
        {
            const float exposure = 2.0f + tick;
            std::memcpy(
                values.passes[graphics.value() - 1].scalars.data() + scalar_offset,
                &exposure,
                sizeof(exposure)
            );
            CHECK(runtime->publishViewPass(
                views[0],
                graphics,
                tick + 1,
                values.camera,
                values.passes[graphics.value() - 1].scalars
            ));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const auto progress = runtime->progress();
        CHECK(progress.render_frames - before_simulation >= 60 && progress.publications == 10);
        CHECK(progress.native_compilations == 1 && progress.waits >= progress.render_frames);
        auto changed = await(checked(runtime->readback(views[0], host, 0, 0, 16)));
        float red;
        std::memcpy(&red, changed.bytes.data(), sizeof(red));
        CHECK(std::abs(red - 11.0f / 12.0f) < 1e-6f);
        for (auto target : targets)
        {
            (void)await(checked(runtime->resize(target, 0, 0)));
        }
        const auto stopped_frames = runtime->progress().render_frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        CHECK(runtime->progress().render_frames == stopped_frames);
        (void)await(checked(runtime->resize(targets[0], 4, 4)));
        frames(*runtime, stopped_frames + 4);
        // Destruction is a real abandon even if completion wins concurrently.
        for (unsigned i = 0; i < 32; ++i)
        {
            auto request = runtime->setActive(views[0], true);
            if (!request)
            {
                CHECK(request.error().type == kTransportCapacity);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        runtime->requestStop();
        CHECK(checked(runtime->waitStopped(5000)));
        CHECK(runtime->state() == ERuntimeState::STOPPED && !runtime->terminalError());
        while (auto error = runtime->pollDiagnostic())
        {
            std::fprintf(stderr, "DIAGNOSTIC %llu\n", static_cast<unsigned long long>(error->type));
            CHECK(false);
        }
        std::printf(
            "RUNTIME frames=%llu simulation_ticks=10 shared_scene=%llu compile=1 heap=0 pixels=PASS stop=PASS\n",
            progress.render_frames,
            progress.reused_scene_passes
        );
    }
    CHECK(validation.errors == 0);
    std::printf("validation_errors=0\n");
}
