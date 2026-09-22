#include <lux/engine/render/comm/server/FeatureRegistration.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/detail/ViewImageLifetime.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/targets/RenderTargetBinding.hpp>
#include <lux/engine/ui/rendering/detail/UiRenderFeature.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace lux::ui::detail
{
UiRenderFeature::UiRenderFeature(UiFontAtlasSnapshot font) : font_(std::move(font))
{
}

UiRenderFeature::~UiRenderFeature()
{
    for (auto &slot : textures_)
    {
        for (auto &texture : slot)
        {
            renderer_->removeTexture(texture.descriptor);
        }
    }
    for (const auto &[format, pipeline] : pipelines_)
    {
        renderer_->destroyColorPipeline(pipeline);
    }
    if (sampler_)
    {
        vkDestroySampler(renderContext().device(), sampler_, nullptr);
    }
}

render::Expected<void> UiRenderFeature::initAndAttachTo(render::RenderScene &)
{
    auto &context = renderContext();
    auto &resources = context.resourceContext();
    auto created = UiVulkanRenderer::create({resources.instanceContext().instance(), resources.physicalDevice(),
                                             resources.logicalDevice(), resources.graphicsQueueFamilyIndex(),
                                             resources.graphicsQueue(), VK_FORMAT_R8G8B8A8_UNORM,
                                             context.framesInFlight(), nullptr},
                                            font_);
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
    renderer_->setTextureResolver(&UiRenderFeature::resolveTexture, this);
    font_ = {};
    return {};
}

void UiRenderFeature::clear() noexcept
{
    frame_.reset();
    reads_.clear();
    // Descriptors remain in their FIF slots until their fences allow reuse.
}

void UiRenderFeature::adopt(std::shared_ptr<const UiRenderFrame> frame)
{
    frame_ = std::move(frame);
    reads_.clear();
    for (const auto &image : frame_->images)
    {
        const auto &version = *render::detail::ViewImageAccess::record(image)->version;
        reads_.push_back({version.target, version.backing_revision.load(std::memory_order_acquire)});
    }
}

std::span<const render::SampledTarget> UiRenderFeature::sampledTargets() const noexcept
{
    return reads_;
}

render::Expected<void> UiRenderFeature::bindSampledTargets(std::span<const render::SampledTargetImage> images,
                                                           std::uint32_t frame_slot, std::uint64_t serial)
{
    if (images.size() != reads_.size() || frame_slot >= textures_.size())
    {
        return render::renderFailure<render::err::comm::RequestInvalid>();
    }
    frame_slot_ = frame_slot;
    serial_ = serial;
    auto &slot = textures_[frame_slot];
    // This FIF slot's fence has completed. Other slots retain their descriptors.
    for (std::size_t i = 0; i < images.size(); ++i)
    {
        if (i == slot.size())
        {
            slot.emplace_back();
        }
        auto &texture = slot[i];
        if (texture.image != images[i].view)
        {
            renderer_->removeTexture(texture.descriptor);
            texture.image = images[i].view;
            texture.descriptor = renderer_->addTexture(sampler_, texture.image, images[i].layout);
            if (!texture.descriptor)
            {
                return render::renderFailure<render::err::feature::ResourceInitFailed>();
            }
        }
        texture.token = frame_->images[i].texture;
        auto &version = *render::detail::ViewImageAccess::record(frame_->images[i])->version;
        version.backing_revision.store(images[i].backing_revision, std::memory_order_release);
    }
    while (slot.size() > images.size())
    {
        renderer_->removeTexture(slot.back().descriptor);
        slot.pop_back();
    }
    return {};
}

VkDescriptorSet UiRenderFeature::resolveTexture(void *user, TextureHandle token) noexcept
{
    const auto &self = *static_cast<UiRenderFeature *>(user);
    for (const auto &texture : self.textures_[self.frame_slot_])
    {
        if (texture.token == token.value)
        {
            return texture.descriptor;
        }
    }
    return VK_NULL_HANDLE;
}

void UiRenderFeature::addPasses(render::RGBuilder &builder)
{
    using namespace render;
    const auto color = builder.findTexture("SceneColor");
    const auto *resource = builder.getResourceDescription(color);
    if (!resource)
    {
        renderContext().reportError(renderError<err::feature::ResourceInitFailed>());
        return;
    }
    const auto format = toVkFormat(std::get<RGTextureDescription>(resource->desc).format);
    auto found = std::find_if(pipelines_.begin(), pipelines_.end(),
                              [format](const auto &value) { return value.first == format; });
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
        .stage(ERenderStage::Overlay)
        .setKernelFn([this, pipeline](const PassRecordContext &context) {
            if (frame_)
            {
                renderer_->renderFrame(frame_->snapshot, context.cmd, pipeline, frame_slot_, serial_);
                for (const auto &image : frame_->images)
                {
                    const auto *record = render::detail::ViewImageAccess::record(image);
                    record->version->last_recording.store(serial_, std::memory_order_release);
                    record->version->last_submission.store(serial_, std::memory_order_release);
                    record->submitted.store(serial_, std::memory_order_release);
                }
            }
        });
}
} // namespace lux::ui::detail

