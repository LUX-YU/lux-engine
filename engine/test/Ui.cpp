#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/Engine.hpp>
#include <lux/engine/Init.hpp>
#include <lux/engine/assets/AssetRepository.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/input/Input.hpp>
#include <lux/engine/render/capabilities/UiScene.hpp>
#include <lux/engine/render/commands/ImageCommands.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/systems/render/ImguiFrameCapture.hpp>
#include <lux/engine/systems/render/RenderFrameSubmission.hpp>
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/systems/render/RenderSystem.hpp>
#include <lux/engine/systems/render/WindowSurface.hpp>
#include <lux/engine/systems/render/view/ViewReaction.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/WindowInput.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/world/World.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string_view>

namespace
{
    using namespace lux;
    namespace rendering = lux::engine::render;
    using Result = render::RenderResult<void>;
    using Clock = std::chrono::steady_clock;
    constexpr std::uint32_t kNativeOutOfMemory = 3; // NRI Result, observed only by the fault fixture.

    // The one outer event pump. Tasks never run this recursively or block Main on an IPC response.
    Result runTask(exec::task<Result> task) noexcept
    {
        auto& execution = execution::ExecutionRuntime::instance();
        std::optional<Result> completed;
        auto result =
            stdexec::starts_on(execution.mainScheduler(), std::move(task)) |
            stdexec::upon_error([](std::exception_ptr) noexcept -> Result { std::terminate(); }) |
            stdexec::upon_stopped(
                []() noexcept -> Result { return cxx::unexpected(render::RenderError{render::kCommandCancelled}); }
            );
        auto started = execution.spawn(stdexec::then(
            std::move(result),
            [&completed](Result value) noexcept { completed.emplace(std::move(value)); }
        ));
        if (!started)
        {
            return cxx::unexpected(execution::toError(started.error()));
        }
        while (!completed)
        {
            window::LuxWindow::pollEvents();
            if (!execution.dispatch())
            {
                std::terminate(); // This outer pump must never be entered from an Execution callback.
            }
            if (!completed && !execution.hasReadyWork())
            {
                // Replies post a native wake. The bounded wait also keeps platform-only changes responsive.
                window::LuxWindow::waitEvents(0.05);
            }
        }
        return std::move(*completed);
    }

    class ImguiCollector final : public rendering::RenderDataCollector
    {
    public:
        static constexpr cxx::type_info capability = cxx::make_basic_type_info<render::ImguiCapability>();
        using CreateResult = render::RenderResult<std::unique_ptr<ImguiCollector>>;
        inline static bool reject_creation{};

        [[nodiscard]] static CreateResult create(
            rendering::RenderSystem& system,
            render::FrameDataDestination destination,
            ui::DrawView& draw,
            render::ImguiImage& font,
            std::array<render::AttachmentId, 2>& attachments
        ) noexcept
        {
            const bool valid_owner = !system.sceneId().isNull() && !system.effectId().isNull() &&
                                     std::get<render::SceneId>(destination.scope) == system.sceneId() &&
                                     system.effectId() == destination.effect;
            if (!valid_owner)
            {
                return cxx::unexpected(render::RenderError{render::kRuntimeIdentity});
            }
            if (reject_creation)
            {
                return cxx::unexpected(render::RenderError{render::kRuntimeCapacity});
            }
            auto attached = destination;
            attached.scope = attachments.front();
            return std::unique_ptr<ImguiCollector>{new ImguiCollector(draw, font, attachments, attached)};
        }

        Result collect(render::FrameWriter& frame) noexcept override
        {
            if (!draw_.valid() || draw_.data().TotalVtxCount == 0)
            {
                return cxx::unexpected(render::RenderError{render::kFrameContract});
            }
            for (std::size_t i = 0; i < attachments_.size(); ++i)
            {
                if (attachments_[i].isNull()) continue;
                output_.destination.scope = attachments_[i];
                Result collected;
                if (const auto* input_case = std::getenv("LUX_UI_INPUT_CASE"))
                {
                    std::string_view selected = input_case;
                    if (selected == "invalid-second-attachment")
                        selected = i == 1 ? "invalid-callback" : "odd-indices";
                    collected = collectFixture(frame, selected);
                }
                else
                {
                    collected = rendering::captureImguiFrame(frame, output_, draw_.data(), {&font_, 1});
                }
                if (!collected) return collected;
            }
            return {};
        }

