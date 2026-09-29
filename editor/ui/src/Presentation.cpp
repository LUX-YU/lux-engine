#include <lux/engine/editor/ui/Presentation.hpp>
#include <lux/engine/editor/ui/detail/UiFrame.hpp>
#include <lux/engine/editor/ui/detail/UiRenderSyncStage.hpp>
#include <lux/engine/editor/ui/WindowOutput.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/process/CompletionWork.hpp>
#include <algorithm>

namespace lux::editor::ui
{
    struct Presentation::Impl final
    {
        Impl(
            process::ExecutionRuntime& execution,
            lux::scene::SceneRuntime& scenes,
            lux::scene::RenderResources& resources,
            window::LuxWindow* window
        )
            : execution_(execution), scenes_(scenes), resources_(resources), window_(window),
              work_(execution, this, [](void* value) noexcept { static_cast<Impl*>(value)->collectCompletions(); }),
              completion_(std::make_shared<process::CompletionWork::Request>(work_.requester()))
        {
            for (auto& slot : frames_)
                slot = std::make_shared<lux::ui::RenderFrame>();
        }
        ~Impl() noexcept
        {
            collectOutputReceipt();
            stopFrames();
            work_.cancel();
            retirement_ = scene_.retire();
            // The application keeps the native window and shared RenderContext alive
            // through SceneRuntime's final retirement drain. No callback blocks here.
        }

