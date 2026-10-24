#include <cassert>
#include <chrono>
#include <cstdio>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/detail/EditorUiScene.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/runtime/FeatureRegistration.hpp>
#include <lux/engine/rendering/RenderDiagnostics.hpp>
#include <lux/engine/rendering/RenderExecution.hpp>
#include <lux/engine/rendering/WindowSurface.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <thread>

using namespace lux;
using namespace lux::render;
using namespace lux::rendering;
using namespace lux::editor;
using namespace std::chrono_literals;

namespace
{
    // Test-only backend fault. The real Surface closes its old swapchain and creates the candidate;
    // exactly one view allocation then fails. No fake submission/completion or production fault seam.
    struct ViewAllocationFailure final
    {
        struct Descriptor final
        {
            using Config = void;
            using Ops = void;
            static inline constexpr StorageNameView name{"test.ui.view-allocation-failure"};
            static inline const StorageDescriptor descriptor{name, 0, 0, {}, "One real output-view failure"};
        };

        nri::CoreInterface& core;
        decltype(nri::CoreInterface::CreateTextureView) create_view;
        static inline ViewAllocationFailure* armed = nullptr;
        static inline std::atomic_uint failures = 0;

        explicit ViewAllocationFailure(detail::DeviceSession& device) noexcept
            : core(const_cast<nri::CoreInterface&>(device.nriCore())), create_view(core.CreateTextureView)
        {
            assert(!armed);
            armed = this;
            core.CreateTextureView = &rejectView;
        }

        ~ViewAllocationFailure() noexcept
        {
            if (armed == this)
            {
                core.CreateTextureView = create_view;
                armed = nullptr;
            }
        }

        static nri::Result NRI_CALL rejectView(const nri::TextureViewDesc&, nri::Descriptor*&) noexcept
        {
            auto& fault = *armed;
            fault.core.CreateTextureView = fault.create_view;
            armed = nullptr;
            ++failures;
            return nri::Result::OUT_OF_MEMORY;
        }

        static RenderResult<std::unique_ptr<ViewAllocationFailure>> create(
            StorageCreateContext& context,
            StorageId
        ) noexcept
        {
            return std::make_unique<ViewAllocationFailure>(context.device);
        }

        static StorageFactory factory() noexcept
        {
            return makeStorageFactory<Descriptor, ViewAllocationFailure, StorageCreateContext, &create>(nullptr);
        }
    };

    template <class T> auto take(T result)
    {
        if constexpr (requires { result.error().type; })
        {
            if (!result)
            {
                reportRenderFailure(result.error(), "Editor UI transport");
            }
        }
        assert(result);
        return std::move(*result);
    }

    struct Content final : ui::Pane
    {
        Content() : Pane("Actual UI GPU content"), label("Font, clipping and alpha use UiRasterFeature")
        {
            assert(addElement(label));
        }

        ui::Label label;
    };
} // namespace