    private:
        Result collectFixture(render::FrameWriter& frame, std::string_view input_case) noexcept
        {
            // Exercise the Server boundary directly: captureImguiFrame correctly rejects arbitrary
            // callbacks before transport, so it cannot exercise rollback after a partial parse.
            const std::array<ImDrawVert, 3> vertices{{
                {{32, 32}, {0, 0}, IM_COL32_WHITE},
                {{96, 32}, {0, 0}, IM_COL32_WHITE},
                {{32, 96}, {0, 0}, IM_COL32_WHITE}
            }};
            const std::array<ImDrawIdx, 3> indices{0, 1, 2}; // UINT16 requires a two-byte padded tail.
            auto lists = frame.allocate<render::ImguiDrawList>(input_case == "empty" ? 0 : 2);
            auto images = frame.copy<render::ImguiImage>({&font_, 1});
            if (!lists || !images)
            {
                return cxx::unexpected(!lists ? lists.error() : images.error());
            }
            for (std::size_t i = 0; i < lists->values.size(); ++i)
            {
                std::array<ImDrawCmd, 2> commands;
                commands[0].UserCallback = ImDrawCallback_ResetRenderState;
                commands[1].ClipRect = {0, 0, 128, 128};
                commands[1].TextureId = static_cast<ImTextureID>(font_.token);
                commands[1].ElemCount = 3;
                if (input_case == "invalid-callback" && i == 1)
                {
                    commands[1].UserCallback = [](const ImDrawList*, const ImDrawCmd*) { std::abort(); };
                }
                auto vertex_range = frame.copy<ImDrawVert>(vertices);
                auto index_range = frame.copy<ImDrawIdx>(indices);
                auto command_range = frame.copy<ImDrawCmd>(commands);
                if (!vertex_range || !index_range || !command_range)
                {
                    return cxx::unexpected(render::RenderError{render::kRuntimeCapacity});
                }
                lists->values[i] = {*vertex_range, *index_range, *command_range};
            }
            const auto& data = draw_.data();
            return frame.write(
                output_,
                {lists->range, *images, {data.DisplayPos.x, data.DisplayPos.y},
                 {data.DisplaySize.x, data.DisplaySize.y}, {data.FramebufferScale.x, data.FramebufferScale.y}}
            );
        }

        ImguiCollector(
            ui::DrawView& draw,
            render::ImguiImage& font,
            std::array<render::AttachmentId, 2>& attachments,
            const render::FrameDataDestination& destination
        ) noexcept
            : draw_(draw), font_(font), attachments_(attachments),
              output_(render::frameDataBinding<render::ImguiCapability, render::ImguiFrameData>(
                  destination.effect,
                  destination.capability,
                  destination.scope
              ))
        {
        }

        ui::DrawView& draw_;
        render::ImguiImage& font_;
        std::array<render::AttachmentId, 2>& attachments_;
        render::FrameDataBinding<render::ImguiFrameData> output_;
    };

    // Test-local ownership only. No Editor service, asset directory, Reaction or additional Scope.
    class UiTest final
    {
    public:
        UiTest(
            window::LuxWindow& window,
            std::unique_ptr<ui::Root> root,
            engine::EngineContext& context,
            render::RenderError& failure,
            window::LuxWindow* secondary_window
        ) noexcept
            : window_(window), secondary_window_(secondary_window), root_(std::move(root)),
              context_(context), engine_(context), failure_(failure)
        {
            root_->bindWindow(&window_);
        }

        ~UiTest() noexcept = default;