namespace lux::render
{
RenderScene *lookupScene(void *user_state, RenderSceneId scene_id);

Expected<FeatureHandle> UiRenderCreateFn(void *scene, const void *data, std::size_t size)
{
    if (!data || size < sizeof(UiRenderCommConfig))
    {
        return renderFailure<err::comm::PayloadSizeMismatch>(sizeof(UiRenderCommConfig), size);
    }
    UiRenderCommConfig config;
    std::memcpy(&config, data, sizeof(config));
    const auto pixels = static_cast<std::uint64_t>(config.width) * config.height;
    if (!pixels || config.width > 16384 || config.height > 16384 || pixels * 4 != size - sizeof(config))
    {
        return renderFailure<err::internal::InvalidArgument>();
    }
    ui::detail::UiFontAtlasSnapshot font;
    font.width = static_cast<int>(config.width);
    font.height = static_cast<int>(config.height);
    font.pixels.resize(static_cast<std::size_t>(pixels * 4));
    std::memcpy(font.pixels.data(), static_cast<const std::byte *>(data) + sizeof(config), font.pixels.size());
    return addFeature<ui::detail::UiRenderFeature>(scene, std::move(font));
}

void handleUiRenderFrame(GeneralRenderServer::Dispatcher::Ctx &context, const UiRenderFramePayload &payload)
{
    auto *scene = lookupScene(context.user_state, payload.scene_id);
    if (!scene)
    {
        return;
    }
    auto *feature = scene->getFeature(payload.feature);
    if (!feature || feature->typeId() != kUiRenderDescriptor.type)
    {
        scene->renderContext().reportError(
            renderError<err::feature::HandleStale>(payload.feature.index, payload.feature.gen), payload.scene_id.index,
            scene->frameSerial());
        return;
    }
    const auto attachment = CommandPacketView(context.program).attachment(payload.attachment_index);
    if (!attachment)
    {
        scene->renderContext().reportError(attachment.error(), payload.scene_id.index, scene->frameSerial());
        return;
    }
    const auto &record = attachment->get();
    using Input = std::shared_ptr<const ui::UiRenderFrame>;
    if (record.type_id != ui::detail::kUiFrameAttachment || record.object_size != sizeof(Input) || !record.object)
    {
        scene->renderContext().reportError(
            renderError<err::comm::AttachmentTypeMismatch>(ui::detail::kUiFrameAttachment, record.type_id),
            payload.scene_id.index, scene->frameSerial());
        return;
    }
    const auto &input = *static_cast<const Input *>(record.object);
    if (!input || !input->snapshot.valid())
    {
        scene->renderContext().reportError(renderError<err::comm::RequestInvalid>(), payload.scene_id.index,
                                           scene->frameSerial());
        return;
    }
    static_cast<ui::detail::UiRenderFeature *>(feature)->adopt(input);
}
void handleUiRenderClear(GeneralRenderServer::Dispatcher::Ctx &context, const UiRenderClearPayload &payload)
{
    auto *scene = lookupScene(context.user_state, payload.scene_id);
    if (!scene)
    {
        return;
    }
    auto *feature = scene->getFeature(payload.feature);
    if (!feature || feature->typeId() != kUiRenderDescriptor.type)
    {
        scene->renderContext().reportError(
            renderError<err::feature::HandleStale>(payload.feature.index, payload.feature.gen), payload.scene_id.index,
            scene->frameSerial());
        return;
    }
    static_cast<ui::detail::UiRenderFeature *>(feature)->clear();
}

} // namespace lux::render
