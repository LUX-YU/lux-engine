#include "Allocation.hpp"
#include "F3Tonemap.pass.hpp"
#include "F4HostRead.pass.hpp"
#include "F5Shared.pass.hpp"
#include "NativeGraphSupport.hpp"
#include <lux/engine/render/runtime/NativeAssembly.hpp>
#include <thread>

using namespace native_graph_test;

void finishRuntimeProfile();

int main(int argc, char** argv)
{
    CHECK(argc == 4);
    const unsigned scene_count = static_cast<unsigned>(std::atoi(argv[2]));
    const unsigned view_count = static_cast<unsigned>(std::atoi(argv[3]));
    CHECK(scene_count >= 1 && scene_count <= 2 && view_count >= scene_count && view_count <= 4);
    Validation validation;
    {
        RuntimeNativeOptions native;
        native.instance = {{}, false, diagnostic, &validation};
        native.device.multiple_queues = true;
        native.device.dynamic_rendering = true;
        RuntimeOptions settings;
        settings.frame_interval_ns = 8'333'333;
        auto builder = checked(RenderRuntimeBuilder::create(native, settings));
        auto& backend = builder.backend();
        CHECK(checked(backend.retain(checked(Sampler::create(backend.device())))) == GraphSampler{1});
        auto asset =
            checked(Buffer::create(backend.allocator(), 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, EMemoryAccess::UPLOAD));
        const std::array<float, 4> input{2, 3, 5, 1};
        CHECK(asset.write(0, std::as_bytes(std::span{input})));
        const auto raw_buffer = asset.native();
        const auto retained = checked(backend.retain(std::move(asset)));
        CHECK(retained.get().native() == raw_buffer);
        RenderGraphBuilder graph;
        const auto source = graph.importBuffer("shared.asset", {16, 16}, EPersistentScope::RUNTIME);
        TextureDesc texture;
        texture.width = texture.height = 2;
        F5Shared params;
        params.asset.buffer = source;
        params.output.texture = graph.texture(texture, "scene.output");
        const auto compute =
            checked(graph.compute("scene.shared", {{"F5Shared", "default"}}, EExecutionScope::SCENE, params));
        F4HostRead read;
        read.source.texture = params.output.texture;
        const auto host = checked(graph.readback("view.read", EExecutionScope::VIEW, read));
        CHECK(graph.source(passKey("view.read"), "source", PassProducer{passKey("scene.shared")}));
        auto definition = checked(std::move(graph).finish());
        auto logical = checked(compileLogicalGraph(definition));
        auto values = makeGraphInvocationData(definition);
        const auto offset = logical.identity().passes[compute.value() - 1].scalar_fields[0].offset;
        std::vector<NativeShaderProgram> programs;
        programs.push_back(
            program(backend.device(), logical, compute, "F5Shared", PassSchema<F5Shared>::contract(), argv[1], true)
        );
        std::vector<NativePassCommand> commands{
            {compute, DispatchCommand{2, 2, 1}, EQueueRole::COMPUTE},
            {host, CopyCommand{}, EQueueRole::TRANSFER}
        };
        std::vector<NativeImportBinding> imports{
            {GraphResourceId{source.value()},
             GraphBackingId{1},
             retained,
             {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT},
             {},
             true}
        };
        const auto first_target = checked(backend.addOffscreenTarget(2, 2));
        const auto path = checked(backend.addPath(
            first_target,
            {std::move(logical), std::move(programs), std::move(commands), values, std::move(imports)}
        ));
        std::vector<RuntimeScene> scenes;
        std::vector<RuntimeView> views;
        for (unsigned i = 0; i < scene_count; ++i)
        {
            scenes.push_back(checked(backend.addScene()));
        }
        for (unsigned i = 0; i < view_count; ++i)
        {
            const auto target = i == 0 ? first_target : checked(backend.addOffscreenTarget(2, 2));
            views.push_back(checked(backend.addView(scenes[i % scene_count], target, path, values)));
        }
        auto runtime = checked(std::move(builder).finish());
        const auto frames = [&](std::uint64_t count)
        {
            for (unsigned i = 0; i < 10000; ++i)
            {
                CHECK(!runtime->terminalError());
                if (runtime->progress().render_frames >= count)
                {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            CHECK(false);
        };
        frames(24);
        allocation_count = allocation_bytes = 0;
        measuring = true;
        frames(264);
        measuring = false;
        CHECK(allocation_count == 0 && allocation_bytes == 0);
        for (auto view : views)
        {
            auto request = checked(runtime->readback(view, host, 0, 0, 64));
            std::optional<RuntimeReply> result;
            for (unsigned i = 0; i < 5000 && !result; ++i)
            {
                result = checked(request.poll());
                if (!result)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            CHECK(result);
            std::array<float, 16> pixels;
            std::memcpy(pixels.data(), result->bytes.data(), sizeof(pixels));
            for (unsigned i = 0; i < pixels.size(); ++i)
            {
                CHECK(pixels[i] == input[i % 4]);
            }
        }
        runtime->requestStop();
        CHECK(checked(runtime->waitStopped(5000)));
        finishRuntimeProfile();
        const auto progress = runtime->progress();
        CHECK(progress.native_compilations == 1);
        std::printf(
            "RUNTIME scenes=%u views=%u frames=%llu invocations=%llu scene_reused=%llu compiles=%llu cache_hits=%llu "
            "waits=%llu deadline_misses=%llu allocations=%llu bytes=%llu\n",
            scene_count,
            view_count,
            progress.render_frames,
            progress.native_invocations,
            progress.reused_scene_passes,
            progress.native_compilations,
            progress.native_cache_hits,
            progress.waits,
            progress.deadline_misses,
            allocation_count.load(),
            allocation_bytes.load()
        );
    }
    CHECK(validation.errors == 0);
}
