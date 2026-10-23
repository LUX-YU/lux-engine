#include <algorithm>
#include <exec/task.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
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

        Impl(RenderExecution& input, TargetDesc description, SurfaceDescription surface)
            : rendering(input), description(description), desired_description(description), surface(surface)
        {
        }

        RenderResult<void> startTransaction(Transaction work) noexcept
        {
            transaction_pending = true;
            const bool cleanup = closing;
            auto operation = stdexec::starts_on(rendering.execution().mainScheduler(), std::move(work));
            auto result = execution::taskResult(std::move(operation));
            auto accepted = rendering.execution().spawn(stdexec::then(
                std::move(result),
                [this, cleanup](auto result) noexcept
                {
                    failure = result                           ? RenderError{}
                              : result.error().domainFailure() ? *result.error().domainFailure()
                                                               : RenderError{kRuntimeState};
                    if (closing && !cleanup && failure.type != 0)
                    {
                        reportRenderFailure(failure, "Configure closing UI output");
                        failure = {}; // Still run cleanup of the handles created before this failure.
                    }
                    transaction_pending = false;
                }
            ));
            if (!accepted)
            {
                transaction_pending = false;
                return cxx::unexpected(execution::toError(accepted.error()));
            }
            return {};
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
                if (!desired_active && created_target.error.type == kDeviceArgument)
                {
                    co_return RenderResult<void>{}; // Minimize raced the submitted creation; retry on restore.
                }
                co_return cxx::unexpected(created_target.error);
            }
            target = created_target.target;
            description = created_target.description;
            desired_description.format = description.format;
            co_return co_await rebuildImages();
        }

        Transaction rebuildImages() noexcept
        {
            const auto revision = desired_image_revision;
            const auto images = desired_images;
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
            if (active && (desired != description || resize_required))
            {
                const auto resized = co_await requestRenderCommand<ResizeTarget>(client, {target, desired});
                if (resized.error.type != 0)
                {
                    if (!desired_active && resized.error.type == kDeviceArgument)
                    {
                        resize_required = true;
                        co_return RenderResult<void>{};
                    }
                    co_return cxx::unexpected(resized.error);
                }
                description = resized.description;
                resize_required = false;
            }
            if (active != applied_active)
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
        TargetDesc description, desired_description;
        SurfaceDescription surface;
        FeatureId feature;
        SceneId scene;
        ViewId view;
        TargetId target;
        EffectId effect, effect_to_release;
        AttachmentId attachment;
        GraphPassId draw_pass;
        TextureId font;
        GraphImageDesc font_description;
        std::optional<BoundCommand<UpdateDrawList>> update_draws;
        ui::DrawData draw_data;
        std::vector<UiTargetImage> desired_images;
        std::vector<UiImageSelection> image_selections;
        std::uint64_t next_image_token = 1;
        std::uint64_t desired_image_revision = 1, applied_image_revision = 0;
        bool desired_active = true, applied_active = true;
        bool transaction_pending = false, closing = false;
        bool resize_required = false;
        RenderError failure;
    };

    EditorUiScene::EditorUiScene(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    FrameworkResult<std::unique_ptr<EditorUiScene>> EditorUiScene::create(
        RenderExecution& rendering,
        TargetDesc description,
        SurfaceDescription surface,
        ui::FontAtlas atlas
    ) noexcept
    {
        auto impl = std::make_unique<Impl>(rendering, description, surface);
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
        return std::unique_ptr<EditorUiScene>{new EditorUiScene(std::move(impl))};
    }

    EditorUiScene::~EditorUiScene() noexcept
    {
        if (impl_->transaction_pending || !impl_->scene.isNull() || !impl_->effect.isNull() ||
            !impl_->effect_to_release.isNull() || !impl_->target.isNull() || !impl_->font.isNull())
        {
            std::terminate();
        }
    }

    void EditorUiScene::setExtent(std::uint32_t width, std::uint32_t height, bool active) noexcept
    {
        impl_->desired_active = active && width != 0 && height != 0;
        if (width && height)
        {
            impl_->desired_description.width = width;
            impl_->desired_description.height = height;
        }
    }

    FrameworkResult<void> EditorUiScene::setTargetImages(std::vector<UiTargetImage> images) noexcept
    {
        std::ranges::sort(images, {}, &UiTargetImage::token);
        for (std::size_t i = 0; i < images.size(); ++i)
        {
            const auto& image = images[i];
            const bool duplicate = i != 0 && image.token == images[i - 1].token;
            const bool invalid = image.token.value == 0 || image.target.isNull() || !image.description.sampled ||
                                 image.description.kind != ETargetKind::OFFSCREEN || image.description.samples != 1;
            if (duplicate || invalid)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
        }
        if (images != impl_->desired_images)
        {
            for (const auto& image : images)
            {
                const auto next = image.token.value == UINT64_MAX ? UINT64_MAX : image.token.value + 1;
                impl_->next_image_token = std::max(impl_->next_image_token, next);
            }
            impl_->desired_images = std::move(images);
            ++impl_->desired_image_revision;
        }
        return {};
    }

    FrameworkResult<rdesc::UiImageToken> EditorUiScene::addTargetImage(TargetId target, TargetDesc description) noexcept
    {
        auto images = impl_->desired_images;
        if (impl_->next_image_token == UINT64_MAX)
        {
            return cxx::unexpected(RenderError{kRuntimeCapacity});
        }
        const rdesc::UiImageToken token{impl_->next_image_token++};
        images.push_back({token, target, description});
        if (auto applied = setTargetImages(std::move(images)); !applied)
        {
            return cxx::unexpected(applied.error());
        }
        return token;
    }

    void EditorUiScene::removeTargetImage(rdesc::UiImageToken token) noexcept
    {
        if (std::erase_if(impl_->desired_images, [&](const auto& image) noexcept { return image.token == token; }))
        {
            ++impl_->desired_image_revision;
        }
    }

    bool EditorUiScene::imagesApplied() const noexcept
    {
        return (impl_->closing && impl_->scene.isNull() && !impl_->transaction_pending) ||
               impl_->applied_image_revision == impl_->desired_image_revision;
    }

    FrameworkResult<void> EditorUiScene::maintain() noexcept
    {
        auto& state = *impl_;
        if (state.transaction_pending)
        {
            return {};
        }
        if (state.closing)
        {
            if (state.failure.type != 0 && state.failure.type != kRuntimeCapacity)
            {
                return cxx::unexpected(state.failure);
            }
            if (!state.scene.isNull() || !state.target.isNull() || !state.effect.isNull() ||
                !state.effect_to_release.isNull() || !state.font.isNull())
            {
                return state.startTransaction(state.close());
            }
            return {};
        }
        if (state.failure.type != 0)
        {
            return cxx::unexpected(state.failure);
        }
        if (state.target.isNull() && !state.desired_active)
        {
            return {}; // No swapchain exists yet; a window restore is the next real producer.
        }
        const bool resize = state.desired_active && (state.target.isNull() || state.resize_required ||
                                                     state.description != state.desired_description);
        const bool changed = resize || state.desired_active != state.applied_active || !imagesApplied();
        return changed ? state.startTransaction(state.reconfigure()) : FrameworkResult<void>{};
    }

    FrameworkResult<bool> EditorUiScene::outputReady() const noexcept
    {
        const auto& state = *impl_;
        if (state.failure.type != 0)
        {
            return cxx::unexpected(state.failure);
        }
        return !state.closing && !state.transaction_pending && state.desired_active && !state.resize_required &&
               !state.attachment.isNull();
    }

    ui::DrawData& EditorUiScene::captureBuffer(FrameInput& frame) noexcept
    {
        impl_->draw_data.setFrameMemory(frame.memory());
        return impl_->draw_data;
    }

    FrameworkResult<void> EditorUiScene::appendFrameCommands(FrameInput& frame) noexcept
    {
        auto& state = *impl_;
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
        frame.group_ends.push_back(frame.commands.size());
        return {};
    }

    void EditorUiScene::stopFrames() noexcept
    {
        if (!impl_->closing)
        {
            impl_->closing = true;
            impl_->failure = {}; // A previous configuration failure does not discharge cleanup.
        }
    }

    bool EditorUiScene::settled() const noexcept
    {
        const auto& state = *impl_;
        return state.closing && !state.transaction_pending && state.scene.isNull() && state.target.isNull() &&
               state.effect.isNull() && state.effect_to_release.isNull() && state.font.isNull();
    }
} // namespace lux::editor
