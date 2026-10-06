#include <algorithm>
#include <lux/engine/editor/EditorUiScene.hpp>
#include <lux/engine/editor/detail/UiFrame.hpp>
#include <lux/engine/editor/detail/UiRenderSyncStage.hpp>
#include <lux/engine/process/CompletionWork.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneError.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>

namespace lux::editor
{
    namespace
    {
        error::Error descriptionError(const scene::SceneDescriptionFailure& failure) noexcept
        {
            return error::makeError(
                {"lux.scene.description",
                 "Scene description code {0}, system {1}, subject {2}",
                 error::ERecovery::NEEDS_INPUT,
                 {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::HEX}},
                {static_cast<std::uint64_t>(failure.code), failure.system.value, failure.subject_hash}
            );
        }
    } // namespace
    struct EditorUiScene::Impl final
    {
        Impl(
            process::ExecutionRuntime& execution,
            lux::scene::SceneRuntime& scenes,
            lux::scene::RenderResources& resources,
            std::optional<scene::ViewConfig> output
        )
            : scenes_(scenes), resources_(resources), output_(std::move(output)),
              work_(execution, this, [](void* value) noexcept { static_cast<Impl*>(value)->collectCompletions(); }),
              completion_(std::make_shared<process::CompletionWork::Request>(work_.requester()))
        {
            for (auto& slot : frames_)
            {
                slot = std::make_shared<lux::ui::RenderFrame>();
            }
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

        FrameworkResult<void> initialize(std::vector<std::byte> configuration, render::RenderRuntime& runtime) noexcept
        {
            const auto registration = lux::scene::builtinRenderSystemRegistration();
            const auto codec = render::UiRenderConfigurationCodec();
            lux::scene::RenderSystemConfiguration render_configuration;
            render_configuration.features.push_back(
                {render::kUiRenderDescriptor.type,
                 std::move(configuration),
                 std::string(codec.schema),
                 codec.schema_version}
            );
            std::vector<std::byte> bytes;
            const auto encoded = registration.configuration.encode(&render_configuration, bytes);
            if (!encoded)
            {
                return lux::cxx::unexpected(error::makeError(
                    {"lux.ui.configuration_encode",
                     "UI configuration encoding code {0}, offset {1}",
                     error::ERecovery::NEEDS_INPUT,
                     {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
                    {static_cast<std::uint64_t>(encoded.error().code), encoded.error().offset}
                ));
            }
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
            {
                return lux::cxx::unexpected(descriptionError(added.error()));
            }
            auto resolved = std::move(description).buildResolved();
            if (!resolved)
            {
                return lux::cxx::unexpected(descriptionError(resolved.error()));
            }
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
            {
                return lux::cxx::unexpected(scene::toError(built.error()));
            }
            scene_ = std::move(*built);
            if (!scenes_.pauseSimulation(scene_.id()))
            {
                render::renderFatal("New UI scene identity was rejected");
            }
            auto borrowed = scenes_.borrowInstance(scene_.id());
            if (!borrowed)
            {
                return lux::cxx::unexpected(scene::toError(borrowed.error()));
            }
            auto& registry = borrowed->get();
            input_ = registry.create();
            registry.emplace<detail::UiFrame>(input_);
            if (output_)
            {
                desired_extent_ = output_->extent;
                output_request_ = registry.create();
                registry.emplace<lux::scene::RenderViewRequest>(
                    output_request_,
                    RenderSystemId,
                    simulation::ecs::NullEntity,
                    *output_,
                    std::uint64_t{1},
                    output_stop_.get_token()
                );
            }
            const auto bound = runtime.bindProgress(
                completion_,
                [](void* value) noexcept { static_cast<process::CompletionWork::Request*>(value)->request(); }
            );
            if (!bound)
            {
                return lux::cxx::unexpected(render::toError(bound.error()));
            }
            return {};
        }

        void collectOutputReceipt() noexcept
        {
            if (!output_ || !bool(scene_))
            {
                return;
            }
            const auto registry = std::as_const(scenes_).borrowInstance(scene_.id());
            if (!registry)
            {
                return;
            }
            const auto* result = registry->get().try_get<lux::scene::RenderViewResult>(output_request_);
            if (!result)
            {
                return;
            }
            if (result->failure)
            {
                failure_ = render::toError(*result->failure);
            }
            if (result->view.isValid())
            {
                if (auto receipt = resources_.viewReceipt(result->view))
                {
                    output_receipt_ = *receipt;
                }
            }
        }

        bool outputReady() noexcept
        {
            collectOutputReceipt();
            return !failure_ && (!output_ || output_receipt_.status().status.state == lux::scene::EViewState::READY);
        }
        static bool reusable(const std::shared_ptr<lux::ui::RenderFrame>& frame) noexcept
        {
            return frame.use_count() == 1 && frame->submission.complete();
        }
        bool hasWritableFrame() const noexcept
        {
            return !frames_stopped_ && !failure_ && !pending_ && std::ranges::any_of(frames_, &Impl::reusable);
        }
        void collectCompletions() noexcept
        {
            for (auto& frame : frames_)
            {
                if (reusable(frame))
                {
                    frame->resources.clear();
                }
            }
        }
        lux::ui::DrawData* tryAcquireDrawData() noexcept
        {
            if (frames_stopped_ || failure_ || pending_)
            {
                return nullptr;
            }
            const auto slot = std::ranges::find_if(frames_, &Impl::reusable);
            if (slot == frames_.end())
            {
                return nullptr;
            }
            current_frame_ = static_cast<std::size_t>(slot - frames_.begin());
            (*slot)->resources.clear();
            return &(*slot)->draw_data;
        }
        lux::cxx::expected<void, lux::ui::ECaptureError> captureDrawData(const lux::ui::DrawData& data) noexcept
        {
            const bool is_valid_slot = current_frame_ < frames_.size() && &frames_[current_frame_]->draw_data == &data;
            if (!is_valid_slot || frames_stopped_ || pending_)
            {
                return lux::cxx::unexpected(lux::ui::ECaptureError::FRAME_OPEN);
            }
            auto& frame = *frames_[current_frame_];
            if (!data.textures().empty())
            {
                auto captured = resources_.captureTextures(data.textures());
                if (!captured)
                {
                    failure_ = render::toError(captured.error());
                    return lux::cxx::unexpected(lux::ui::ECaptureError::INVALID_INPUT);
                }
                frame.resources.push_back(std::move(*captured));
            }
            frame.sequence = ++captures_;
            pending_ = frames_[current_frame_];
            return {};
        }
        void stopFrames() noexcept
        {
            if (std::exchange(frames_stopped_, true))
            {
                return;
            }
            output_stop_.request_stop();
            pending_.reset();
            clear_pending_ = true;
        }
        FrameworkResult<void> applySceneInput() noexcept
        {
            if (failure_)
            {
                return lux::cxx::unexpected(*failure_);
            }
            if (!pending_ && !clear_pending_ && !extent_pending_)
            {
                return {};
            }
            auto borrowed = scenes_.borrowInstance(scene_.id());
            if (!borrowed)
            {
                const auto* error = std::get_if<lux::scene::ESceneRuntimeError>(&borrowed.error().cause);
                if (error && *error == lux::scene::ESceneRuntimeError::BUSY)
                {
                    return {};
                }
                return lux::cxx::unexpected(scene::toError(borrowed.error()));
            }
            auto& registry = borrowed->get();
            if (extent_pending_ && output_)
            {
                registry.patch<scene::RenderViewRequest>(
                    output_request_,
                    [&](auto& request)
                    {
                        request.configuration.extent = desired_extent_;
                        ++request.revision;
                    }
                );
                extent_pending_ = false;
            }
            if (pending_ || clear_pending_)
            {
                registry.patch<detail::UiFrame>(input_, [&](auto& value) { value.frame = std::move(pending_); });
                clear_pending_ = false;
                current_frame_ = frames_.size();
            }
            return {};
        }

        inline static constexpr system::SystemInstanceId RenderSystemId{1};
        lux::scene::SceneRuntime& scenes_;
        lux::scene::RenderResources& resources_;
        std::optional<scene::ViewConfig> output_;
        render::PixelExtent desired_extent_{};
        bool extent_pending_{};
        process::CompletionWork work_;
        std::shared_ptr<process::CompletionWork::Request> completion_;
        lux::scene::SceneInstanceLease scene_;
        lux::scene::InstanceRetirement retirement_;
        simulation::ecs::Entity input_{simulation::ecs::NullEntity}, output_request_{simulation::ecs::NullEntity};
        std::stop_source output_stop_;
        lux::scene::RenderViewReceipt output_receipt_;
        std::array<std::shared_ptr<lux::ui::RenderFrame>, render::TRenderProgramChannel<>::request_slot_count + 1>
            frames_;
        std::shared_ptr<const lux::ui::RenderFrame> pending_;
        std::size_t current_frame_{frames_.size()};
        bool frames_stopped_{}, clear_pending_{};
        std::uint64_t captures_{};
        std::optional<error::Error> failure_;
    };

    FrameworkResult<std::unique_ptr<EditorUiScene>> EditorUiScene::create(
        process::ExecutionRuntime& execution,
        scene::SceneRuntime& scenes,
        render::RenderRuntime& runtime,
        scene::RenderResources& resources,
        std::vector<std::byte> configuration,
        std::optional<scene::ViewConfig> output
    ) noexcept
    {
        if (!resources.uses(runtime))
        {
            return cxx::unexpected(render::toError(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT}));
        }
        auto impl = std::make_unique<Impl>(execution, scenes, resources, std::move(output));
        auto initialized = impl->initialize(std::move(configuration), runtime);
        if (!initialized)
        {
            return cxx::unexpected(std::move(initialized.error()));
        }
        return std::unique_ptr<EditorUiScene>{new EditorUiScene(std::move(impl))};
    }
    EditorUiScene::EditorUiScene(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    EditorUiScene::~EditorUiScene() noexcept = default;
    void EditorUiScene::setExtent(render::PixelExtent extent) noexcept
    {
        if (impl_->desired_extent_ != extent)
        {
            impl_->desired_extent_ = extent;
            impl_->extent_pending_ = true;
        }
    }
    bool EditorUiScene::outputReady() noexcept
    {
        return impl_->outputReady();
    }
    bool EditorUiScene::hasWritableFrame() const noexcept
    {
        return impl_->hasWritableFrame();
    }
    ui::DrawData* EditorUiScene::acquireDrawData() noexcept
    {
        return impl_->tryAcquireDrawData();
    }
    cxx::expected<void, ui::ECaptureError> EditorUiScene::captureDrawData(const ui::DrawData& data) noexcept
    {
        return impl_->captureDrawData(data);
    }
    FrameworkResult<void> EditorUiScene::publishInput() noexcept
    {
        return impl_->applySceneInput();
    }
    void EditorUiScene::stopFrames() noexcept
    {
        impl_->stopFrames();
    }
    std::uint64_t EditorUiScene::capturedFrames() const noexcept
    {
        return impl_->captures_;
    }
    scene::SceneInstanceId EditorUiScene::sceneId() const noexcept
    {
        return impl_->scene_.id();
    }
} // namespace lux::editor
