#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp>
#include <lux/engine/function/render/client/features/streaming_feedback/StreamingFeedbackOperation.hpp>
#include <lux/engine/render/renderer/features/streaming_feedback/StreamingFeedbackFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

namespace lux::render
{
    Expected<FeatureHandle> StreamingFeedbackCreateFn(
        void* scene,
        const void* parameters,
        std::size_t parameter_size)
    {
        auto decoded = decodeCommConfig<StreamingFeedbackCommConfig>(
            parameters,
            parameter_size);
        if (!decoded)
            return lux::cxx::unexpected(decoded.error());

        const auto& source = *decoded;
        StreamingFeedbackFeature::Config config{};
        config.cull_shader = source.cull_shader;
        config.compact_shader = source.compact_shader;
        config.mask_vert = source.mask_vert;
        config.mask_frag = source.mask_frag;
        config.composite_frag = source.composite_frag;
        config.descriptor_layout_version =
            source.descriptor_layout_version;
        config.extension_flags = source.extension_flags;
        config.tile_size = source.tile_size;
        config.speed = source.speed;
        config.intensity = source.intensity;
        config.color[0] = source.color[0];
        config.color[1] = source.color[1];
        config.color[2] = source.color[2];
        config.pattern = source.pattern;
        return static_cast<RenderScene*>(scene)
            ->addFeature<StreamingFeedbackFeature>(std::move(config));
    }
}