int main()
{
    auto execution = take(execution::ExecutionRuntime::create({2, 128, {32}}));
    auto window = take(EditorWindow::create({640, 480, "Render UI integration"}));
    execution.setWake(&window::LuxWindow::wakeEvents);
    RenderExecutionConfig configuration;
    configuration.render.channel_name = "editor.ui.test";
    configuration.render.validation = true;
    configuration.render.frames_in_flight = 3;
    configuration.render.uploads.emplace();
    configuration.device.texture_descriptors = true;
    StorageTypeId fault_type;
    configuration.feature_registration = &fault_type;
    configuration.register_features = [](void* value, RenderServer& server) noexcept -> RenderResult<void>
    {
        auto installed = server.registerStorage<ViewAllocationFailure>();
        if (!installed)
        {
            return cxx::unexpected(installed.error());
        }
        *static_cast<StorageTypeId*>(value) = *installed;
        return {};
    };
    auto rendering = take(RenderExecution::create(std::move(configuration), execution));
    SurfaceDescription surface;
    assert(makeWindowSurface(window->nativeWindow(), true, surface));
    auto& root = window->uiRoot();
    auto ui_scene = take(EditorUiScene::create(
        *rendering,
        *window,
        {640, 480, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::SURFACE},
        surface,
        take(root.fontAtlas())
    ));
    std::unique_ptr<ui::Pane> content = std::make_unique<Content>();
    assert(root.addPane(std::move(content)));
    auto& client = rendering->mainClient();
    std::size_t completed = 0, captured_vertices = 0, injected_frame_failures = 0;
    bool expect_allocation_failure = false;
    auto drive = [&](bool capture)
    {
        window::LuxWindow::pollEvents();
        static_cast<void>(rendering->consumeResponses());
        assert(execution.dispatchMainCompletions() && execution.dispatchTaskEvents());
        const bool draw = capture && take(ui_scene->outputReady());
        if ((!draw && !client.hasQueuedFrameCommands()) || !client.hasFrameCapacity())
        {
            return;
        }
        auto* frame = take(client.beginFrame());
        if (draw)
        {
            assert(window->sampleInput());
            assert(ui_scene->appendFrame(*frame));
            captured_vertices += root.statistics().draw_vertices;
        }
        assert(execution.submit(
            {},
            [&](execution::TaskReporter) noexcept
            {
                return stdexec::then(
                    client.submitFrame(*frame),
                    [&, frame](FrameCompletion response) noexcept
                    {
                        if (response.error.type)
                        {
                            reportRenderFailure(response.error, "UI frame");
                        }
                        if (response.error.type != 0)
                        {
                            assert(expect_allocation_failure && ViewAllocationFailure::failures == 1);
                            assert(response.error.type == kDeviceFailure && injected_frame_failures++ == 0);
                        }
                        assert(client.releaseFrame(*frame));
                        ++completed;
                    }
                );
            }
        ));
    };
    auto until = [&](auto condition, bool draw)
    {
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (!condition())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            drive(draw);
            std::this_thread::sleep_for(1ms);
        }
    };
    until([&] { return completed >= 12 && rendering->stats().presentations >= 6; }, true);
    assert(captured_vertices > 0);
    auto call = [&]<RenderCommand Command>(typename Command::Request request)
    {
        typename Command::Response response;
        bool done = false;
        assert(execution.spawn(stdexec::then(
            client.request(take(client.bind<Command>()), request),
            [&](typename Command::Response value) noexcept
            {
                response = std::move(value);
                done = true;
            }
        )));
        until([&] { return done; }, true);
        assert(response.error.type == 0);
        return response;
    };
    call.operator()<PrepareStorage>({{GlobalStorageLocation{}, fault_type, "fault"}, {}});
    auto placement = take(window->state()).placement;
    const auto original_placement = placement;
    placement.normal.width = 800;
    placement.normal.height = 600;
    expect_allocation_failure = true;
    assert(window->applyPlacement(placement));
    until([&] { return ui_scene->configurationError().type != 0; }, true);
    assert(ViewAllocationFailure::failures == 1 && !take(ui_scene->outputReady()));
    assert(window->applyPlacement(original_placement)); // A -> failed B (old chain closed) -> A must recreate.
    until([&] { return take(ui_scene->outputReady()); }, true);
    expect_allocation_failure = false;
    assert(injected_frame_failures == 1);
    const TargetDesc sampled{32, 32, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN, true};
    const auto first = call.operator()<CreateTarget>({sampled, {}}).target;
    const auto second = call.operator()<CreateTarget>({sampled, {}}).target;
    const auto first_image = take(ui_scene->addTargetImage(first, sampled));
    until([&] { return ui_scene->imageReady(first_image); }, true);
    bool removed = false;
    assert(execution.spawn(stdexec::then(
        ui_scene->removeTargetImage(first_image),
        [&](auto result) noexcept
        {
            assert(result);
            removed = true;
        }
    )));
    const auto second_image = take(ui_scene->addTargetImage(second, sampled));
    until([&] { return removed; }, true);
    call.operator()<RemoveTarget>({first});
    until([&] { return ui_scene->imageReady(second_image); }, true);
    removed = false;
    assert(execution.spawn(stdexec::then(
        ui_scene->removeTargetImage(second_image),
        [&](auto result) noexcept
        {
            assert(result);
            removed = true;
        }
    )));
    until([&] { return removed; }, true);
    call.operator()<RemoveTarget>({second});
    // A malformed real output fails asynchronously; it is not reported as a ready UI scene.
    auto invalid = take(EditorUiScene::create(
        *rendering,
        *window,
        {0, 480, rdesc::ETextureFormat::RGBA8_UNORM},
        {},
        take(root.fontAtlas())
    ));
    until([&] { return !invalid->outputReady(); }, true);
    assert(!invalid->outputReady());
    invalid->stopFrames();
    until([&] { return invalid->settled(); }, true);
    invalid.reset();
    until([&] { return client.pendingRequests() == 0; }, false);
    assert(root.clearPanes());
    const auto before = completed;
    until([&] { return completed >= before + 6; }, true);
    ui_scene->setExtent(0, 0, false);
    until([&] { return !take(ui_scene->outputReady()) && client.pendingRequests() == 0; }, false);
    ui_scene->setExtent(640, 480, true);
    until([&] { return take(ui_scene->outputReady()) && completed >= before + 10; }, true);
    ui_scene->stopFrames();
    until([&] { return ui_scene->settled() && client.pendingRequests() == 0; }, false);
    assert(rendering->shutdown());
    assert(rendering->stats().validation_errors == 0);
    ui_scene.reset();
    rendering.reset();
    execution.requestStop();
    assert(execution.waitUntil([&]() noexcept { return execution.settled(); }));
    assert(execution.join());
    std::puts("PASS actual Root/font/UiRasterFeature, shared Main frame, minimize/restore and native shutdown; "
              "pixel/clip/blend assertions are retained in engine.render_ui");
}