        // The caller keeps this complete object and Initialization alive when retirement fails.
        exec::task<Result> retire() noexcept
        {
            auto consumed = co_await rendering::RenderFrameSubmission::instance().whenSubmittedFramesConsumed();
            if (!consumed)
            {
                co_return consumed;
            }
            if (completed_frames_ != submitted_frames_ && failure_.type == 0)
            {
                failure_ = {render::kFrameContract};
            }
            auto removed = co_await removeOutput();
            if (!removed)
            {
                co_return removed;
            }
            if (scene_id_.valid())
            {
                auto retired = co_await engine_.removeScene(scene_id_);
                if (!retired)
                {
                    co_return retired;
                }
                scene_id_ = {};
                rendering_system_ = nullptr;
            }
            candidate_.reset(); // Cold candidates never started remote work.
            // Scene/Effect have already retired. Global texture release is qualified by its resource ID.
            if (!font_.texture.isNull())
            {
                auto& runtime = context_.renderClient();
                const auto texture = font_.texture;
                render::ImageCommands::Release::Request release{texture};
                auto response = co_await runtime.request<render::ImageCommands::Release>(release_font_id_, release);
                if (response.error.type != 0)
                {
                    co_return cxx::unexpected(response.error);
                }
                font_.texture = {};
                auto stale = co_await runtime.request<render::ImageCommands::Release>(release_font_id_, release);
                if (stale.error.type != error::errorId("lux.render.resource.identity"))
                {
                    co_return cxx::unexpected(render::RenderError{render::kCommandContract});
                }
                std::puts("Global texture released after Effect retirement; stale generation rejected.");
            }
            root_->bindWindow(nullptr);
            co_return Result{};
        }

        engine::Engine& engine() noexcept
        {
            return engine_;
        }

        exec::task<Result> initialize(std::string_view output) noexcept
        {
            const std::array names{
                render::CommandName{render::ImageCommands::Upload::name},
                render::CommandName{render::ImageCommands::Release::name}
            };
            auto commands = co_await context_.renderClient().request<render::QueryCommandIds>(names);
            if (commands.error.type) co_return cxx::unexpected(commands.error);
            upload_font_id_ = commands.ids[0];
            release_font_id_ = commands.ids[1];
            if (!layout_.addElement(title_) || !layout_.addElement(toggle_) || !layout_.addElement(number_))
            {
                co_return cxx::unexpected(render::RenderError{render::kRuntimeState});
            }
            auto pane = std::make_unique<ui::Pane>("LuxWindow / Engine / ImGui");
            if (!pane->addElement(layout_) || !root_->addPane(std::move(pane)))
            {
                co_return cxx::unexpected(render::RenderError{render::kRuntimeState});
            }
            co_return co_await createOutput(output);
        }

