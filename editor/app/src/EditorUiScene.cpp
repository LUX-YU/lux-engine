#include <algorithm>
#include <exec/task.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/detail/EditorUiScene.hpp>
#include <lux/engine/editor/detail/RenderCommandRequest.hpp>
#include <lux/engine/render/features/UiRasterFeature.hpp>
#include <lux/engine/rendering/RenderDiagnostics.hpp>
#include <lux/engine/rendering/RenderExecution.hpp>
#include <lux/engine/rendering/UiDrawInput.hpp>

namespace lux::editor
{
    using namespace render;
    using namespace rendering;

    using detail::requestRenderCommand;

    struct EditorUiScene::Impl final
    {
        using Transaction = exec::task<RenderResult<void>>;

        Impl(RenderExecution& input, EditorWindow& window, TargetDesc description, SurfaceDescription surface)
            : rendering(input), window(window), description(description), desired_description(description),
              surface(surface)
        {
        }

        void finishTransaction(bool cleanup, std::uint64_t revision, RenderError error) noexcept
        {
            transaction_pending = false;
            failure = error;
            failed_revision = revision;
            if (closing && !cleanup)
            {
                failure = {}; // Configuration failure does not discharge the handles it already created.
            }
            completeImageRemovals(revision, closing ? RenderError{} : error);
            if (error.type != 0 && !closing)
            {
                reportRenderFailure(error, "Configure UI output");
            }
            requestConfiguration();
        }

        RenderResult<void> startTransaction(Transaction work) noexcept
        {
            transaction_pending = true;
            const bool cleanup = closing;
            const auto revision = requested_revision;
            auto result =
                execution::taskResult(stdexec::starts_on(rendering.execution().mainScheduler(), std::move(work)));
            auto accepted = rendering.execution().spawn(stdexec::then(
                std::move(result),
                [this, cleanup, revision](auto result) noexcept
                {
                    const auto error = result                           ? RenderError{}
                                       : result.error().domainFailure() ? *result.error().domainFailure()
                                                                        : RenderError{kRuntimeState};
                    finishTransaction(cleanup, revision, error);
                }
            ));
            if (!accepted)
            {
                transaction_pending = false;
                failure = execution::toError(accepted.error());
                failed_revision = revision;
                completeImageRemovals(revision, failure);
                return cxx::unexpected(failure);
            }
            return {};
        }

        bool needsConfiguration() const noexcept
        {
            if (closing)
            {
                return !scene.isNull() || !target.isNull() || !effect.isNull() || !effect_to_release.isNull() ||
                       !font.isNull();
            }
            const bool needs_output =
                desired_active && (target.isNull() || !output_available || description != desired_description);
            const bool needs_pause = !desired_active && applied_active && !target.isNull();
            const bool needs_images = !target.isNull() && desired_image_revision != applied_image_revision;
            return needs_output || needs_pause || needs_images;
        }

        void requestConfiguration() noexcept
        {
            const bool rejected_intent = failure.type != 0 && failed_revision == requested_revision;
            if (transaction_pending || rejected_intent || !needsConfiguration())
            {
                return;
            }
            failure = {};
            static_cast<void>(startTransaction(configure()));
        }

        Transaction configure() noexcept
        {
            for (;;)
            {
                auto result = closing ? co_await close() : co_await reconfigure();
                if (result || result.error().type != kDeviceBusy)
                {
                    co_return result;
                }
                // Resize/attachment mutations occupy a real submitted frame. Await its terminal
                // responses rather than observing Server ticks; subsequent frames cannot extend this wait.
                co_await rendering.mainClient().whenSubmittedFramesComplete();
            }
        }

        bool referencesImage(rdesc::UiImageToken token) const noexcept
        {
            const bool adopted =
                std::ranges::any_of(image_selections, [&](const auto& image) { return image.token == token; });
            const bool preparing =
                std::ranges::any_of(configuring_images, [&](const auto& image) { return image.token == token; });
            return adopted || preparing;
        }

