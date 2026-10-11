#include "F4HostRead.pass.hpp"
#include "F5Shared.pass.hpp"
#include "NativeGraphSupport.hpp"
#include <atomic>
#include <lux/engine/render/runtime/NativeAssembly.hpp>
#include <thread>

using namespace native_graph_test;

namespace
{
    std::atomic<bool> fail_image{}, hold_submit{}, entered_submit{};
    std::atomic<unsigned> lose_submit{}, native_submits{};
    VkDevice native_device{};

    template <typename F> void until(F&& condition)
    {
        for (unsigned i = 0; i < 5000; ++i)
        {
            if (condition())
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(false);
    }

    RenderResult<RuntimeReply> result(RuntimeRequest& request)
    {
        for (unsigned i = 0; i < 5000; ++i)
        {
            auto value = request.poll();
            if (!value)
            {
                return lux::cxx::unexpected(value.error());
            }
            if (*value)
            {
                return **value;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(false);
        return lux::cxx::unexpected(RenderError{kInvalidArgument});
    }

    struct Fixture
    {
        std::unique_ptr<RenderRuntime> runtime;
        RuntimeScene scene;
        RuntimeTarget target;
        RuntimeView view;
        RuntimePath path;
        GraphPassId compute, host;
        GraphInvocationData values;
        std::uint32_t offset;
    };

    Fixture create(Validation& validation, const char* shaders)
    {
        RuntimeNativeOptions native;
        native.instance = {{}, true, diagnostic, &validation};
        native.device.multiple_queues = true;
        RuntimeOptions settings;
        settings.frame_interval_ns = 8'333'333;
        settings.transport.pending_programs = 1;
        settings.transport.replies = 2;
        auto builder = checked(RenderRuntimeBuilder::create(native, settings));
        auto& backend = builder.backend();
        native_device = backend.device().native();
        auto asset =
            checked(Buffer::create(backend.allocator(), 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, EMemoryAccess::UPLOAD));
        const std::array<float, 4> input{2, 3, 5, 1};
        CHECK(asset.write(0, std::as_bytes(std::span{input})));
        const auto retained = checked(backend.retain(std::move(asset)));
        RenderGraphBuilder graph;
        const auto source = graph.importBuffer("lifecycle.asset", {16, 16}, EPersistentScope::RUNTIME);
        TextureDesc texture;
        texture.width = texture.height = 1;
        texture.extent_kind = EExtentKind::TARGET_RELATIVE;
        F5Shared params;
        params.asset.buffer = source;
        params.output.texture = graph.texture(texture, "lifecycle.output");
        const auto compute =
            checked(graph.compute("lifecycle.fill", {{"F5Shared", "default"}}, EExecutionScope::VIEW, params));
        F4HostRead read;
        read.source.texture = params.output.texture;
        const auto host = checked(graph.readback("lifecycle.read", EExecutionScope::VIEW, read));
        auto definition = checked(std::move(graph).finish());
        auto logical = checked(compileLogicalGraph(definition));
        auto values = makeGraphInvocationData(definition);
        const auto offset = logical.identity().passes[compute.value() - 1].scalar_fields[0].offset;
        std::vector<NativeShaderProgram> programs;
        programs.push_back(
            program(backend.device(), logical, compute, "F5Shared", PassSchema<F5Shared>::contract(), shaders, true)
        );
        std::vector<NativePassCommand> commands{
            {compute, DispatchCommand{8, 8, 1}, EQueueRole::COMPUTE},
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
        const auto target = checked(backend.addOffscreenTarget(2, 2));
        const auto path = checked(backend.addPath(
            target,
            {std::move(logical), std::move(programs), std::move(commands), values, std::move(imports)}
        ));
        const auto scene = checked(backend.addScene());
        const auto view = checked(backend.addView(scene, target, path, values));
        return {
            checked(std::move(builder).finish()),
            scene,
            target,
            view,
            path,
            compute,
            host,
            std::move(values),
            offset
        };
    }

    void pixel(Fixture& f, float factor)
    {
        auto request = checked(f.runtime->readback(f.view, f.host, 0, 0, 16));
        auto reply = checked(result(request));
        std::array<float, 4> actual;
        std::memcpy(actual.data(), reply.bytes.data(), sizeof(actual));
        CHECK((actual == std::array{2 * factor, 3 * factor, 5 * factor, factor}));
    }
} // namespace

namespace lux::render::vulkan::test
{
    VkResult before(const char* operation) noexcept
    {
        const std::string_view name{operation};
        if (name == "image" && fail_image.exchange(false))
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        if (name == "submit")
        {
            ++native_submits;
            if (hold_submit.load())
            {
                entered_submit.store(true);
                hold_submit.wait(true);
            }
            const auto remaining = lose_submit.load();
            if (remaining && lose_submit.fetch_sub(1) == 1)
            {
                // Fault injection cannot physically cancel a real healthy GPU.
                // Discharge its earlier work before simulating the lost-device
                // contract; this cold test-only barrier is absent in production.
                CHECK(vkDeviceWaitIdle(native_device) == VK_SUCCESS);
                return VK_ERROR_DEVICE_LOST;
            }
        }
        return VK_SUCCESS;
    }

    VkResult after(const char*, VkResult value) noexcept
    {
        return value;
    }

    void destroyed(const char*) noexcept {}
} // namespace lux::render::vulkan::test

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    Validation validation;
    {
        auto f = create(validation, argv[1]);
        until([&] { return f.runtime->progress().render_frames >= 8; });
        pixel(f, 1);
        fail_image = true;
        auto resize = checked(f.runtime->resize(f.target, 3, 3));
        const auto rejected = result(resize);
        CHECK(
            !rejected && rejected.error().type == kNativeFailure &&
            rejected.error().args[0] == static_cast<std::uint32_t>(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        );
        pixel(f, 1); // Last-good target and native invocation remain usable.
        auto resized = checked(f.runtime->resize(f.target, 3, 3));
        CHECK(result(resized));
        until([&] { return f.runtime->progress().render_frames >= 16; });
        pixel(f, 1);

        auto foreign = f.view;
        ++foreign.owner;
        auto wrong = checked(f.runtime->setActive(foreign, true));
        auto wrong_result = result(wrong);
        CHECK(!wrong_result && wrong_result.error().type == kWrongOwner);
        std::thread wrong_thread(
            [&]
            {
                auto refused = f.runtime->setActive(f.view, true);
                CHECK(!refused && refused.error().type == kTransportWrongThread);
            }
        );
        wrong_thread.join();

        hold_submit = true;
        until([] { return entered_submit.load(); });
        auto& scalars = f.values.passes[f.compute.value() - 1].scalars;
        const float first = 3, retained = 7, later = 11;
        std::memcpy(scalars.data() + f.offset, &first, sizeof(first));
        CHECK(f.runtime->publishViewPass(f.view, f.compute, 1, f.values.camera, scalars));
        std::memcpy(scalars.data() + f.offset, &retained, sizeof(retained));
        auto full = f.runtime->publishViewPass(f.view, f.compute, 2, f.values.camera, scalars);
        CHECK(!full && full.error().type == kTransportCapacity);
        std::memcpy(scalars.data() + f.offset, &later, sizeof(later));
        auto blocked = f.runtime->publishViewPass(f.view, f.compute, 3, f.values.camera, scalars);
        CHECK(!blocked && blocked.error().type == kTransportBusy);
        hold_submit = false;
        hold_submit.notify_all();
        until(
            [&]
            {
                auto retry = f.runtime->retryPublication();
                return bool(retry);
            }
        );
        until([&] { return f.runtime->progress().publications == 2; });
        const auto frame = f.runtime->progress().render_frames;
        until([&] { return f.runtime->progress().render_frames >= frame + 3; });
        pixel(f, retained); // Immutable retained bytes, not mutated source bytes.

        auto remove = checked(f.runtime->remove(f.view));
        CHECK(result(remove));
        auto stale = checked(f.runtime->setActive(f.view, true));
        CHECK(!result(stale));
        auto remove_scene = checked(f.runtime->remove(f.scene));
        CHECK(result(remove_scene));
        auto remove_target = checked(f.runtime->remove(f.target));
        CHECK(result(remove_target));
        const auto stopped = f.runtime->progress().render_frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(stopped == f.runtime->progress().render_frames);
        auto surviving_reply = checked(f.runtime->setActive(f.view, true));
        f.runtime.reset();
        CHECK(!result(surviving_reply)); // Arena outlives Runtime safely.
    }
    CHECK(validation.errors == 0);
    for (unsigned submission : {1u, 3u})
    {
        auto f = create(validation, argv[1]);
        until([&] { return f.runtime->progress().render_frames >= 4; });
        pixel(f, 1);
        const auto before = native_submits.load();
        lose_submit = submission; // Real Runtime; fail before this native submit only.
        until([&] { return f.runtime->state() == ERuntimeState::FAILED; });
        const auto failure = f.runtime->terminalError();
        CHECK(
            failure && failure->type == kNativeFailure &&
            failure->args[0] == static_cast<std::uint32_t>(VK_ERROR_DEVICE_LOST)
        );
        CHECK(native_submits.load() - before == submission);
        CHECK(checked(f.runtime->waitStopped(5000)));
        CHECK(f.runtime->pollDiagnostic().has_value());
        auto refused = f.runtime->setActive(f.view, true);
        CHECK(!refused && refused.error().type == kTransportStopping);
        f.runtime.reset();
        CHECK(validation.errors == 0);
    }
    std::puts("LIFECYCLE rollback=PASS retained_revision=PASS owner_thread=PASS remove_inflight=PASS "
              "reply_after_runtime=PASS device_lost_first_partial=PASS validation_errors=0");
}
