#pragma once

#include <lux/engine/scene/RenderFeatureMeta.hpp>
#include <lux/engine/scene/SceneMetaManager.hpp>
#include <lux/engine/scene/render/visibility.h>

namespace lux::scene
{
    enum class ERenderSystemMetaError : std::uint8_t
    {
        INVALID_RENDER_FEATURE,
        DUPLICATE_RENDER_FEATURE,
        RENDER_FEATURE_TYPE_COLLISION,
        INVALID_RENDER_FEATURE_BINDING,
        DUPLICATE_RENDER_FEATURE_BINDING,
        UNKNOWN_RENDER_FEATURE_BINDING,
        UNKNOWN_SCENE_SYSTEM_BINDING,
        CONFIGURATION_REFLECTION_NOT_FOUND,
        CONFIGURATION_DEFAULT_ENCODING_FAILURE,
        UNKNOWN_COMPONENT_SCHEMA,
    };

    struct RenderSystemMetaFailure final
    {
        ERenderSystemMetaError code{};
        std::uint64_t subject_hash{};
        lux::serialization::SerializationFailure configuration{};
    };

    // Render feature metadata belongs to the RenderSystem assembly, not generic Scene metadata.
    class LUX_ENGINE_SCENE_RENDER_PUBLIC RenderSystemMetadata final
    {
      public:
        [[nodiscard]] static lux::cxx::expected<RenderSystemMetadata, RenderSystemMetaFailure> build(
            const SceneMetaManager &scene, std::vector<render::RenderFeatureRegistration> features,
            std::span<const RenderFeatureSceneBinding> bindings);

        RenderSystemMetadata(RenderSystemMetadata &&) noexcept;
        RenderSystemMetadata &operator=(RenderSystemMetadata &&) noexcept;
        ~RenderSystemMetadata();
        RenderSystemMetadata(const RenderSystemMetadata &) = delete;
        RenderSystemMetadata &operator=(const RenderSystemMetadata &) = delete;

        [[nodiscard]] const RenderFeatureMeta *feature(render::FeatureTypeId) const noexcept;
        [[nodiscard]] const RenderFeatureMeta *feature(std::string_view) const noexcept;
        [[nodiscard]] std::span<const RenderFeatureMeta> features() const noexcept;

      private:
        struct Impl;
        explicit RenderSystemMetadata(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::scene