        Result run(std::uint32_t frame_limit) noexcept
        {
            engine_.addEngineStageCallback(
                [this]() noexcept -> exec::task<Result>
                {
                    if (!attachments_.front().isNull() || rendering_system_->sceneId().isNull())
                    {
                        co_return Result{};
                    }
                    auto& runtime = context_.renderClient();
                    destination_ = {rendering_system_->effectId(), 0, 0, rendering_system_->sceneId()};
                    for (std::size_t i = 0; i < targets_.size(); ++i)
                    {
                        if (targets_[i].isNull()) continue;
                        auto view = co_await runtime.request<lux::render::CreateView>({rendering_system_->sceneId()});
                        if (view.error.type != 0)
                        {
                            co_return cxx::unexpected(view.error);
                        }
                        views_[i] = view.view;
                        auto attached = co_await runtime.request<lux::render::Attach>(
                            {rendering_system_->sceneId(), views_[i], targets_[i], rendering_system_->effectId()}
                        );
                        if (attached.error.type != 0)
                        {
                            co_return cxx::unexpected(attached.error);
                        }
                        attachments_[i] = attached.attachment;
                    }

                    auto atlas = root_->fontAtlas();
                    if (!atlas)
                    {
                        co_return cxx::unexpected(render::RenderError{render::kRuntimeState});
                    }
                    rdesc::TextureInfo texture{};
                    texture.width = atlas->width;
                    texture.height = atlas->height;
                    texture.channel = 4;
                    texture.pixel_format = rdesc::ETexturePixelFormat::RGBA8_UNORM;
                    texture.color_space = rdesc::ETextureColorSpace::DATA;
                    texture.mip_ranges[0] = {
                        0,
                        atlas->pixels.size(),
                        static_cast<std::uint32_t>(texture.width),
                        static_cast<std::uint32_t>(texture.height)
                    };
                    render::ImageCommands::Upload::Request upload{
                        implementation_,
                        {texture, std::as_bytes(std::span(atlas->pixels))}
                    };
                    auto uploaded = co_await runtime.request<render::ImageCommands::Upload>(upload_font_id_, upload);
                    if (uploaded.error.type != 0)
                    {
                        co_return cxx::unexpected(uploaded.error);
                    }
                    font_.texture =
                        uploaded.texture; // Token zero is the Root font atlas; no asset directory is involved.
                    const auto* input_case = std::getenv("LUX_UI_INPUT_CASE");
                    if (input_case && std::string_view(input_case) == "no-draw")
                    {
                        for (auto target : targets_)
                        {
                            if (target.isNull()) continue;
                            auto paused = co_await runtime.request<lux::render::SetTargetActive>({target, false});
                            if (paused.error.type != 0) co_return cxx::unexpected(paused.error);
                        }
                    }
                    co_return Result{};
                },
                engine::EEngineStage::BEGIN_UPDATE
            );
            engine_.addEngineStageCallback(
                [this, previous = Clock::now()]() mutable noexcept { return prepareFrame(previous); },
                engine::EEngineStage::SIMULATION_FINISHED
            );
            engine_.addEngineStageCallback(
                [this, frame_limit]() noexcept -> Result
                {
                    ++submitted_frames_;
                    if (frame_limit != 0 && submitted_frames_ == frame_limit)
                    {
                        engine_.requestStop();
                    }
                    return {};
                },
                engine::EEngineStage::END_UPDATE
            );
            auto ran = engine_.run(
                [this]() noexcept
                {
                    window::LuxWindow::pollEvents();
                    if (window_.shouldClose())
                    {
                        engine_.requestStop();
                    }
                },
                [](Clock::time_point deadline) noexcept
                {
                    if (deadline == Clock::time_point::max())
                    {
                        window::LuxWindow::waitEvents();
                        return;
                    }
                    const auto remaining = std::chrono::duration<double>(deadline - Clock::now()).count();
                    if (remaining > 0)
                    {
                        window::LuxWindow::waitEvents(remaining);
                    }
                }
            );
            if (!ran)
            {
                return ran;
            }
            if (frame_limit != 0 && submitted_frames_ != frame_limit)
            {
                return cxx::unexpected(render::RenderError{render::kCommandCancelled});
            }
            return {};
        }

    private:
        Result prepareFrame(Clock::time_point& previous) noexcept
        {
            if (failure_.type != 0)
            {
                return cxx::unexpected(failure_);
            }
            std::uint32_t width, height, pixels_x, pixels_y;
            window_.size(width, height);
            window_.framebufferSize(pixels_x, pixels_y);
            const bool changed_extent = pixels_x != target_description_.width || pixels_y != target_description_.height;
            if (width == 0 || height == 0 || changed_extent)
            {
                return cxx::unexpected(render::RenderError{render::kRuntimeUnsupported});
            }
            input_.sample(window_);
            if (!ui::feedWindowInput(*root_, input_.snapshot()))
            {
                return cxx::unexpected(render::RenderError{render::kRuntimeState});
            }
            const auto now = Clock::now();
            const auto delta = std::max(std::chrono::duration<float>(now - previous).count(), 0.000001F);
            previous = now;
            const ui::FrameInfo frame_info{
                {float(width), float(height)},
                delta,
                {float(pixels_x) / width, float(pixels_y) / height}
            };
            if (!root_->update(frame_info, draw_))
            {
                return cxx::unexpected(render::RenderError{render::kFrameContract});
            }
            auto writer = rendering::RenderFrameSubmission::instance().writer();
            writer.onCompletion(this, &frameCompleted);
            return {};
        }

