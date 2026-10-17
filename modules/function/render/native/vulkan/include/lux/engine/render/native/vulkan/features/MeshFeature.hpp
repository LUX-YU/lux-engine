#pragma once

#include <lux/engine/render/features/MeshFeature.hpp>
#include <lux/engine/render/features/Registration.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureCommands.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshCommands.hpp>
#include <lux/engine/render/native/vulkan/material/MaterialCommands.hpp>
#include <lux/engine/render/native/vulkan/instance/InstanceCommands.hpp>

namespace lux::render
{
    template <class Declaration> struct FeatureProvider;

    template <> struct FeatureProvider<BasicMeshFeature> final
    {
        using Handler = void;
        using Storages = std::tuple<render::TextureStorage, render::MeshStorage,
            render::MaterialStorage, render::InstanceStorage>;
        [[nodiscard]] feature_detail::Implementation definition() const noexcept;
    };

    template <> struct FeatureProvider<DepthOnlyFeature> final
    {
        using Handler = void;
        using Storages = std::tuple<render::MeshStorage, render::MaterialStorage, render::InstanceStorage>;
        [[nodiscard]] feature_detail::Implementation definition() const noexcept;
    };
}
