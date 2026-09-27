#include <lux/engine/ui/rendering/detail/FontConfiguration.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>
#include <lux/engine/render/comm/server/FeatureRegistration.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/targets/RenderTargetBinding.hpp>
#include <lux/engine/render/renderer/FrameContext.hpp>
#include <lux/engine/ui/rendering/detail/RenderFeature.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace lux::ui::detail
{
    namespace
    {
        bool supportsImageFormat(VkFormat format) noexcept
        {
            // ImageElement uses the ordinary floating-point sampler2D shader. Integer,
            // depth/stencil and unknown formats require another explicit presentation.
            switch (format)
            {
            case VK_FORMAT_R8_UNORM:
            case VK_FORMAT_R8G8_UNORM:
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
            case VK_FORMAT_R16_UNORM:
            case VK_FORMAT_R16G16B16A16_SFLOAT:
            case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
            case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
            case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
            case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
            case VK_FORMAT_BC3_SRGB_BLOCK:
            case VK_FORMAT_BC5_UNORM_BLOCK:
            case VK_FORMAT_BC7_SRGB_BLOCK:
            case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
            case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
            case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
            case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
            case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
            case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
            case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
            case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
                return true;
            default:
                return false;
            }
        }
    }

    RenderFeature::RenderFeature(FontAtlas font) : font_(std::move(font)) {}

    RenderFeature::~RenderFeature()
    {
        for (auto& slot : textures_)
        {
            for (auto& texture : slot)
            {
                renderer_->removeTexture(texture.descriptor);
            }
        }
        for (const auto& [format, pipeline] : pipelines_)
        {
            renderer_->destroyColorPipeline(pipeline);
        }
        if (sampler_)
        {
            vkDestroySampler(renderContext().device(), sampler_, nullptr);
        }
    }

    render::Expected<void> RenderFeature::initAndAttachTo(render::RenderScene&)
    {
        auto& context = renderContext();
        auto& resources = context.resourceContext();
        auto created = VulkanRenderer::create(
            {resources.instanceContext().instance(),
             resources.physicalDevice(),
             resources.logicalDevice(),
             resources.graphicsQueueFamilyIndex(),
             resources.graphicsQueue(),
             VK_FORMAT_R8G8B8A8_UNORM,
             context.framesInFlight(),
             nullptr},
            font_
        );
        if (!created)
        {
            return render::renderFailure<render::err::feature::ResourceInitFailed>();
        }
        renderer_ = std::move(*created);
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        const auto result = vkCreateSampler(context.device(), &info, nullptr, &sampler_);
        if (result != VK_SUCCESS)
        {
            return render::renderFailure<render::err::device::VulkanCallFailed>(result);
        }
        textures_.resize(context.framesInFlight());
        renderer_->setTextureResolver(&RenderFeature::resolveTexture, this);
        font_ = {};
        return {};
    }

    void RenderFeature::clear() noexcept
    {
        frame_.reset();
        submission_ = {};
        reads_.clear();
        // Descriptors remain in their FIF slots until their fences allow reuse.
    }

    render::Expected<void> RenderFeature::adopt(const FrameInput& input)
    {
        auto& resources = renderContext().globalRegistry().must<render::TextureResources>();
        std::vector<render::SampledTarget> reads;
        for (const auto handle : input.frame->draw_data.textures())
        {
            const auto* texture = resources.resolve(handle);
            if (!texture)
                return render::renderFailure<render::err::resource::NotFound>();
            if (texture->kind == render::TextureResources::ERemoteKind::CUBE)
                return render::renderFailure<render::err::resource::TypeMismatch>();
            if (texture->kind == render::TextureResources::ERemoteKind::OUTPUT)
            {
                // The generational remote texture identifies its output. Resolve the
                // real producer/dependency on Render; no Main-side View record is borrowed.
                reads.push_back({texture->target, 0});
            }
            else
            {
                const auto local = resources.resolveTexture(handle);
                if (!local.isValid())
                    return render::renderFailure<render::err::resource::NotFound>();
                if (!supportsImageFormat(resources.bindlessSet2D().slotFormat(local.index)))
                    return render::renderFailure<render::err::resource::TypeMismatch>();
            }
        }
        frame_ = input.frame;
        submission_ = input.submission;
        reads_ = std::move(reads);
        return {};
    }

    void RenderFeature::retainSubmissions(const render::FrameRuntime& frame) const noexcept
    {
        if (!frame_)
            return;
        frame.retainSubmission(submission_);
        for (const auto& resource : frame_->resources)
            frame.retainSubmission(resource);
    }

    std::span<const render::SampledTarget> RenderFeature::sampledTargets() const noexcept
    {
        return reads_;
    }

    render::Expected<void> RenderFeature::bindSampledTargets(
        std::span<const render::SampledTargetImage> images,
        std::uint32_t frame_slot,
        std::uint64_t serial
    )
    {
        if (images.size() != reads_.size() || frame_slot >= textures_.size())
        {
            return render::renderFailure<render::err::comm::RequestInvalid>();
        }
        frame_slot_ = frame_slot;
        serial_ = serial;
        auto& slot = textures_[frame_slot];
        auto& resources = renderContext().globalRegistry().must<render::TextureResources>();
        const auto handles = frame_ ? frame_->draw_data.textures() : std::span<const render::RTextureHandle>{};
        std::size_t output_index{};
        // The slot fence is complete. Re-resolve each distinct texture once here,
        // including stable-ID mip backing replacements, never once per draw command.
        for (std::size_t i{}; i < handles.size(); ++i)
        {
            const auto* source = resources.resolve(handles[i]);
            if (!source)
                return render::renderFailure<render::err::resource::NotFound>();
            VkImageView view{};
            VkSampler sampler = sampler_;
            VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (source->kind == render::TextureResources::ERemoteKind::OUTPUT)
            {
                if (output_index >= images.size() || images[output_index].target != source->target)
                    return render::renderFailure<render::err::comm::RequestInvalid>();
                const auto& image = images[output_index];
                view = image.view;
                layout = image.layout;
                reads_[output_index++].backing_revision = image.backing_revision;
            }
            else
            {
                const auto local = resources.resolveTexture(handles[i]);
                if (!local.isValid())
                    return render::renderFailure<render::err::resource::NotFound>();
                view = resources.imageView(local);
                sampler = resources.sampler(local);
            }
            if (i == slot.size())
                slot.emplace_back();
            auto& texture = slot[i];
            if (texture.token != handles[i] || texture.image != view || texture.sampler != sampler)
            {
                renderer_->removeTexture(texture.descriptor);
                texture.image = view;
                texture.sampler = sampler;
                texture.descriptor = renderer_->addTexture(sampler, view, layout);
                if (!texture.descriptor)
                    return render::renderFailure<render::err::feature::ResourceInitFailed>();
            }
            texture.token = handles[i];
        }
        while (slot.size() > handles.size())
        {
            renderer_->removeTexture(slot.back().descriptor);
            slot.pop_back();
        }
        return {};
    }

    VkDescriptorSet RenderFeature::resolveTexture(void* user, render::RTextureHandle token) noexcept
    {
        const auto& self = *static_cast<RenderFeature*>(user);
        for (const auto& texture : self.textures_[self.frame_slot_])
        {
            if (texture.token == token)
            {
                return texture.descriptor;
            }
        }
        return VK_NULL_HANDLE;
    }

    void RenderFeature::addPasses(render::RGBuilder& builder)
    {
        using namespace render;
        const auto color = builder.findTexture("SceneColor");
        const auto* resource = builder.getResourceDescription(color);
        if (!resource)
        {
            renderContext().reportError(renderError<err::feature::ResourceInitFailed>());
            return;
        }
        const auto format = toVkFormat(std::get<RGTextureDescription>(resource->desc).format);
        auto found = std::find_if(pipelines_.begin(), pipelines_.end(), [format](const auto& value) {
            return value.first == format;
        });
        if (found == pipelines_.end())
        {
            const auto pipeline = renderer_->createColorPipeline(format);
            if (!pipeline)
            {
                renderContext().reportError(renderError<err::feature::ResourceInitFailed>());
                return;
            }
            pipelines_.emplace_back(format, pipeline);
            found = std::prev(pipelines_.end());
        }
        const auto pipeline = found->second;
        builder.addPass("UiRender", ERGPassType::GRAPHICS)
            .write(color, ETextureRole::COLOR_ATTACHMENT)
            .stage(ERenderStage::OVERLAY_STAGE)
            .setKernelFn([this, pipeline](const PassRecordContext& context) {
                if (frame_)
                {
                    renderer_->renderFrame(frame_->draw_data, context.cmd, pipeline, frame_slot_, serial_);
                }
            });
    }
} // namespace lux::ui::detail