        static void frameCompleted(void* context, const render::FrameCompletion& completion) noexcept
        {
            auto& test = *static_cast<UiTest*>(context);
            const auto* input_case = std::getenv("LUX_UI_INPUT_CASE");
            if (input_case && std::string_view(input_case) == "invalid-second-attachment" &&
                test.completed_frames_ == 0)
            {
                const bool rejected_second = completion.error.type == render::kFrameContract &&
                    completion.failed_entry == 1 && completion.requires_resynchronization;
                if (!rejected_second) test.failure_ = {render::kCommandContract};
            }
            if (input_case && std::string_view(input_case) == "submit-oom" && test.completed_frames_ == 0)
            {
                const bool partial_failure = completion.error.type == render::kDeviceFailure &&
                    completion.error.args[0] == kNativeOutOfMemory && completion.submitted_batches != 0 &&
                    completion.outcome == render::EFrameOutcome::PARTIALLY_SUBMITTED &&
                    completion.requires_resynchronization;
                if (!partial_failure) test.failure_ = {render::kCommandContract};
                std::printf("UI partial preparation: accepted=%u outcome=%u resynchronize=%u\n",
                    completion.submitted_batches, unsigned(completion.outcome), completion.requires_resynchronization);
            }
            if (completion.error.type != 0 && test.failure_.type == 0)
            {
                test.failure_ = completion.error;
            }
            const bool no_draw_expected = input_case && std::string_view(input_case) == "no-draw";
            const bool valid_outcome = no_draw_expected
                ? completion.outcome == render::EFrameOutcome::NO_DRAW && completion.submitted_batches == 0
                : completion.outcome == render::EFrameOutcome::SUBMITTED && completion.submitted_batches != 0;
            if (!valid_outcome && test.failure_.type == 0)
            {
                test.failure_ = {render::kFrameContract};
            }
            ++test.completed_frames_;
        }

        exec::task<Result> createOutput(std::string_view output) noexcept
        {
            auto& runtime = context_.renderClient();
            window_.framebufferSize(target_description_.width, target_description_.height);
            target_description_.format = rdesc::ETextureFormat::RGBA8_UNORM;
            target_description_.kind = render::ETargetKind::SURFACE;
            const std::array windows{&window_, secondary_window_};
            for (std::size_t i = 0; i < windows.size(); ++i)
            {
                if (!windows[i]) continue;
                render::SurfaceDescription surface;
                if (!rendering::makeWindowSurface(windows[i]->nativeWindow(), true, surface))
                {
                    co_return cxx::unexpected(render::RenderError{render::kRuntimeUnsupported});
                }
                auto target = co_await runtime.request<lux::render::CreateTarget>({target_description_, surface});
                if (target.error.type != 0)
                {
                    co_return cxx::unexpected(target.error);
                }
                targets_[i] = target.target;
                target_description_ = target.description; // Compile against the actual surface format.
            }

            const render::EffectOutputDesc effect_output{
                target_description_.format, target_description_.samples,
                target_description_.kind, target_description_.sampled
            };
            auto implementations = co_await runtime.request<lux::render::ListCapabilities>(
                {std::string(render::kUiSceneCapability), "lux.render.scene.ui", effect_output}
            );
            if (implementations.error.type != 0)
            {
                co_return cxx::unexpected(implementations.error);
            }
            if (implementations.implementations.size() != 1)
            {
                co_return cxx::unexpected(render::RenderError{render::kCapabilityDeclaration});
            }
            const auto& implementation = implementations.implementations.front();
            if (implementation.unavailable.type != 0)
            {
                co_return cxx::unexpected(implementation.unavailable);
            }
            render::UiConfiguration ui;
            ui.index_type = sizeof(ImDrawIdx) == 2 ? render::EIndexType::UINT16 : render::EIndexType::UINT32;
            auto configuration = render::ConfigValue::make(ui);
            if (!configuration)
            {
                co_return cxx::unexpected(configuration.error());
            }
            render::CapabilityComposition effect;
            effect.contributions.push_back(
                {"ui", implementation.capability.type, implementation.implementation_id, std::move(*configuration)}
            );
            effect.outputs.push_back({"ui", std::string(output)});
            implementation_ = implementation.implementation_id;
            auto& metadata = metadata_;
            if (auto registered = metadata.addSceneReaction<rendering::ViewReaction>(); !registered)
            {
                co_return registered;
            }
            if (auto registered = metadata.addCollector<ImguiCollector>(draw_, font_, attachments_); !registered)
            {
                co_return registered;
            }
            auto world = std::make_unique<world::World>(world::WorldDescription{"UI", {}});
            auto system = rendering::RenderSystem::create(
                std::move(effect),
                effect_output,
                *world,
                context_,
                metadata
            );
            if (!system)
            {
                co_return cxx::unexpected(system.error());
            }
            candidate_ =
                std::make_unique<scene::SceneInstance>(std::move(world), std::make_unique<simulation::Simulation>());
            rendering_system_ = system->get();
            candidate_->add(std::move(*system));
            auto added = engine_.addScene(candidate_);
            if (!added)
            {
                co_return cxx::unexpected(added.error());
            }
            scene_id_ = *added;
            co_return co_await engine_.whenSceneInitialized(scene_id_);
        }

