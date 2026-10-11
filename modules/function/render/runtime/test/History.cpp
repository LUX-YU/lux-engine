#include "Allocation.hpp"
#include "F4HostRead.pass.hpp"
#include "F5History.pass.hpp"
#include "NativeGraphSupport.hpp"
#include <lux/engine/render/runtime/NativeAssembly.hpp>
#include <thread>

using namespace native_graph_test;

RuntimeReply await(RuntimeRequest request)
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
    return {};
}

void frames(RenderRuntime& runtime, std::uint64_t target)
{
    for (unsigned i = 0; i < 5000; ++i)
    {
        CHECK(!runtime.terminalError());
        if (runtime.progress().render_frames >= target)
        {
            return;
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
        native.device.multiple_queues = true;
        native.capacity.frames_in_flight = 3;
        RuntimeOptions settings;
        settings.frame_interval_ns = 8'333'333;
        auto builder = checked(RenderRuntimeBuilder::create(native, settings));
        auto& backend = builder.backend();
        std::vector<std::reference_wrapper<const Buffer>> history;
        for (unsigned i = 0; i < 4; ++i)
        {
            auto buffer = checked(
                Buffer::create(backend.allocator(), 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, EMemoryAccess::UPLOAD)
            );
            const std::array<unsigned, 2> zero{};
            CHECK(buffer.write(0, std::as_bytes(std::span{zero})));
            history.push_back(checked(backend.retain(std::move(buffer))));
        }
        RenderGraphBuilder graph;
        GraphImportContract temporal;
        temporal.temporal_history = true;
        const auto previous = graph.importBuffer("view.history", {8, 4}, EPersistentScope::VIEW, temporal);
        TextureDesc texture;
        texture.width = texture.height = 1;
        texture.extent_kind = EExtentKind::TARGET_RELATIVE;
        F5History params;
        params.history.buffer = previous;
        params.output.texture = graph.texture(texture, "view.history.output");
        const auto compute =
            checked(graph.compute("view.accumulate", {{"F5History", "default"}}, EExecutionScope::VIEW, params));
        CHECK(graph.invocationInputs(passKey("view.accumulate"), 1 | 2 | 4));
        F4HostRead read;
        read.source.texture = params.output.texture;
        const auto host = checked(graph.readback("view.read", EExecutionScope::VIEW, read));
        auto definition = checked(std::move(graph).finish());
        auto logical = checked(compileLogicalGraph(definition));
        auto values = makeGraphInvocationData(definition);
        std::vector<RuntimeFrameField> fields;
        const auto& scalars = logical.identity().passes[compute.value() - 1].scalar_fields;
        for (std::uint32_t i = 0; i < scalars.size(); ++i)
        {
            if (scalars[i].path != "camera" && scalars[i].path != "seconds" && scalars[i].path != "epoch")
            {
                continue;
            }
            const auto kind = scalars[i].path == "camera"    ? ERuntimeFrameValue::CAMERA_FLOAT
                              : scalars[i].path == "seconds" ? ERuntimeFrameValue::RENDER_SECONDS
                                                             : ERuntimeFrameValue::HISTORY_EPOCH;
            fields.push_back({compute, i, kind});
        }
        std::vector<NativeShaderProgram> programs;
        programs.push_back(
            program(backend.device(), logical, compute, "F5History", PassSchema<F5History>::contract(), argv[1], true)
        );
        std::vector<NativePassCommand> commands{
            {compute, DispatchCommand{1, 1, 1}, EQueueRole::COMPUTE},
            {host, CopyCommand{}, EQueueRole::TRANSFER}
        };
        const auto imported = [&](unsigned i)
        {
            return NativeImportBinding{
                GraphResourceId{previous.value()},
                GraphBackingId{i + 1},
                history[i],
                {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT},
                {},
                true
            };
        };
        std::array<RuntimeTarget, 4> targets;
        for (auto& target : targets)
        {
            target = checked(backend.addOffscreenTarget(1, 1));
        }
        const auto reject_fields = [&](std::vector<RuntimeFrameField> invalid)
        {
            auto rejected =
                backend.addPath(targets[0], {logical, {}, commands, values, {imported(0)}, std::move(invalid)});
            CHECK(!rejected && rejected.error().type == kInvalidArgument);
        };
        auto duplicate_fields = fields;
        duplicate_fields.push_back(fields.front());
        reject_fields(std::move(duplicate_fields));
        auto wrong_type = fields;
        wrong_type.front().value = ERuntimeFrameValue::HISTORY_EPOCH;
        reject_fields(std::move(wrong_type));
        bool checked_resource_member = false;
        for (std::uint32_t i = 0; i < scalars.size(); ++i)
        {
            if (scalars[i].path.starts_with("history."))
            {
                reject_fields({{compute, i, ERuntimeFrameValue::HISTORY_EPOCH}});
                checked_resource_member = true;
            }
        }
        CHECK(checked_resource_member);
        const auto path = checked(backend.addPath(
            targets[0],
            {std::move(logical), std::move(programs), std::move(commands), values, {imported(0)}, std::move(fields)}
        ));
        const auto scene = checked(backend.addScene());
        std::array<RuntimeView, 4> views;
        for (unsigned i = 0; i < 4; ++i)
        {
            values.camera[0] = float(i + 1);
            const std::array input{imported(i)};
            views[i] = checked(backend.addView(scene, targets[i], path, values, input));
        }
        const std::array duplicate{imported(0)};
        auto wrong = backend.addView(scene, targets[0], path, values, duplicate);
        CHECK(!wrong && wrong.error().type == kWrongOwner);
        auto runtime = checked(std::move(builder).finish());
        frames(*runtime, 24);
        const auto pixel = [&](RuntimeView view)
        {
            auto value = await(checked(runtime->readback(view, host, 0, 0, 16)));
            std::array<float, 4> actual;
            std::memcpy(actual.data(), value.bytes.data(), sizeof(actual));
            CHECK(actual[1] == float(value.frame_serial));
            return actual;
        };
        for (unsigned i = 0; i < 4; ++i)
        {
            const auto actual = pixel(views[i]);
            std::fprintf(stderr, "HISTORY view=%u pixel=%f,%f,%f,%f\n", i, actual[0], actual[1], actual[2], actual[3]);
            CHECK(actual[0] == float(i + 1) && actual[2] == 1 && actual[3] > 0);
        }
        allocation_count = allocation_bytes = 0;
        measuring = true;
        frames(*runtime, runtime->progress().render_frames + 24);
        measuring = false;
        CHECK(allocation_count == 0 && allocation_bytes == 0);
        CHECK(runtime->progress().native_compilations == 1);
        (void)await(checked(runtime->resize(targets[0], 2, 2)));
        const auto reset_at = runtime->progress().render_frames;
        frames(*runtime, reset_at + 8);
        auto reset = await(checked(runtime->readback(views[0], host, 0, 0, 16)));
        std::array<float, 4> first;
        std::memcpy(first.data(), reset.bytes.data(), sizeof(first));
        CHECK(first[0] == 1 && first[2] == 2 && first[1] >= 8 && first[1] < float(reset.frame_serial));
        for (unsigned i = 1; i < 4; ++i)
        {
            CHECK(pixel(views[i])[2] == 1);
        }
        CHECK(runtime->progress().native_compilations == 2);
        runtime->requestStop();
        CHECK(checked(runtime->waitStopped(5000)));
    }
    CHECK(validation.errors == 0);
    std::puts("HISTORY four_actual_gpu_histories=PASS camera_and_time=PASS three_FIF=PASS resize_epoch=PASS "
              "unchanged_other_views=PASS heap=0 validation_errors=0");
}
