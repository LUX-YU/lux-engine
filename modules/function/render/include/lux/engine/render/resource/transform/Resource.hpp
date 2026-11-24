#pragma once

#include <lux/engine/render/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resource/ResourceProjection.hpp>
#include <lux/engine/render/resource/transform/Binding.hpp>
#include <lux/engine/render/resource/transform/Data.hpp>

namespace lux::render
{
    struct TransformResource final
    {
        using PerFrameData = std::tuple<TransformRecordUpdate>;
        using Config = void;

        [[nodiscard]] static ResourceProjection projection(ETransformGraphProjection role) noexcept
        {
            const auto bytes = role == ETransformGraphProjection::PAGE_ADDRESSES ?
                kTransformInitialPageAddresses * sizeof(std::uint64_t) : sizeof(TransformMatrix);
            return {
                GraphBufferDesc{bytes}, {GraphBufferRange{0, bytes}}, EGraphUse::SHADER_BUFFER_READ, EGraphUse::STORAGE_READ
            };
        }

        static inline const ResourceDefinition descriptor{
            kTransformResourceName,
            {},
            {},
            cxx::make_basic_type_info<TransformBinding>(),
            "Scene transform records shared by Mesh instances.",
            EResourceDomain::SCENE,
            resourceFrameTypes(static_cast<PerFrameData*>(nullptr))
        };
    };
} // namespace lux::render