        exec::task<Result> removeOutput() noexcept
        {
            auto& runtime = context_.renderClient();
            for (std::size_t i = targets_.size(); i-- != 0;)
            {
                if (!attachments_[i].isNull())
                {
                    auto response = co_await runtime.request<lux::render::Detach>({attachments_[i]});
                    if (response.error.type != 0) co_return cxx::unexpected(response.error);
                    attachments_[i] = {};
                }
                if (!views_[i].isNull())
                {
                    const auto scene = std::get<render::SceneId>(destination_.scope);
                    auto response = co_await runtime.request<lux::render::RemoveView>({scene, views_[i]});
                    if (response.error.type != 0) co_return cxx::unexpected(response.error);
                    views_[i] = {};
                }
                if (!targets_[i].isNull())
                {
                    auto response = co_await runtime.request<lux::render::RemoveTarget>({targets_[i]});
                    if (response.error.type != 0) co_return cxx::unexpected(response.error);
                    targets_[i] = {};
                }
            }
            co_return Result{};
        }

        window::LuxWindow& window_;
        window::LuxWindow* secondary_window_;
        ui::Label title_{"LuxWindow -> Collector -> RenderSystem -> NRI"};
        ui::CheckBox toggle_{"Interactive checkbox", false};
        ui::NumericEdit number_{0.5F};
        ui::Layout layout_{ui::ELayoutType::VERTICAL};
        std::unique_ptr<ui::Root> root_; // Root/Pane die before their borrowed controls.
        input::Input input_;
        ui::DrawView draw_;
        render::ImguiImage font_;
        render::CommandId upload_font_id_;
        render::CommandId release_font_id_;
        rendering::RenderCapabilityMetaRegistry metadata_;
        rendering::RenderSystem* rendering_system_ = nullptr;
        scene::SceneInstanceId scene_id_;
        render::FrameDataDestination destination_;
        render::CapabilityImplementationId implementation_;
        render::TargetDesc target_description_;
        std::array<render::TargetId, 2> targets_;
        std::array<render::ViewId, 2> views_;
        std::array<render::AttachmentId, 2> attachments_;
        std::unique_ptr<scene::SceneInstance> candidate_; // addScene consumes only on success.
        engine::EngineContext& context_;
        engine::Engine engine_; // Systems die before UI, asset services and collector inputs.
        render::RenderError& failure_;
        std::uint32_t submitted_frames_ = 0;
        std::uint32_t completed_frames_ = 0;
    };
} // namespace