        void completeImageRemovals(std::uint64_t attempted_revision, RenderError error) noexcept
        {
            ImageRemoval* ready = nullptr;
            auto** link = &image_removals;
            while (*link)
            {
                auto* removal = *link;
                const bool removed = !referencesImage(removal->token);
                const bool failed = error.type != 0 && removal->revision <= attempted_revision;
                if (removed || failed)
                {
                    *link = removal->next;
                    removal->next = std::exchange(ready, removal);
                }
                else
                {
                    link = &removal->next;
                }
            }
            while (ready)
            {
                auto* removal = ready;
                ready = removal->next;
                const bool removed = !referencesImage(removal->token);
                EditorUiScene::deliverImageRemoval(
                    *removal,
                    removed ? FrameworkResult<void>{} : cxx::unexpected(error)
                );
            }
        }

        Transaction initialize(ui::FontAtlas atlas) noexcept
        {
            auto& client = rendering.mainClient();
            const auto features = co_await requestRenderCommand<ListFeatures>(client, {});
            if (features.error.type != 0)
            {
                co_return cxx::unexpected(features.error);
            }
            const auto selected =
                std::ranges::find(features.features, UiRasterFeatureDescriptor::name.name(), &FeatureInfo::name);
            if (selected == features.features.end())
            {
                co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
            feature = selected->id;
            const auto storages = co_await requestRenderCommand<ListStorages>(client, {});
            if (storages.error.type != 0)
            {
                co_return cxx::unexpected(storages.error);
            }
            const auto type = std::ranges::find(storages.storages, kTextureStorageType.name(), &StorageInfo::name);
            if (type == storages.storages.end())
            {
                co_return cxx::unexpected(RenderError{kStorageFactory});
            }
            const auto storage =
                co_await requestRenderCommand<PrepareStorage>(client, {{GlobalStorageLocation{}, type->id, "ui"}, {}});
            if (storage.error.type != 0)
            {
                co_return cxx::unexpected(storage.error);
            }
            rdesc::TextureInfo texture{};
            texture.width = atlas.width;
            texture.height = atlas.height;
            texture.channel = 4;
            texture.pixel_format = rdesc::ETexturePixelFormat::RGBA8_UNORM;
            texture.color_space = rdesc::ETextureColorSpace::DATA;
            texture.mip_ranges[0] = {
                0,
                atlas.pixels.size(),
                static_cast<std::uint32_t>(atlas.width),
                static_cast<std::uint32_t>(atlas.height)
            };
            font_description = {static_cast<std::uint32_t>(atlas.width), static_cast<std::uint32_t>(atlas.height)};
            auto upload = requestRenderCommand<CreateTexture>(
                rendering.assetClient(),
                {storage.storage, texture, std::as_bytes(std::span(atlas.pixels))}
            );
            const auto uploaded = co_await stdexec::continues_on(
                stdexec::starts_on(rendering.assetScheduler(), std::move(upload)),
                rendering.execution().mainScheduler()
            );
            if (uploaded.error.type != 0)
            {
                co_return cxx::unexpected(uploaded.error);
            }
            font = uploaded.texture;
            std::vector<std::uint8_t>{}.swap(atlas.pixels); // Real Response ended the source borrow.
            const auto created_scene = co_await requestRenderCommand<CreateScene>(client, {});
            if (created_scene.error.type != 0)
            {
                co_return cxx::unexpected(created_scene.error);
            }
            scene = created_scene.scene;
            const auto created_view = co_await requestRenderCommand<CreateView>(client, {scene});
            if (created_view.error.type != 0)
            {
                co_return cxx::unexpected(created_view.error);
            }
            view = created_view.view;
            co_return desired_active && !closing ? co_await createOutput() : RenderResult<void>{};
        }

        Transaction createOutput() noexcept
        {
            auto& client = rendering.mainClient();
            const auto created_target =
                co_await requestRenderCommand<CreateTarget>(client, {desired_description, surface});
            if (created_target.error.type != 0)
            {
                co_return cxx::unexpected(created_target.error);
            }
            target = created_target.target;
            target_epoch = created_target.epoch;
            output_available = created_target.output_available;
            description = created_target.description;
            desired_description.format = description.format;
            co_return co_await rebuildImages();
        }

        Transaction rebuildImages() noexcept
        {
            const auto revision = desired_image_revision;
            const auto images = desired_images;
            configuring_images = images;
            cxx::scope_exit end_configuration([this]() noexcept { configuring_images = {}; });
            if (!effect_to_release.isNull())
            {
                const auto removed =
                    co_await requestRenderCommand<RemoveEffect>(rendering.mainClient(), {effect_to_release});
                if (removed.error.type != 0)
                {
                    co_return cxx::unexpected(removed.error);
                }
                effect_to_release = {};
            }
            UiRasterFeatureConfig ui{description};
            ui.images.push_back(UiTextureImage{font, font_description, "ui"});
            CreateEffectRequest program{description};
            AttachmentRequest binding{scene, view, target};
            std::vector<UiImageSelection> selections{{{0}, 0}};
            for (std::uint32_t i = 0; i < images.size(); ++i)
            {
                const auto& image = images[i];
                GraphImageDesc source{1, 1, 1, 1, image.description.format};
                source.extent.source = EImageExtentSource::IMPORTED;
                ui.images.push_back(render::UiTargetImage{source});
                const auto resource = "ui.target." + std::to_string(i);
                program.definition.inputs.push_back(
                    {{0, uiImagePort(i + 1)},
                     {resource, source, EGraphOrigin::IMPORTED, {GraphImageRange{}}, EGraphUse::SAMPLED}}
                );
                binding.target_images.push_back({resource, image.target});
                selections.push_back({image.token, i + 1});
            }
            auto configuration = ConfigValue::make(std::move(ui));
            if (!configuration)
            {
                co_return cxx::unexpected(configuration.error());
            }
            program.definition.features.push_back({feature, "ui", std::move(*configuration)});
            program.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{description.format, 1}});
            auto& client = rendering.mainClient();
            const auto compiled = co_await requestRenderCommand<CreateEffect>(client, std::move(program));
            if (compiled.error.type != 0)
            {
                co_return cxx::unexpected(compiled.error);
            }
            effect_to_release = compiled.effect;
            binding.effect = compiled.effect;
            binding.replaces = attachment;
            const auto attached = co_await requestRenderCommand<Attach>(client, std::move(binding));
            if (attached.error.type != 0)
            {
                const auto removed = co_await requestRenderCommand<RemoveEffect>(client, {effect_to_release});
                if (removed.error.type == 0)
                {
                    effect_to_release = {};
                }
                else
                {
                    reportRenderFailure(removed.error, "Discard UI Effect");
                }
                co_return cxx::unexpected(attached.error);
            }
            if (attached.draw_lists.size() != 1)
            {
                std::terminate(); // Built-in UiRasterFeature has exactly one list.
            }
            std::swap(effect, effect_to_release);
            attachment = attached.attachment;
            draw_pass = attached.draw_lists[0].second;
            image_selections = std::move(selections);
            applied_image_revision = revision;
            if (!effect_to_release.isNull())
            {
                const auto removed = co_await requestRenderCommand<RemoveEffect>(client, {effect_to_release});
                if (removed.error.type != 0)
                {
                    co_return cxx::unexpected(removed.error);
                }
                effect_to_release = {};
            }
            co_return RenderResult<void>{};
        }

