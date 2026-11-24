#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/graph/ImportedResourceSlot.hpp>
#include <lux/engine/render/resource/ResourceId.hpp>
#include <lux/engine/render/resource/ResourceProjection.hpp>
#include <string>
#include <vector>

namespace lux::render { struct ResourceRegistration; }

namespace lux::render::runtime_detail
{
    struct EffectResourceManifest final
    {
        EffectResourceManifest() noexcept = default;
        EffectResourceManifest(EffectResourceManifest&&) noexcept = default;
        EffectResourceManifest& operator=(EffectResourceManifest&&) noexcept = default;
        EffectResourceManifest(const EffectResourceManifest&) = delete;
        EffectResourceManifest& operator=(const EffectResourceManifest&) = delete;

        struct RasterSource final
        {
            std::uint32_t stream, programs, draw_list;
        };
        struct BufferExtent final
        {
            std::uint32_t requirement, field;
        };
        std::vector<RasterSource> raster_sources;
        std::vector<BufferExtent> buffer_extents;
        std::vector<std::uint32_t> buffer_binding_regions;
        std::vector<detail::FinalBufferWrite> buffer_writes;
        std::vector<detail::FinalBufferExport> buffer_exports;
        // The Server seals its registration table before any device graph is published.
        struct ScopedResource final
        {
            ResourceTypeId type;
            const ResourceRegistration* factory;
            std::string key;
        };
        struct Operand final
        {
            std::string binding;
            ResourceTypeId type;
            std::uint32_t projection, requirement;
            bool optional;
            // Explicit operands report a fixed contract failure only when an instance is supplied.
            error::ErrorId binding_error;
            std::uint32_t gpu_import = kGraphInvalidIndex;
        };
        struct GpuImport final
        {
            ResourceProjection contract;
            std::uint32_t graph_uses;
            detail::ImportedResourceSlot slot;
        };
        std::vector<ScopedResource> requirements;
        std::vector<Operand> operands;
        std::vector<GpuImport> gpu_imports;
    };
}