int main(int argc, char** argv)
{
    std::uint32_t frames = 0;
    const bool reject_collector = argc == 2 && std::string_view(argv[1]) == "--reject-collector";
    const bool reject_effect = argc == 2 && std::string_view(argv[1]) == "--reject-effect";
    ImguiCollector::reject_creation = reject_collector;
    if (argc != 1 && !reject_collector && !reject_effect)
    {
        const std::string_view value = argc == 3 ? argv[2] : "";
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), frames);
        const bool valid_count = parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && frames != 0;
        const bool valid_arguments = argc == 3 && std::string_view(argv[1]) == "--frames" && valid_count;
        if (!valid_arguments)
        {
            std::fprintf(stderr, "usage: engine_ui_test [--frames N | --reject-collector | --reject-effect]\n");
            return 2;
        }
    }
    auto platform = lux::window::GlfwRuntime::create();
    if (!platform)
    {
        return 1;
    }
    // The native window outlives EngineContext even if remote removal fails: Server joins before HWND destruction.
    auto window = lux::window::LuxWindow::create({960, 640, "Lux Engine UI test"});
    if (!window)
    {
        return 1;
    }
    std::unique_ptr<lux::window::LuxWindow> secondary_window;
    if (std::getenv("LUX_UI_TWO_WINDOWS"))
    {
        auto created = lux::window::LuxWindow::create({960, 640, "Lux Engine UI second output"});
        if (!created) return 1;
        secondary_window = std::move(*created);
    }
    lux::render::RenderServerConfig configuration;
    configuration.channel_name = "lux.engine.test.ui";
    configuration.uploads.emplace();
    if (const auto* fif = std::getenv("LUX_UI_FIF"))
    {
        configuration.frames_in_flight = static_cast<std::uint32_t>(std::atoi(fif));
    }
    if (std::getenv("LUX_UI_SMALL_UPLOAD_WINDOW"))
    {
        // Force vertices and indices through separate accepted windows, including staging exhaustion.
        configuration.uploads->chunk_bytes = 65536;
        configuration.uploads->staging_pages = 1;
        configuration.uploads->window_regions = 1;
    }
    lux::render::DeviceOptions device;
    device.texture_descriptors = true;
    auto initialized = lux::engine::init(
        {.cpu_concurrency = 2, .timer = {16}, .blocking = execution::BlockingSchedulerConfig{2, 32}},
        {64U << 20U, 128U << 20U, 16},
        std::move(configuration),
        device
    );
    if (!initialized)
    {
        std::fprintf(stderr, "Initialization failed: %s\n", lux::error::format(initialized.error()).c_str());
        return 1;
    }
    auto& execution = lux::execution::ExecutionRuntime::instance();
    execution.setWake(&lux::window::LuxWindow::wakeEvents);
    lux::cxx::scope_exit remove_wake([&execution]() noexcept { execution.setWake(nullptr); });
    lux::render::RenderError failure;
    {
        auto root = lux::ui::Root::create({.docking = false});
        if (!root)
        {
            return 1;
        }
        UiTest sample{**window, std::move(*root), **initialized, failure, secondary_window.get()};
        if (auto ready = runTask(sample.initialize(reject_effect ? "absent-output" : "color")); !ready)
        {
            failure = ready.error();
        }
        else if (auto ran = sample.run(frames); !ran)
        {
            failure = ran.error();
        }
        auto retired = runTask(sample.retire());
        if (!retired)
        {
            std::fprintf(stderr, "UI retirement contract failed: %s\n", lux::error::format(retired.error()).c_str());
            std::abort(); // Failed test invariant: do not unwind owners whose remote release was rejected.
        }
    }
    if (reject_collector)
    {
        return failure.type == render::kRuntimeCapacity ? 0 : 1;
    }
    if (reject_effect)
    {
        return failure.type == error::errorId("lux.render.feature.missing_provider") ? 0 : 1;
    }
    const auto* input_case = std::getenv("LUX_UI_INPUT_CASE");
    if (input_case && std::string_view(input_case) == "submit-oom")
    {
        // NRI OUT_OF_MEMORY=3 is injected externally before native submission, after real accepted copies.
        if (failure.type != render::kDeviceFailure || failure.args[0] != kNativeOutOfMemory) return 1;
        std::puts("UI partial submission failure delivered; frame sources and remote owners settled.");
        return 0;
    }
    const bool expected_input_failure = input_case &&
        (std::string_view(input_case) == "invalid-callback" ||
         std::string_view(input_case) == "invalid-second-attachment");
    if (expected_input_failure)
    {
        if (failure.type != render::kFrameContract) return 1;
        std::puts("UI invalid callback rejected after the first list; accepted frame and owners settled.");
        return 0;
    }
    if (failure.type != 0)
    {
        std::fprintf(stderr, "UI test failed: %s\n", lux::error::format(failure).c_str());
        return 1;
    }
    std::puts("UI frame submission and logical cleanup completed.");
    return 0;
}
