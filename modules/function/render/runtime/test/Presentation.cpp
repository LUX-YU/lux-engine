#include "Allocation.hpp"
#include "F4HostRead.pass.hpp"
#include "F5Shared.pass.hpp"
#include "NativeGraphSupport.hpp"
#include <lux/engine/render/runtime/NativeAssembly.hpp>
#define NOMINMAX
#include <Windows.h>

using namespace native_graph_test;

void pump()
{
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    Sleep(1);
}

RuntimeReply await(RuntimeRequest request)
{
    for (unsigned i = 0; i < 3000; ++i)
    {
        auto reply = checked(request.poll());
        if (reply)
        {
            return *reply;
        }
        pump();
    }
    CHECK(false);
    return {};
}

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    Validation validation;
    const auto host = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = host;
    type.lpszClassName = L"LuxF5Qualification";
    CHECK(RegisterClassW(&type));
    const auto window = CreateWindowExW(
        0,
        type.lpszClassName,
        L"F5 GPU Qualification",
        WS_POPUP,
        0,
        0,
        32,
        32,
        nullptr,
        nullptr,
        host,
        nullptr
    );
    CHECK(window);
    const auto mirror_window = CreateWindowExW(
        0,
        type.lpszClassName,
        L"F5 Mirror Qualification",
        WS_POPUP,
        0,
        0,
        32,
        32,
        nullptr,
        nullptr,
        host,
        nullptr
    );
    CHECK(mirror_window);
    {
        RuntimeNativeOptions native;
        native.instance = {{}, true, diagnostic, &validation};
        native.presentation = true;
        native.device.multiple_queues = true;
        RuntimeOptions settings;
        settings.frame_interval_ns = 8'333'333;
        auto assembly = checked(RenderRuntimeBuilder::create(native, settings));
        auto& backend = assembly.backend();
        CHECK(backend.device().swapchainMaintenance());
        const auto target = checked(backend.addWindowTarget(host, window, 32, 32));
        const auto mirror = checked(backend.addWindowTarget(host, mirror_window, 32, 32));
        auto asset =
            checked(Buffer::create(backend.allocator(), 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, EMemoryAccess::UPLOAD));
        const std::array<float, 4> color{0.25f, 0.5f, 0.75f, 1};
        CHECK(asset.write(0, std::as_bytes(std::span{color})));
        const auto retained = checked(backend.retain(std::move(asset)));
        RenderGraphBuilder graph;
        const auto source = graph.importBuffer("present.color", {16, 16}, EPersistentScope::RUNTIME);
        TextureDesc texture;
        texture.extent_kind = EExtentKind::TARGET_RELATIVE;
        F5Shared params;
        params.asset.buffer = source;
        params.output.texture = graph.importTarget("present.output", texture, renderTargetSemanticId("f5.output"));
        const auto compute =
            checked(graph.compute("present.compute", {{"F5Shared", "default"}}, EExecutionScope::TARGET, params));
        F4HostRead read;
        read.source.texture = params.output.texture;
        const auto output = checked(graph.readback("present.read", EExecutionScope::TARGET, read));
        CHECK(graph.source(passKey("present.read"), "source", PassProducer{passKey("present.compute")}));
        CHECK(
            graph.exportTexture(params.output.texture, PassProducer{passKey("present.compute")}, EGraphOutput::PRESENT)
        );
        auto definition = checked(std::move(graph).finish());
        auto logical = checked(compileLogicalGraph(definition));
        auto values = makeGraphInvocationData(definition);
        const auto factor_offset = logical.identity().passes[compute.value() - 1].scalar_fields[0].offset;
        std::vector<NativeShaderProgram> programs;
        programs.push_back(
            program(backend.device(), logical, compute, "F5Shared", PassSchema<F5Shared>::contract(), argv[1], true)
        );
        std::vector<NativePassCommand> commands{
            {compute, DispatchCommand{64, 64, 1}, EQueueRole::COMPUTE},
            {output, CopyCommand{}, EQueueRole::TRANSFER}
        };
        std::vector<NativeImportBinding> imports{
            {GraphResourceId{source.value()},
             GraphBackingId{1},
             retained,
             {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT},
             {},
             true}
        };
        const auto path = checked(backend.addPath(
            target,
            {std::move(logical), std::move(programs), std::move(commands), values, std::move(imports)}
        ));
        // A valid offscreen-only path is not a valid replacement for a View
        // already consumed by either swapchain. Rejection must be transactional.
        RenderGraphBuilder offscreen_graph;
        F5Shared offscreen_params;
        offscreen_params.asset.buffer =
            offscreen_graph.importBuffer("present.color", {16, 16}, EPersistentScope::RUNTIME);
        offscreen_params.output.texture =
            offscreen_graph.importTarget("present.output", texture, renderTargetSemanticId("f5.output"));
        CHECK(
            checked(
                offscreen_graph
                    .compute("present.compute", {{"F5Shared", "default"}}, EExecutionScope::TARGET, offscreen_params)
            ) == compute
        );
        F4HostRead offscreen_read;
        offscreen_read.source.texture = offscreen_params.output.texture;
        CHECK(checked(offscreen_graph.readback("present.read", EExecutionScope::TARGET, offscreen_read)) == output);
        CHECK(offscreen_graph.source(passKey("present.read"), "source", PassProducer{passKey("present.compute")}));
        auto offscreen_definition = checked(std::move(offscreen_graph).finish());
        auto offscreen_logical = checked(compileLogicalGraph(offscreen_definition));
        std::vector<NativeShaderProgram> offscreen_programs;
        offscreen_programs.push_back(program(
            backend.device(),
            offscreen_logical,
            compute,
            "F5Shared",
            PassSchema<F5Shared>::contract(),
            argv[1],
            true
        ));
        const auto offscreen_path = checked(backend.addPath(
            target,
            {std::move(offscreen_logical),
             std::move(offscreen_programs),
             {{compute, DispatchCommand{64, 64, 1}, EQueueRole::COMPUTE}, {output, CopyCommand{}, EQueueRole::TRANSFER}
             },
             values,
             {{GraphResourceId{source.value()},
               GraphBackingId{1},
               retained,
               {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT},
               {},
               true}}}
        ));
        const auto scene = checked(backend.addScene());
        auto overlay_values = values;
        const float half = 0.5f;
        std::memcpy(overlay_values.passes[compute.value() - 1].scalars.data() + factor_offset, &half, sizeof(half));
        // Reverse creation order proves declared Layer order, not SlotMap order.
        const auto overlay =
            checked(backend.addView(scene, target, path, overlay_values, {}, {10, RuntimeTargetRect{16, 0, 16, 16}}));
        const auto view = checked(backend.addView(scene, target, path, values));
        CHECK(backend.reuseOutput(view, mirror));
        CHECK(backend.reuseOutput(overlay, mirror, {10, RuntimeTargetRect{16, 0, 16, 16}}));
        CHECK(!backend.reuseOutput(view, mirror));
        auto runtime = checked(std::move(assembly).finish());
        const auto frames = [&](std::uint64_t count)
        {
            for (unsigned i = 0; i < 4000; ++i)
            {
                if (auto error = runtime->terminalError())
                {
                    std::fprintf(
                        stderr,
                        "WSI terminal=%llu native=%llu\n",
                        static_cast<unsigned long long>(error->type),
                        error->args[0]
                    );
                    CHECK(false);
                }
                if (runtime->progress().render_frames >= count)
                {
                    return;
                }
                pump();
            }
            CHECK(false);
        };
        frames(24);
        std::uint32_t target_width = 32;
        const auto verify = [&]
        {
            const auto pixels = await(checked(runtime->readbackTarget(target, 0, 16)));
            const std::array<unsigned, 4> expected{191, 128, 64, 255};
            for (unsigned i = 0; i < 16; ++i)
            {
                CHECK(std::abs(int(std::to_integer<unsigned>(pixels.bytes[i])) - int(expected[i % 4])) <= 1);
            }
            const auto layer = await(checked(runtime->readbackTarget(target, (4 * target_width + 20) * 4, 16)));
            const std::array<unsigned, 4> expected_layer{96, 64, 32, 128};
            for (unsigned i = 0; i < 16; ++i)
            {
                CHECK(std::abs(int(std::to_integer<unsigned>(layer.bytes[i])) - int(expected_layer[i % 4])) <= 1);
            }
        };
        verify();
        auto rejected = checked(runtime->setPath(view, offscreen_path));
        bool rejected_path = false;
        for (unsigned i = 0; i < 3000 && !rejected_path; ++i)
        {
            auto result = rejected.poll();
            if (!result)
            {
                CHECK(result.error().type == kInvalidArgument);
                rejected_path = true;
            }
            else
            {
                CHECK(!result->has_value());
                pump();
            }
        }
        CHECK(rejected_path && !runtime->terminalError());
        frames(runtime->progress().render_frames + 4);
        verify();
        allocation_count = allocation_bytes = 0;
        measuring = true;
        frames(runtime->progress().render_frames + 24);
        measuring = false;
        CHECK(allocation_count == 0 && allocation_bytes == 0);
        (void)await(checked(runtime->resize(target, 0, 0)));
        const auto mirror_start = runtime->progress();
        frames(mirror_start.render_frames + 8);
        CHECK(runtime->progress().native_invocations == mirror_start.native_invocations);
        const auto mirrored = await(checked(runtime->readbackTarget(mirror, 0, 16)));
        const std::array<unsigned, 4> mirror_expected{191, 128, 64, 255};
        for (unsigned i = 0; i < 16; ++i)
        {
            CHECK(std::abs(int(std::to_integer<unsigned>(mirrored.bytes[i])) - int(mirror_expected[i % 4])) <= 1);
        }
        (void)await(checked(runtime->resize(mirror, 0, 0)));
        const auto paused = runtime->progress().render_frames;
        for (unsigned i = 0; i < 80; ++i)
        {
            pump();
        }
        CHECK(runtime->progress().render_frames == paused);
        CHECK(SetWindowPos(window, nullptr, 0, 0, 48, 24, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
        (void)await(checked(runtime->resize(target, 48, 24)));
        (void)await(checked(runtime->resize(mirror, 32, 32)));
        target_width = 48;
        frames(paused + 24);
        verify();
        CHECK(runtime->progress().native_compilations == 3);
        runtime->requestStop();
        CHECK(checked(runtime->waitStopped(5000)));
        CHECK(runtime->state() == ERuntimeState::STOPPED);
        (void)view;
        (void)overlay;
    }
    CHECK(DestroyWindow(window));
    CHECK(DestroyWindow(mirror_window));
    CHECK(UnregisterClassW(type.lpszClassName, host));
    CHECK(validation.errors == 0);
    std::puts("PRESENT actual_swapchain_BGRA8_oracle=PASS ordered_opaque_layers=PASS two_targets_one_result=PASS "
              "32x32_to_48x24=PASS minimize=PASS heap=0 validation_errors=0");
}
