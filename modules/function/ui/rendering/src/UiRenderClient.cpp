#include <lux/engine/function/render/client/FeatureOpSend.hpp>
#include <lux/engine/render/detail/ViewImageLifetime.hpp>
#include <lux/engine/ui/detail/ContextAccess.hpp>
#include <lux/engine/ui/rendering/detail/UiRenderFeature.hpp>

#include <algorithm>
#include <cstring>

namespace lux::ui
{
lux::cxx::expected<std::vector<std::byte>, EUiInitError> makeUiRenderConfiguration(Context &context)
{
    auto font = detail::ContextAccess::fontAtlas(context);
    if (!font)
    {
        return lux::cxx::unexpected(font.error());
    }
    render::UiRenderCommConfig config{static_cast<std::uint32_t>(font->width),
                                      static_cast<std::uint32_t>(font->height)};
    std::vector<std::byte> bytes(sizeof(config) + font->pixels.size());
    std::memcpy(bytes.data(), &config, sizeof(config));
    std::memcpy(bytes.data() + sizeof(config), font->pixels.data(), font->pixels.size());
    return bytes;
}

render::Expected<void> appendUiFrame(render::RenderProgramSession::Builder &builder,
                                     const render::UiRenderOperationIds &operations,
                                     const render::RenderSceneLease &scene, render::FeatureHandle feature,
                                     const std::shared_ptr<const UiRenderFrame> &input)
{
    if (!operations.valid() || !scene || !feature.isValid() || !input || !input->snapshot.valid() ||
        input->sequence == 0)
    {
        return render::renderFailure<render::err::comm::RequestInvalid>();
    }
    for (auto texture : input->snapshot.textures())
    {
        if (texture.value == 0)
        {
            continue;
        }
        const auto found = std::find_if(input->images.begin(), input->images.end(), [&](const auto &image) {
            return image.texture == texture.value && image.lease.valid();
        });
        if (found == input->images.end())
        {
            return render::renderFailure<render::err::comm::RequestInvalid>();
        }
    }
    for (const auto &image : input->images)
    {
        const auto *record = render::detail::ViewImageAccess::record(image);
        if (!record || !record->version || record->version->texture != image.texture)
        {
            return render::renderFailure<render::err::comm::RequestInvalid>();
        }
        if (record->version->scene == scene.id())
        {
            return render::renderFailure<render::err::graph::DependencyCycle>();
        }
    }
    const auto index =
        builder.emplaceAttachment<std::shared_ptr<const UiRenderFrame>>(detail::kUiFrameAttachment, input);
    builder.emplaceAttachment<render::RenderSceneLease>(detail::kUiSceneUseAttachment, scene.retain());
    builder.push(render::opcode_of_v<render::UiRenderFrameOp>, operations.id<render::UiRenderFrameOp>(),
                 render::UiRenderFramePayload{scene.id(), feature, index});
    return {};
}
render::Expected<void> appendUiClear(render::RenderProgramSession::Builder &builder,
                                     const render::UiRenderOperationIds &operations,
                                     const render::RenderSceneLease &scene, render::FeatureHandle feature)
{
    if (!operations.valid() || !scene || !feature.isValid())
    {
        return render::renderFailure<render::err::comm::RequestInvalid>();
    }
    builder.emplaceAttachment<render::RenderSceneLease>(detail::kUiSceneUseAttachment, scene.retain());
    builder.push(render::opcode_of_v<render::UiRenderClearOp>, operations.id<render::UiRenderClearOp>(),
                 render::UiRenderClearPayload{scene.id(), feature});
    return {};
}

} // namespace lux::ui
