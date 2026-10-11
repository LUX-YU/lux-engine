#include "Allocation.hpp"
#include "F3Tonemap.pass.hpp"
#include "F4HostRead.pass.hpp"
#include "F5Shared.pass.hpp"
#include "NativeGraphSupport.hpp"
#include <lux/engine/render/runtime/NativeAssembly.hpp>
#include <thread>

using namespace native_graph_test;

int main(int argc, char** argv)
{
    CHECK(argc >= 2 && argc <= 4);
    const unsigned backing_bytes = argc >= 3 ? 32 : 16;
    Validation validation;
    {
        RuntimeNativeOptions native;
        native.instance = {{}, true, diagnostic, &validation};
        native.device.multiple_queues = true;
        native.device.dynamic_rendering = true;
        RuntimeOptions settings;
        settings.frame_interval_ns = 8'333'333;
        auto builder = checked(RenderRuntimeBuilder::create(native, settings));
        auto& backend = builder.backend();
        CHECK(checked(backend.retain(checked(Sampler::create(backend.device())))) == GraphSampler{1});
        auto asset = checked(Buffer::create(
            backend.allocator(),
            backing_bytes,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            EMemoryAccess::UPLOAD
        ));
        const std::array<float, 4> input{2, 3, 5, 1};
        CHECK(asset.write(0, std::as_bytes(std::span{input})));
        const auto raw_buffer = asset.native();
        const auto retained = checked(backend.retain(std::move(asset)));
        CHECK(retained.get().native() == raw_buffer);
        RenderGraphBuilder graph;
        const auto source = graph.importBuffer("shared.asset", {backing_bytes, 16}, EPersistentScope::RUNTIME);
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
        const auto second_target = checked(backend.addOffscreenTarget(2, 2));
        const auto path = checked(backend.addPath(
            first_target,
            {std::move(logical), std::move(programs), std::move(commands), values, std::move(imports)}
        ));
        RenderGraphBuilder alternate;
        const auto second_source =
            alternate.importBuffer("shared.asset", {backing_bytes, 16}, EPersistentScope::RUNTIME);
        F5Shared second_params;
        second_params.asset.buffer = second_source;
        if (argc == 4)
        {
            second_params.asset.range.byte_count = 32;
        }
        second_params.output.texture = alternate.texture(texture, "alternate.hdr");
        const auto second_compute = checked(
            alternate.compute("alternate.fill", {{"F5Shared", "default"}}, EExecutionScope::SCENE, second_params)
        );
        F3Tonemap tone;
        tone.input.texture = second_params.output.texture;
        tone.nearest.sampler = GraphSampler{1};
        tone.output.texture = alternate.texture(texture, "alternate.ldr");
        const auto second_tone =
            checked(alternate.graphics("alternate.tone", {{"F3Tonemap", "default"}}, EExecutionScope::VIEW, tone));
        F4HostRead second_read;
        second_read.source.texture = tone.output.texture;
        const auto second_host = checked(alternate.readback("alternate.read", EExecutionScope::VIEW, second_read));
        auto second_definition = checked(std::move(alternate).finish());
        auto second_logical = checked(compileLogicalGraph(second_definition));
        auto second_values = makeGraphInvocationData(second_definition);
        std::vector<NativeShaderProgram> second_programs;
        second_programs.push_back(program(
            backend.device(),
            second_logical,
            second_compute,
            "F5Shared",
            PassSchema<F5Shared>::contract(),
            argv[1],
            true
        ));
        second_programs.push_back(program(
            backend.device(),
            second_logical,
            second_tone,
            "F3Tonemap",
            PassSchema<F3Tonemap>::contract(),
            argv[1],
            false
        ));
        std::vector<NativePassCommand> second_commands{
            {second_compute, DispatchCommand{2, 2, 1}, EQueueRole::COMPUTE},
            {second_tone, DrawCommand{}, EQueueRole::GRAPHICS},
            {second_host, CopyCommand{}, EQueueRole::TRANSFER}
        };
        std::vector<NativeImportBinding> second_imports{
            {GraphResourceId{second_source.value()},
             GraphBackingId{1},
             retained,
             {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT},
             {},
             true}
        };
        auto second_candidate = backend.addPath(
            first_target,
            {std::move(second_logical),
             std::move(second_programs),
             std::move(second_commands),
             second_values,
             std::move(second_imports)}
        );
        const auto second_path = checked(std::move(second_candidate));
        const auto first_scene = checked(backend.addScene());
        const auto second_scene = checked(backend.addScene());
        const auto first_view = checked(backend.addView(first_scene, first_target, path, values));
        const auto second_view = checked(backend.addView(second_scene, second_target, path, values));
        auto runtime = checked(std::move(builder).finish());
        const float factor = 7;
        std::memcpy(values.passes[compute.value() - 1].scalars.data() + offset, &factor, sizeof(factor));
        CHECK(runtime->publishScenePass(second_scene, path, compute, 1, values.passes[compute.value() - 1].scalars));
        const auto wait_frames = [&](std::uint64_t target)
        {
            for (unsigned i = 0; i < 3000; ++i)
            {
                if (runtime->progress().render_frames >= target)
                {
                    return;
                }
                if (auto error = runtime->terminalError())
                {
                    std::fprintf(
                        stderr,
                        "SHARED terminal=%llu args=%llu,%llu\n",
                        static_cast<unsigned long long>(error->type),
                        error->args[0],
                        error->args[1]
                    );
                    CHECK(false);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            CHECK(false);
        };
        wait_frames(24);
        for (auto [view, scale] : {std::pair{first_view, 1.0f}, std::pair{second_view, factor}})
        {
            auto request = checked(runtime->readback(view, host, 0, 0, 64));
            std::optional<RuntimeReply> result;
            for (unsigned i = 0; i < 2000 && !result; ++i)
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
                CHECK(pixels[i] == input[i % 4] * scale);
            }
        }
        allocation_count = allocation_bytes = 0;
        measuring = true;
        wait_frames(runtime->progress().render_frames + 24);
        measuring = false;
        CHECK(allocation_count == 0 && allocation_bytes == 0);
        CHECK(
            runtime->progress().native_compilations == (argc == 4 ? 3 : 2) &&
            runtime->progress().reused_scene_passes == 0
        );
        const auto await = [&](RuntimeRequest request)
        {
            for (unsigned i = 0; i < 5000; ++i)
            {
                auto value = checked(request.poll());
                if (value)
                {
                    return *value;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            CHECK(false);
            return RuntimeReply{};
        };
        (void)await(checked(runtime->setPath(first_view, second_path)));
        wait_frames(runtime->progress().render_frames + 8);
        auto mapped = await(checked(runtime->readback(first_view, second_host, 0, 0, 64)));
        std::array<float, 16> mapped_pixels;
        std::memcpy(mapped_pixels.data(), mapped.bytes.data(), sizeof(mapped_pixels));
        for (unsigned i = 0; i < mapped_pixels.size(); ++i)
        {
            const float expected = i % 4 == 3 ? 1.0f : input[i % 4] / (1.0f + input[i % 4]);
            CHECK(std::abs(mapped_pixels[i] - expected) < 1e-6f);
        }
        auto unchanged = await(checked(runtime->readback(second_view, host, 0, 0, 64)));
        std::array<float, 16> original_pixels;
        std::memcpy(original_pixels.data(), unchanged.bytes.data(), sizeof(original_pixels));
        for (unsigned i = 0; i < original_pixels.size(); ++i)
        {
            CHECK(original_pixels[i] == input[i % 4] * factor);
        }
        (void)await(checked(runtime->remove(first_view)));
        (void)await(checked(runtime->remove(first_scene)));
        (void)await(checked(runtime->remove(first_target)));
        wait_frames(runtime->progress().render_frames + 8);
        unchanged = await(checked(runtime->readback(second_view, host, 0, 0, 64)));
        std::memcpy(original_pixels.data(), unchanged.bytes.data(), sizeof(original_pixels));
        for (unsigned i = 0; i < original_pixels.size(); ++i)
        {
            CHECK(original_pixels[i] == input[i % 4] * factor);
        }
        CHECK(runtime->progress().native_compilations == (argc == 4 ? 3 : 2));
        runtime->requestStop();
        CHECK(checked(runtime->waitStopped(5000)));
        CHECK(runtime->state() == ERuntimeState::STOPPED);
    }
    CHECK(validation.errors == 0);
    std::puts("SHARED_ASSET two_scenes=PASS one_actual_buffer=PASS one_upload=PASS scene_values=PASS "
              "different_render_paths=PASS scene_delete_isolation=PASS GPU_oracle=PASS heap=0 validation_errors=0");
}