namespace lux::render
{
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    Expected<FeatureHandle> UiRenderCreateFn(void* scene, const void* data, std::size_t size)
    {
        if (!data || size < sizeof(UiRenderCommConfig))
        {
            return renderFailure<err::comm::PayloadSizeMismatch>(sizeof(UiRenderCommConfig), size);
        }
        UiRenderCommConfig config;
        std::memcpy(&config, data, sizeof(config));
        const auto pixels = static_cast<std::uint64_t>(config.width) * config.height;
        if (!ui::detail::validFontConfiguration(config.width, config.height, size - sizeof(config)))
        {
            return renderFailure<err::internal::InvalidArgument>();
        }
        ui::FontAtlas font;
        font.width = static_cast<int>(config.width);
        font.height = static_cast<int>(config.height);
        font.pixels.resize(static_cast<std::size_t>(pixels * 4));
        std::memcpy(font.pixels.data(), static_cast<const std::byte*>(data) + sizeof(config), font.pixels.size());
        return addFeature<ui::detail::RenderFeature>(scene, std::move(font));
    }

    void handleUiRenderFrame(GeneralRenderServer::Dispatcher::Ctx& context, const UiRenderFramePayload& payload)
    {
        auto* scene = lookupScene(context.user_state, payload.scene_id);
        if (!scene)
        {
            return;
        }
        auto* feature = scene->getFeature(payload.feature);
        if (!feature || feature->typeId() != kUiRenderDescriptor.type)
        {
            scene->renderContext().reportError(
                renderError<err::feature::HandleStale>(payload.feature.index, payload.feature.gen),
                payload.scene_id.index,
                scene->frameSerial()
            );
            return;
        }
        const auto attachment = CommandPacketView(context.program).attachment(payload.attachment_index);
        if (!attachment)
        {
            scene->renderContext().reportError(attachment.error(), payload.scene_id.index, scene->frameSerial());
            return;
        }
        const auto& record = attachment->get();
        using Input = ui::detail::FrameInput;
        if (record.type_id != ui::detail::kFrameAttachment || record.object_size != sizeof(Input) || !record.object)
        {
            scene->renderContext().reportError(
                renderError<err::comm::AttachmentTypeMismatch>(ui::detail::kFrameAttachment, record.type_id),
                payload.scene_id.index,
                scene->frameSerial()
            );
            return;
        }
        const auto& input = *static_cast<const Input*>(record.object);
        if (!input.frame || !input.frame->draw_data.valid() || !input.submission)
        {
            scene->renderContext()
                .reportError(renderError<err::comm::RequestInvalid>(), payload.scene_id.index, scene->frameSerial());
            return;
        }
        if (auto adopted = static_cast<ui::detail::RenderFeature*>(feature)->adopt(input); !adopted)
            scene->renderContext().reportError(adopted.error(), payload.scene_id.index, scene->frameSerial());
    }
    void handleUiRenderClear(GeneralRenderServer::Dispatcher::Ctx& context, const UiRenderClearPayload& payload)
    {
        auto* scene = lookupScene(context.user_state, payload.scene_id);
        if (!scene)
        {
            return;
        }
        auto* feature = scene->getFeature(payload.feature);
        if (!feature || feature->typeId() != kUiRenderDescriptor.type)
        {
            scene->renderContext().reportError(
                renderError<err::feature::HandleStale>(payload.feature.index, payload.feature.gen),
                payload.scene_id.index,
                scene->frameSerial()
            );
            return;
        }
        static_cast<ui::detail::RenderFeature*>(feature)->clear();
    }

} // namespace lux::render