        EditorResult<void> initialize(lux::ui::Root& root, render::RenderRuntime& runtime) noexcept
        {
            auto configuration = lux::ui::makeRenderConfiguration(root);
            if (!configuration)
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::FRONTEND_FAILURE, "ui.configuration", 0, {}, configuration.error()}
                );
            const auto registration = lux::scene::builtinRenderSystemRegistration();
            const auto codec = render::UiRenderConfigurationCodec();
            lux::scene::RenderSystemConfiguration render_configuration;
            render_configuration.features.push_back(
                {render::kUiRenderDescriptor.type,
                 std::move(*configuration),
                 std::string(codec.schema),
                 codec.schema_version}
            );
            std::vector<std::byte> bytes;
            const auto encoded = registration.configuration.encode(&render_configuration, bytes);
            if (!encoded)
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::FRONTEND_FAILURE, "ui.configuration", 0, {}, encoded.error()}
                );
            lux::scene::SceneDescriptionBuilder description;
            auto added = description.addSystem(
                RenderSystemId,
                "Editor UI",
                registration.type,
                registration.description->version,
                registration.description->configuration_schema_name,
                registration.description->configuration_schema_version,
                bytes
            );
            if (!added)
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::FRONTEND_FAILURE, "ui.description", 0, {}, added.error()}
                );
            auto resolved = std::move(description).buildResolved();
            if (!resolved)
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::FRONTEND_FAILURE, "ui.description", 0, {}, resolved.error()}
                );
            const simulation::ecs::ComponentSchemaSet components;
            const simulation::SimulationSystemRegistry systems;
            const std::array bindings{detail::uiRenderFeatureBinding()};
            lux::scene::RenderFeatureSceneBindings binding_view{bindings};
            const std::array providers{
                lux::scene::makeSceneCapabilityProvider<render::RenderRuntime>(
                    "runtime",
                    "lux.render.runtime",
                    runtime
                ),
                lux::scene::makeSceneCapabilityProvider<lux::scene::RenderResources>(
                    "resources",
                    "lux.render.resources",
                    resources_
                ),
                lux::scene::makeSceneCapabilityProvider<lux::scene::RenderFeatureSceneBindings>(
                    "bindings",
                    "lux.render.scene_bindings",
                    binding_view
                )
            };
            auto builder = scenes_.builder();
            builder.setDescription(std::make_shared<const lux::scene::SceneDescription>(std::move(*resolved)))
                .setWorld(std::make_shared<const world::WorldDescription>())
                .setSimulation(std::make_shared<const simulation::SimulationDescription>())
                .setRegistrations(components, systems, std::span{&registration, 1})
                .setProviders(providers);
            auto built = builder.build();
            if (!built)
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::FRONTEND_FAILURE, "ui.scene", 0, {}, built.error()}
                );
            scene_ = std::move(*built);
            if (!scenes_.pauseSimulation(scene_.id()))
                render::renderFatal("New UI scene identity was rejected");
            auto borrowed = scenes_.borrowInstance(scene_.id());
            if (!borrowed)
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::FRONTEND_FAILURE, "ui.registry", 0, {}, borrowed.error()}
                );
            auto& registry = borrowed->get();
            input_ = registry.create();
            registry.emplace<detail::UiFrame>(input_);
            if (window_)
            {
                auto surface = windowOutput(*window_);
                if (!surface)
                    return lux::cxx::unexpected(surface.error());
                std::uint32_t width{}, height{};
                window_->framebufferSize(width, height);
                lux::scene::ViewConfig config{.extent = {width, height}, .output = *surface};
                output_request_ = registry.create();
                registry.emplace<lux::scene::RenderViewRequest>(
                    output_request_,
                    RenderSystemId,
                    simulation::ecs::NullEntity,
                    config,
                    std::uint64_t{1},
                    output_stop_.get_token()
                );
            }
            const auto bound = runtime.bindProgress(completion_, [](void* value) noexcept {
                static_cast<process::CompletionWork::Request*>(value)->request();
            });
            if (!bound)
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::FRONTEND_FAILURE, "ui.progress", 0, {}, bound.error()}
                );
            return {};
        }

        void collectOutputReceipt() noexcept
        {
            if (!window_ || !bool(scene_))
                return;
            const auto registry = std::as_const(scenes_).borrowInstance(scene_.id());
            if (!registry)
                return;
            const auto* result = registry->get().try_get<lux::scene::RenderViewResult>(output_request_);
            if (!result)
                return;
            if (result->failure)
                failure_ = EditorFailure{EEditorError::FRONTEND_FAILURE, "ui.output", 0, {}, *result->failure};
            if (result->view.isValid())
                if (auto receipt = resources_.viewReceipt(result->view))
                    output_receipt_ = *receipt;
        }

        lux::ui::FrameInfo frameInfo() noexcept
        {
            if (!window_ || frames_stopped_ || failure_)
                return {};
            std::uint32_t width{}, height{}, pixels_x{}, pixels_y{};
            window_->size(width, height);
            window_->framebufferSize(pixels_x, pixels_y);
            if (auto borrowed = scenes_.borrowInstance(scene_.id()))
            {
                auto& registry = borrowed->get();
                const render::PixelExtent extent{pixels_x, pixels_y};
                if (registry.get<lux::scene::RenderViewRequest>(output_request_).configuration.extent != extent)
                    registry.patch<lux::scene::RenderViewRequest>(output_request_, [&](auto& request) {
                        request.configuration.extent = extent;
                        ++request.revision;
                    });
            }
            collectOutputReceipt();
            if (failure_)
                return {};
            const bool has_extent = width && height && pixels_x && pixels_y;
            const bool is_ready = output_receipt_.status().status.state == lux::scene::EViewState::READY;
            if (!has_extent || window_->minimized() || !is_ready)
                return {};
            const auto elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - last_frame_).count();
            return {
                {float(width), float(height)},
                std::clamp(elapsed, 0.001F, 0.1F),
                {float(pixels_x) / width, float(pixels_y) / height}
            };
        }
        static bool reusable(const std::shared_ptr<lux::ui::RenderFrame>& frame) noexcept
        {
            return frame.use_count() == 1 && frame->submission.complete();
        }
        std::chrono::steady_clock::time_point nextFrameTime() const noexcept
        {
            // A pending input can become writable in the just-finished Runtime
            // traversal. Retry once at that safe point without spinning on GPU waits.
            if (pending_ && scenes_.borrowInstance(scene_.id()))
                return std::chrono::steady_clock::now();
            const bool has_output = !window_ || (!window_->minimized() && output_receipt_.status().status.state ==
                                                                              lux::scene::EViewState::READY);
            const bool can_present = !frames_stopped_ && !failure_ && !pending_ && has_output &&
                                     std::ranges::any_of(frames_, &Impl::reusable);
            return can_present ? next_frame_ : std::chrono::steady_clock::time_point::max();
        }
        void collectCompletions() noexcept
        {
            for (auto& frame : frames_)
                if (reusable(frame))
                    frame->resources.clear();
        }
        lux::ui::DrawData* tryAcquireDrawData() noexcept
        {
            if (frames_stopped_ || failure_ || pending_)
                return nullptr;
            if (std::chrono::steady_clock::now() < nextFrameTime())
                return nullptr;
            const auto slot = std::ranges::find_if(frames_, &Impl::reusable);
            if (slot == frames_.end())
                return nullptr;
            current_frame_ = static_cast<std::size_t>(slot - frames_.begin());
            (*slot)->resources.clear();
            return &(*slot)->draw_data;
        }
        lux::cxx::expected<void, lux::ui::ECaptureError> captureDrawData(const lux::ui::DrawData& data) noexcept
        {
            const bool is_valid_slot = current_frame_ < frames_.size() && &frames_[current_frame_]->draw_data == &data;
            if (!is_valid_slot || frames_stopped_ || pending_)
                return lux::cxx::unexpected(lux::ui::ECaptureError::FRAME_OPEN);
            auto& frame = *frames_[current_frame_];
            if (!data.textures().empty())
            {
                auto captured = resources_.captureTextures(data.textures());
                if (!captured)
                {
                    failure_ = EditorFailure{EEditorError::FRONTEND_FAILURE, "ui.textures", 0, {}, captured.error()};
                    return lux::cxx::unexpected(lux::ui::ECaptureError::INVALID_INPUT);
                }
                frame.resources.push_back(std::move(*captured));
            }
            frame.sequence = ++captures_;
            last_frame_ = std::chrono::steady_clock::now();
            next_frame_ = last_frame_ + std::chrono::milliseconds(16);
            pending_ = frames_[current_frame_];
            return {};
        }
        void stopFrames() noexcept
        {
            if (std::exchange(frames_stopped_, true))
                return;
            output_stop_.request_stop();
            pending_.reset();
            clear_pending_ = true;
        }
        EditorResult<void> applySceneInput() noexcept
        {
            if (failure_)
                return lux::cxx::unexpected(*failure_);
            if (!pending_ && !clear_pending_)
                return {};
            auto borrowed = scenes_.borrowInstance(scene_.id());
            if (!borrowed)
            {
                const auto* error = std::get_if<lux::scene::ESceneRuntimeError>(&borrowed.error().cause);
                if (error && *error == lux::scene::ESceneRuntimeError::BUSY)
                    return {};
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::FRONTEND_FAILURE, "ui.input", 0, {}, borrowed.error()}
                );
            }
            borrowed->get().patch<detail::UiFrame>(input_, [&](auto& value) { value.frame = std::move(pending_); });
            clear_pending_ = false;
            current_frame_ = frames_.size();
            return {};
        }

        inline static constexpr system::SystemInstanceId RenderSystemId{1};
        process::ExecutionRuntime& execution_;
        lux::scene::SceneRuntime& scenes_;
        lux::scene::RenderResources& resources_;
        window::LuxWindow* window_;
        process::CompletionWork work_;
        std::shared_ptr<process::CompletionWork::Request> completion_;
        lux::scene::SceneInstanceLease scene_;
        lux::scene::InstanceRetirement retirement_;
        simulation::ecs::Entity input_{simulation::ecs::NullEntity}, output_request_{simulation::ecs::NullEntity};
        std::stop_source output_stop_;
        lux::scene::RenderViewReceipt output_receipt_;
        std::chrono::steady_clock::time_point last_frame_{std::chrono::steady_clock::now()}, next_frame_{};
        std::array<std::shared_ptr<lux::ui::RenderFrame>, render::TRenderProgramChannel<>::request_slot_count + 1>
            frames_;
        std::shared_ptr<const lux::ui::RenderFrame> pending_;
        std::size_t current_frame_{frames_.size()};
        bool frames_stopped_{}, clear_pending_{};
        std::uint64_t captures_{};
        std::optional<EditorFailure> failure_;
    };

    EditorResult<std::unique_ptr<Presentation>> Presentation::create(
        lux::ui::Root& root,
        process::ExecutionRuntime& execution,
        lux::scene::SceneRuntime& scenes,
        render::RenderRuntime& runtime,
        lux::scene::RenderResources& resources,
        window::LuxWindow* window
    ) noexcept
    {
        if (!resources.uses(runtime) || !root.isOnAffinityThread())
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "ui.presentation"});
        auto impl = std::make_unique<Impl>(execution, scenes, resources, window);
        const auto initialized = impl->initialize(root, runtime);
        if (!initialized)
            return lux::cxx::unexpected(initialized.error());
        return std::unique_ptr<Presentation>(new Presentation(std::move(impl)));
    }
    Presentation::Presentation(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    Presentation::~Presentation() noexcept = default;
    lux::ui::FrameInfo Presentation::frameInfo() noexcept
    {
        return impl_->frameInfo();
    }
    std::chrono::steady_clock::time_point Presentation::nextFrameTime() const noexcept
    {
        return impl_->nextFrameTime();
    }
    lux::ui::DrawData* Presentation::tryAcquireDrawData() noexcept
    {
        return impl_->tryAcquireDrawData();
    }
    lux::cxx::expected<void, lux::ui::ECaptureError> Presentation::captureDrawData(const lux::ui::DrawData& data
    ) noexcept
    {
        return impl_->captureDrawData(data);
    }
    EditorResult<void> Presentation::applySceneInput() noexcept
    {
        return impl_->applySceneInput();
    }
    void Presentation::stopFrames() noexcept
    {
        impl_->stopFrames();
    }
    std::uint64_t Presentation::capturedFrames() const noexcept
    {
        return impl_->captures_;
    }
    lux::scene::SceneInstanceId Presentation::sceneId() const noexcept
    {
        return impl_->scene_.id();
    }
}