        Transaction reconfigure() noexcept
        {
            const auto desired = desired_description;
            const auto active = desired_active;
            auto& client = rendering.mainClient();
            if (target.isNull())
            {
                co_return active ? co_await createOutput() : RenderResult<void>{};
            }
            if (active && (desired != description || !output_available))
            {
                const auto resized = co_await requestRenderCommand<ResizeTarget>(client, {target, desired});
                output_available = resized.output_available;
                if (resized.error.type != 0)
                {
                    co_return cxx::unexpected(resized.error);
                }
                description = resized.description;
                target_epoch = resized.epoch;
                applied_active = false;
            }
            if (!active && applied_active)
            {
                const auto changed = co_await requestRenderCommand<SetTargetActive>(client, {target, active});
                if (changed.error.type != 0)
                {
                    co_return cxx::unexpected(changed.error);
                }
                applied_active = active;
            }
            if (desired_image_revision != applied_image_revision)
            {
                co_return co_await rebuildImages();
            }
            co_return RenderResult<void>{};
        }

        Transaction close() noexcept
        {
            auto& client = rendering.mainClient();
            if (!scene.isNull())
            {
                const auto result = co_await requestRenderCommand<RemoveScene>(client, {scene});
                if (result.error.type != 0)
                {
                    co_return cxx::unexpected(result.error);
                }
                scene = {};
                view = {};
                attachment = {};
                image_selections.clear();
                completeImageRemovals(desired_image_revision, {});
            }
            for (auto* handle : {&effect_to_release, &effect})
            {
                if (handle->isNull())
                {
                    continue;
                }
                const auto result = co_await requestRenderCommand<RemoveEffect>(client, {*handle});
                if (result.error.type != 0)
                {
                    co_return cxx::unexpected(result.error);
                }
                *handle = {};
            }
            if (!target.isNull())
            {
                const auto result = co_await requestRenderCommand<RemoveTarget>(client, target);
                if (result.error.type != 0)
                {
                    co_return cxx::unexpected(result.error);
                }
                target = {};
            }
            if (!font.isNull())
            {
                const auto result = co_await requestRenderCommand<ReleaseTexture>(client, font);
                if (result.error.type != 0)
                {
                    co_return cxx::unexpected(result.error);
                }
                font = {};
            }
            co_return RenderResult<void>{};
        }

