#pragma once

#include <lux/engine/render/features/mesh/MeshFeature.hpp>
#include <lux/engine/render/features/Registration.hpp>
#include <lux/engine/render/native/vulkan/material/MaterialCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshInstanceCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureCommandHandler.hpp>

namespace lux::render
{
    template <class Declaration> struct FeatureProvider;

    template <> struct FeatureProvider<BasicMeshFeature> final
    {
        using Handler = void;
        using Storages = std::
            tuple<render::TextureStorage, render::MeshStorage, render::MaterialStorage, render::MeshInstanceStorage>;
        [[nodiscard]] feature_detail::Implementation definition() const noexcept;
    };

    template <> struct FeatureProvider<DepthOnlyFeature> final
    {
        using Handler = void;
        using Storages = std::tuple<render::MeshStorage, render::MaterialStorage, render::MeshInstanceStorage>;
        [[nodiscard]] feature_detail::Implementation definition() const noexcept;
    };
} // namespace lux::render
