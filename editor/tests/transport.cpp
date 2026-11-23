#include <cassert>
#include <chrono>
#include <cstdio>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/detail/EditorUiScene.hpp>
#include <lux/engine/process/assets/AssetRepository.hpp>
#include <lux/engine/render/capabilities/UiScene.hpp>
#include <lux/engine/render/commands/ImageCommands.hpp>
#include <lux/engine/render/runtime/commands/Registration.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/frame/RenderFrame.hpp>
#include <lux/engine/render/inputs/SampledTextureInputs.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/systems/render/RenderAssets.hpp>
#include <lux/engine/systems/render/RenderDiagnostics.hpp>
#include <lux/engine/systems/render/RenderExecution.hpp>
#include <lux/engine/systems/render/WindowSurface.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <thread>

using namespace lux;
using namespace lux::render;
using namespace lux::engine::render;
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

        static RenderResult<std::unique_ptr<ViewAllocationFailure>>
        create(StorageCreateContext& context, StorageId) noexcept
        {
            return std::make_unique<ViewAllocationFailure>(context.device);
        }

        static StorageFactory factory() noexcept
        {
            return makeStorageFactory<Descriptor, ViewAllocationFailure, StorageCreateContext, &create>(nullptr);
        }
    };

    struct CleanupRejection final
    {
        static constexpr std::string_view name = "test.ui.cleanup-rejection";

        static CleanupRejection* create(ResourceRegistry&) noexcept
        {
            return new CleanupRejection;
        }

        void remove(const ImageCommands::Release::Request&, ImageCommands::Release::Response& response) noexcept
        {
            response.error = RenderError{kRuntimeUnsupported}; // Definite refusal; no native side effect.
        }
    };

    struct FailureCommands final
    {
        using PrerequisiteCapabilities = std::tuple<>;
        using Commands = std::tuple<ImageCommands::Release>;
        using PerFrameData = std::tuple<>;
    };

    struct ViewFailureCapability final
    {
        using Capability = FailureCommands;
        using Features = std::tuple<>;
        using FrameBindings = std::tuple<>;
        using Config = void;
        using Storages = std::tuple<ViewAllocationFailure>;
        using Bindings = std::tuple<CommandMethod<ImageCommands::Release, &CleanupRejection::remove>>;
        static constexpr FeatureName name{"test.ui.failure"};

        static CapabilityImplementation implementation() noexcept
        {
            static const std::array inputs{
                CapabilityStorageDependency{EStorageScope::GLOBAL, ViewAllocationFailure::Descriptor::name, "fault"}
            };

            static const CapabilityImplementation result = []
            {
                CapabilityImplementation value;
                value.implementation = name.name();
                return value;
            }();
            auto definition = result;
            definition.storage_dependencies = inputs;
            return definition;
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
    configuration.register_features = [](void* value, RenderServer& server) noexcept -> RenderResult<void>
    {
        auto installed = server.registerCapabilityImplementation<ViewFailureCapability>();
        if (!installed)
        {
            return cxx::unexpected(installed.error());
        }
        return {};
    };
    auto rendering = take(RenderExecution::create(std::move(configuration), execution));
    auto runtime = take(scene::SceneRuntime::create(execution, 64));
    asset::AssetVfs vfs;
    auto source = take(process::assets::AssetRepository::create(vfs.view(), {256U << 20U, 512U << 20U, 32}
    ));

    auto assets = take(RenderAssets::create(*rendering, *source));
    SurfaceDescription surface;
    assert(makeWindowSurface(window->nativeWindow(), true, surface));
    auto& root = window->uiRoot();
    auto ui_scene = take(EditorUiScene::create(
        *rendering,
        *runtime,
        *assets,
        *window,
        {640, 480, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::SURFACE},
        surface,
        take(root.fontAtlas())
    ));
    std::unique_ptr<ui::Pane> content = std::make_unique<Content>();
    assert(root.addPane(std::move(content)));
    auto& client = rendering->mainClient();
    std::size_t completed = 0, captured_vertices = 0, injected_frame_failures = 0;
    bool expect_allocation_failure = false, expect_cleanup_failure = false;

    struct FrameObservation final
    {
        std::size_t& completed;
        std::size_t& injected_failures;
        bool& expect_allocation_failure;
        bool& expect_cleanup_failure;

        static void complete(void* value, const FrameCompletion& response) noexcept
        {
            auto& state = *static_cast<FrameObservation*>(value);
            if (response.outcome == EFrameOutcome::NOT_SUBMITTED)
            {
                assert(response.error.type == kCommandCancelled);
                return;
            }
            if (response.error.type)
            {
                reportRenderFailure(response.error, "UI frame");
            }
            if (response.error.type != 0 && state.expect_cleanup_failure)
            {
                assert(response.error.type == kRuntimeUnsupported);
            }
            else if (response.error.type != 0)
            {
                assert(state.expect_allocation_failure && ViewAllocationFailure::failures == 1);
                assert(response.error.type == kDeviceFailure && state.injected_failures++ == 0);
            }
            ++state.completed;
        }
    } observation{completed, injected_frame_failures, expect_allocation_failure, expect_cleanup_failure};
    auto drive = [&](bool capture)
    {
        window::LuxWindow::pollEvents();
        static_cast<void>(rendering->consumeResponses());
        assert(execution.dispatchMainCompletions() && execution.dispatchTaskEvents());
        const auto prepare = [&](std::span<const scene::SceneInstanceId> candidates) noexcept -> RenderResult<void>
        {
            auto started = rendering->beginSceneBatch(candidates);
            if (!started)
            {
                return cxx::unexpected(started.error());
            }
            if (*started)
            {
                FrameWriter writer{*client.buildingFrame()};
                writer.onCompletion(&observation, &FrameObservation::complete);
            }
            if (capture)
            {
                assert(window->sampleInput());
                auto prepared = ui_scene->prepareFrame();
                if (!prepared)
                {
                    return prepared;
                }
                captured_vertices += root.statistics().draw_vertices;
            }
            return {};
        };
        auto published =
            runtime->driveFrame(capture ? scene::ESceneDriveMode::ADVANCE : scene::ESceneDriveMode::SUSPENDED, prepare);
        assert(published && published->empty());
        assert(rendering->submitSceneBatch(*published));
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
    auto call = [&]<RenderCommand Command>(typename Command::Request request, bool draw = true)
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
        until([&] { return done; }, draw);
        if (response.error.type != 0)
        {
            reportRenderFailure(response.error, Command::name);
        }
        assert(response.error.type == 0);
        return response;
    };
    const auto close_ui = [&](EditorUiScene& value)
    {
        bool closed = false;
        assert(execution.spawn(stdexec::then(
            execution::asResult(stdexec::starts_on(execution.mainScheduler(), value.closeAsync())),
            [&](auto result) noexcept
            {
                assert(result);
                closed = true;
            }
        )));
        until([&] { return closed; }, false);
    };
    const auto choices = call.operator(
    )<ListCapabilities>({std::string(cxx::make_basic_type_info<FailureCommands>().name()), "test.ui.failure"});
    assert(choices.implementations.size() == 1);
    CreateEffectRequest installation{{32, 32, rdesc::ETextureFormat::RGBA8_UNORM}};
    installation.composition.contributions.push_back(
        {"failure", choices.implementations.front().implementation_id, ConfigValue{}}
    );
    const auto ui_choices = call.operator()<ListCapabilities>({std::string(kUiSceneCapability)});
    installation.composition.contributions.push_back(
        {"ui", ui_choices.implementations.front().implementation_id, take(ConfigValue::make(UiConfiguration{}))}
    );
    installation.composition.outputs.push_back({"ui", "color"});
    const auto failure_effect = call.operator()<CreateEffect>(std::move(installation)).effect;
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
        *runtime,
        *assets,
        *window,
        {0, 480, rdesc::ETextureFormat::RGBA8_UNORM},
        {},
        take(root.fontAtlas())
    ));
    until([&] { return !invalid->outputReady(); }, true);
    assert(!invalid->outputReady());
    close_ui(*invalid);
    assert(invalid->settled());
    invalid.reset();
    until([&] { return client.pendingRequests() == 0; }, false);
    assert(root.clearPanes());
    const auto before = completed;
    until([&] { return completed >= before + 6; }, true);
    ui_scene->setExtent(0, 0, false);
    until([&] { return !take(ui_scene->outputReady()) && client.pendingRequests() == 0; }, false);
    ui_scene->setExtent(640, 480, true);
    until([&] { return take(ui_scene->outputReady()) && completed >= before + 10; }, true);
    close_ui(*ui_scene);
    assert(ui_scene->settled());
    ui_scene.reset();
    // A second real UI scene keeps its font texture identity after a definite cleanup refusal.
    ui_scene = take(EditorUiScene::create(
        *rendering,
        *runtime,
        *assets,
        *window,
        {640, 480, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::SURFACE},
        surface,
        take(root.fontAtlas())
    ));
    until([&] { return take(ui_scene->outputReady()); }, true);
    const auto commands = call.operator()<ListCommands>({});
    const auto command = std::ranges::find(commands.commands, ImageCommands::Release::name, &CommandInfo::name);
    assert(command != commands.commands.end());
    const auto bindings = call.operator()<ListCommandBindings>(command->id);
    const auto rejection = std::ranges::find(bindings.handlers, CleanupRejection::name, &HandlerInfo::name);
    assert(rejection != bindings.handlers.end());
    // Binding selection is a directory mutation at a Server frame boundary. Stop
    // generating frames and consume already accepted frames before changing it.
    until([&] { return client.pendingRequests() == 0 && !client.needsFrameInput(); }, false);
    call.operator()<SelectCommandBinding>({command->id, rejection->id}, false);
    // Selecting an unrelated global Handler must not redirect this UI capability's exact binding.
    const auto refusal = take(client.commandBinding<ImageCommands::Release>({failure_effect, 0, GlobalRenderScope{}}));
    ImageCommands::Release::Request refused{refusal.destination, {}};
    bool refused_done = false;
    assert(execution.spawn(stdexec::then(
        client.request(take(client.bind(refusal)), refused),
        [&](auto response) noexcept
        {
            assert(response.error.type == kRuntimeUnsupported);
            refused_done = true;
        }
    )));
    until([&] { return refused_done; }, false);
    close_ui(*ui_scene);
    assert(ui_scene->settled() && ui_scene->cleanupFailure().type == 0);
    bool joined = false;
    assert(execution.spawn(stdexec::then(
        rendering->whenStopped(),
        [&]() noexcept
        {
            assert(client.serverStopped());
            joined = true;
        }
    )));
    bool assets_closed = false;
    assert(execution.spawn(stdexec::then(
        execution::asResult(stdexec::starts_on(execution.mainScheduler(), assets->closeAsync())),
        [&](auto result) noexcept
        {
            assert(result);
            assets_closed = true;
        }
    )));
    until([&] { return assets_closed; }, false);
    assert(execution.spawn(stdexec::then(rendering->closeAsync(), []() noexcept {})));
    until([&] { static_cast<void>(rendering->consumeResponses()); return joined; }, false);
    assert(ui_scene->settled());
    assert(rendering->shutdown());
    assert(rendering->stats().validation_errors == 0);
    ui_scene.reset();
    assets.reset();
    source.reset();
    runtime.reset();
    rendering.reset();
    execution.requestStop();
    assert(execution.waitUntil([&]() noexcept { return execution.settled(); }));
    assert(execution.join());
    std::puts("PASS actual Root/font/UiRasterFeature, shared Main frame, minimize/restore and native shutdown; "
              "pixel/clip/blend assertions are retained in engine.render_ui");
}