        RenderExecution& rendering;
        EditorWindow& window;
        TargetDesc description, desired_description;
        SurfaceDescription surface;
        FeatureId feature;
        SceneId scene;
        ViewId view;
        TargetId target;
        std::uint64_t target_epoch{};
        EffectId effect, effect_to_release;
        AttachmentId attachment;
        GraphPassId draw_pass;
        TextureId font;
        GraphImageDesc font_description;
        std::optional<BoundCommand<UpdateDrawList>> update_draws;
        ui::DrawData draw_data;
        std::vector<UiTargetImage> desired_images;
        std::span<const UiTargetImage> configuring_images;
        ImageRemoval* image_removals = nullptr;
        std::vector<UiImageSelection> image_selections;
        std::uint64_t next_image_token = 1;
        std::uint64_t desired_image_revision = 1, applied_image_revision = 0;
        bool desired_active = true, applied_active = true;
        bool transaction_pending = false, closing = false;
        bool output_available = false;
        std::uint64_t requested_revision = 1, failed_revision = 0;
        std::uint64_t resynchronization{};
        RenderError failure;
    };

    EditorUiScene::EditorUiScene(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    FrameworkResult<std::unique_ptr<EditorUiScene>> EditorUiScene::create(
        RenderExecution& rendering,
        EditorWindow& window,
        TargetDesc description,
        SurfaceDescription surface,
        ui::FontAtlas atlas
    ) noexcept
    {
        auto impl = std::make_unique<Impl>(rendering, window, description, surface);
        auto command = rendering.mainClient().bind<UpdateDrawList>();
        if (!command)
        {
            return cxx::unexpected(command.error());
        }
        impl->update_draws = *command;
        if (auto started = impl->startTransaction(impl->initialize(std::move(atlas))); !started)
        {
            return cxx::unexpected(started.error());
        }
        auto result = std::unique_ptr<EditorUiScene>{new EditorUiScene(std::move(impl))};
        if (description.kind == ETargetKind::SURFACE)
        {
            window.setMetricsReceiver(
                result.get(),
                [](void* value, const WindowMetrics& metrics) noexcept
                {
                    static_cast<EditorUiScene*>(value)
                        ->setExtent(metrics.framebuffer_width, metrics.framebuffer_height, !metrics.minimized);
                }
            );
        }
        return result;
    }

    EditorUiScene::~EditorUiScene() noexcept
    {
        if (impl_->description.kind == ETargetKind::SURFACE)
        {
            impl_->window.setMetricsReceiver(nullptr, nullptr);
        }
        if (impl_->image_removals || impl_->transaction_pending || !impl_->scene.isNull() || !impl_->effect.isNull() ||
            !impl_->effect_to_release.isNull() || !impl_->target.isNull() || !impl_->font.isNull())
        {
            std::terminate();
        }
    }

    void EditorUiScene::setExtent(std::uint32_t width, std::uint32_t height, bool active) noexcept
    {
        const bool desired_active = active && width != 0 && height != 0;
        const bool changed =
            impl_->desired_active != desired_active ||
            (width && height &&
             (impl_->desired_description.width != width || impl_->desired_description.height != height));
        impl_->desired_active = desired_active;
        if (width && height)
        {
            impl_->desired_description.width = width;
            impl_->desired_description.height = height;
        }
        if (changed || impl_->failure.type != 0)
        {
            ++impl_->requested_revision;
            impl_->requestConfiguration();
        }
    }

    FrameworkResult<rdesc::UiImageToken> EditorUiScene::addTargetImage(TargetId target, TargetDesc description) noexcept
    {
        const bool invalid = target.isNull() || !description.sampled || description.kind != ETargetKind::OFFSCREEN ||
                             description.samples != 1;
        if (invalid)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        if (impl_->next_image_token == UINT64_MAX)
        {
            return cxx::unexpected(RenderError{kRuntimeCapacity});
        }
        const rdesc::UiImageToken token{impl_->next_image_token++};
        impl_->desired_images.push_back({token, target, description});
        ++impl_->desired_image_revision;
        ++impl_->requested_revision;
        impl_->requestConfiguration();
        return token;
    }

    void EditorUiScene::deliverImageRemoval(ImageRemoval& removal, FrameworkResult<void> result) noexcept
    {
        auto code_lifetime = std::move(removal.code_lifetime);
        removal.complete(removal, std::move(result));
    }

    void EditorUiScene::beginImageRemoval(ImageRemoval& removal) noexcept
    {
        auto& state = *impl_;
        if (removal.token.value == 0)
        {
            deliverImageRemoval(removal, cxx::unexpected(RenderError{kCommandContract}));
            return;
        }
        if (std::erase_if(state.desired_images, [&](const auto& image) { return image.token == removal.token; }))
        {
            ++state.desired_image_revision;
            ++state.requested_revision;
        }
        if (!state.referencesImage(removal.token))
        {
            state.requestConfiguration();
            deliverImageRemoval(removal, {});
            return;
        }
        removal.revision = state.requested_revision;
        removal.next = std::exchange(state.image_removals, &removal);
        state.requestConfiguration();
    }

    bool EditorUiScene::imageReady(rdesc::UiImageToken token) const noexcept
    {
        return token.value != 0 &&
               std::ranges::any_of(impl_->image_selections, [&](const auto& image) { return image.token == token; });
    }

    FrameworkResult<bool> EditorUiScene::outputReady() const noexcept
    {
        const auto& state = *impl_;
        if (state.failure.type != 0 && state.target.isNull())
        {
            return cxx::unexpected(state.failure);
        }
        return state.output_available && !state.closing && !state.transaction_pending && state.desired_active &&
               !state.attachment.isNull();
    }

    RenderError EditorUiScene::configurationError() const noexcept
    {
        return impl_->failure;
    }

    FrameworkResult<void> EditorUiScene::appendFrame(FrameInput& frame) noexcept
    {
        auto& state = *impl_;
        state.draw_data.setFrameMemory(frame.memory());
        if (auto updated = state.window.updateUi(state.draw_data); !updated)
        {
            return updated;
        }
        auto input = makeUiDrawInput(frame, state.draw_data.data(), state.image_selections);
        if (!input)
        {
            return cxx::unexpected(input.error());
        }
        auto appended = state.rendering.mainClient()
                            .append(frame, *state.update_draws, {state.attachment, state.draw_pass, *input});
        if (!appended)
        {
            return appended;
        }
        const auto resynchronization = state.rendering.mainClient().resynchronization();
        if (!state.applied_active || state.resynchronization != resynchronization)
        {
            auto active = state.rendering.mainClient().bind<SetTargetActive>();
            if (!active)
            {
                return cxx::unexpected(active.error());
            }
            if (auto resumed =
                    state.rendering.mainClient()
                        .append(frame, *active, SetTargetActive::Request{state.target, true, state.target_epoch});
                !resumed)
            {
                return resumed;
            }
            state.applied_active = true;
            state.resynchronization = resynchronization;
        }
        frame.group_ends.push_back(frame.commands.size());
        return {};
    }

    void EditorUiScene::stopFrames() noexcept
    {
        if (!impl_->closing)
        {
            impl_->closing = true;
            impl_->failure = {}; // A previous configuration failure does not discharge cleanup.
            ++impl_->requested_revision;
            impl_->requestConfiguration();
        }
    }

    bool EditorUiScene::settled() const noexcept
    {
        const auto& state = *impl_;
        return state.closing && !state.transaction_pending && state.scene.isNull() && state.target.isNull() &&
               state.effect.isNull() && state.effect_to_release.isNull() && state.font.isNull();
    }
} // namespace lux::editor
