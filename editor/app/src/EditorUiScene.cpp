#include <algorithm>
#include <exec/task.hpp>
#include <exec/variant_sender.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/editor/detail/EditorUiScene.hpp>
#include <lux/engine/render/features/UiRasterFeature.hpp>
#include <lux/engine/rendering/RenderDiagnostics.hpp>
#include <lux/engine/rendering/RenderExecution.hpp>
#include <lux/engine/rendering/UiDrawInput.hpp>

namespace lux::editor
{
    using namespace render;
    using namespace rendering;

    namespace
    {
        // A value-owned request in the caller's existing Sender chain. No task, queue or reply owner.
        template <RenderCommand C> auto request(RenderRequester& client, typename C::Request input) noexcept
        {
            return stdexec::let_value(
                stdexec::just(std::move(input), client.bind<C>()),
                [&client](typename C::Request& input, auto& binding) noexcept
                {
                    using Immediate = decltype(stdexec::just(typename C::Response{}));
                    using Request = decltype(client.request(*binding, input));
                    using VRequest = exec::variant_sender<Immediate, Request>;
                    return binding ? VRequest{client.request(*binding, input)}
                                   : VRequest{stdexec::just(typename C::Response{binding.error()})};
                }
            );
        }
    } // namespace

    struct EditorUiScene::Impl final
    {
        using Transaction = exec::task<RenderResult<void>>;

        Impl(RenderExecution& input, TargetDesc description, SurfaceDescription surface)
            : rendering(input), description(description), desired_description(description), surface(surface)
        {
        }

        RenderResult<void> launch(Transaction work) noexcept
        {
            transaction_pending = true;
            auto accepted = rendering.execution().submit(
                {.name = "Configure UI output", .category = "editor.ui"},
                [this, work = std::move(work)](execution::TaskReporter) mutable noexcept
                {
                    auto operation = stdexec::starts_on(rendering.execution().mainScheduler(), std::move(work));
                    // Revocation stops new UI frames, not cleanup of already accepted native objects.
                    auto result = execution::taskResult(stdexec::write_env(
                        std::move(operation),
                        stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
                    ));
                    return stdexec::then(
                        std::move(result),
                        [this](auto result) noexcept
                        {
                            if (!result)
                            {
                                if (const auto* domain = result.error().domainFailure())
                                {
                                    failure = *domain;
                                }
                                else
                                {
                                    failure =
                                        RenderError{result.error().isCancelled() ? kCommandCancelled : kRuntimeState};
                                }
                            }
                            transaction_pending = false;
                            return execution::taskState(result);
                        }
                    );
                }
            );
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
            const auto features = co_await request<ListFeatures>(client, {});
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
            const auto storages = co_await request<ListStorages>(client, {});
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
                co_await request<PrepareStorage>(client, {{GlobalStorageLocation{}, type->id, "ui"}, {}});
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
            auto upload = request<CreateTexture>(
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
            const auto created_scene = co_await request<CreateScene>(client, {});
            if (created_scene.error.type != 0)
            {
                co_return cxx::unexpected(created_scene.error);
            }
            scene = created_scene.scene;
            const auto created_view = co_await request<CreateView>(client, {scene});
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
            const auto created_target = co_await request<CreateTarget>(client, {desired_description, surface});
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
            const auto compiled = co_await request<CreateEffect>(client, std::move(program));
            if (compiled.error.type != 0)
            {
                co_return cxx::unexpected(compiled.error);
            }
            binding.effect = compiled.effect;
            binding.replaces = attachment;
            const auto attached = co_await request<Attach>(client, std::move(binding));
            if (attached.error.type != 0)
            {
                static_cast<void>(co_await request<RemoveEffect>(client, {compiled.effect}));
                co_return cxx::unexpected(attached.error);
            }
            if (attached.draw_lists.size() != 1)
            {
                std::terminate(); // Built-in UiRasterFeature has exactly one list.
            }
            const auto previous = std::exchange(effect, compiled.effect);
            attachment = attached.attachment;
            draw_pass = attached.draw_lists[0].second;
            image_selections = std::move(selections);
            applied_image_revision = revision;
            if (!previous.isNull())
            {
                const auto removed = co_await request<RemoveEffect>(client, {previous});
                if (removed.error.type != 0)
                {
                    co_return cxx::unexpected(removed.error);
                }
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
                const auto resized = co_await request<ResizeTarget>(client, {target, desired});
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
                const auto changed = co_await request<SetTargetActive>(client, {target, active});
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
            RenderError first;
            const auto remember = [&](RenderError error) noexcept
            {
                if (first.type == 0)
                {
                    first = error;
                }
            };
            if (!scene.isNull())
            {
                remember((co_await request<RemoveScene>(client, {std::exchange(scene, {})})).error);
                attachment = {};
            }
            if (!effect.isNull())
            {
                remember((co_await request<RemoveEffect>(client, {std::exchange(effect, {})})).error);
            }
            if (!target.isNull())
            {
                remember((co_await request<RemoveTarget>(client, std::exchange(target, {}))).error);
            }
            if (!font.isNull())
            {
                remember((co_await request<ReleaseTexture>(client, std::exchange(font, {}))).error);
            }
            co_return first.type == 0 ? RenderResult<void>{} : RenderResult<void>{cxx::unexpected(first)};
        }

        RenderExecution& rendering;
        TargetDesc description, desired_description;
        SurfaceDescription surface;
        FeatureId feature;
        SceneId scene;
        ViewId view;
        TargetId target;
        EffectId effect;
        AttachmentId attachment;
        GraphPassId draw_pass;
        TextureId font;
        GraphImageDesc font_description;
        std::optional<BoundCommand<UpdateDrawList>> update_draws;
        ui::DrawData draw_data;
        std::vector<UiTargetImage> desired_images;
        std::vector<UiImageSelection> image_selections;
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
        if (auto started = impl->launch(impl->initialize(std::move(atlas))); !started)
        {
            return cxx::unexpected(started.error());
        }
        return std::unique_ptr<EditorUiScene>{new EditorUiScene(std::move(impl))};
    }

    EditorUiScene::~EditorUiScene() noexcept
    {
        if (impl_->transaction_pending)
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
            impl_->desired_images = std::move(images);
            ++impl_->desired_image_revision;
        }
        return {};
    }

    bool EditorUiScene::imagesApplied() const noexcept
    {
        return impl_->applied_image_revision == impl_->desired_image_revision;
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
            if (!state.scene.isNull() || !state.target.isNull() || !state.effect.isNull() || !state.font.isNull())
            {
                return state.launch(state.close());
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
        return changed ? state.launch(state.reconfigure()) : FrameworkResult<void>{};
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
        impl_->closing = true;
    }

    bool EditorUiScene::settled() const noexcept
    {
        const auto& state = *impl_;
        return state.closing && !state.transaction_pending && state.scene.isNull() && state.target.isNull() &&
               state.font.isNull();
    }
} // namespace lux::editor
