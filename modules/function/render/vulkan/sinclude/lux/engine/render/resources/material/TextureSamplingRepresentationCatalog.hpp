#pragma once

#include <lux/engine/function/visibility.h>
#include <lux/engine/description/RenderRepresentation.hpp>
#include <lux/engine/render/resources/material/MaterialGpuTypes.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lux::render
{
    enum class ETextureSamplingCatalogError : std::uint8_t
    {
        INVALID_ID,
        HASH_COLLISION,
        DUPLICATE_ID,
        DUPLICATE_INDEX,
        NON_CONTIGUOUS_INDEX,
        MISSING_RESOLVER,
        MISSING_SHADER_IMPLEMENTATION
    };

    using ResolveTextureSamplingReferenceFn = lux::cxx::expected<
        TextureRefGPU,
        std::string> (*)(
            std::uint32_t resource_index,
            std::uint32_t aux,
            std::uint32_t flags) noexcept;

    struct TextureSamplingRepresentationDescriptor final
    {
        lux::rdesc::TextureSamplingRepresentationId id;
        std::uint32_t representation_index{0u};
        ResolveTextureSamplingReferenceFn resolve_reference{};
        std::vector<std::string> semantic_bindings;
        std::string shader_sampling_implementation;
    };

    class LUX_FUNCTION_PUBLIC TextureSamplingRepresentationCatalog final
    {
    public:
        TextureSamplingRepresentationCatalog() = default;
        TextureSamplingRepresentationCatalog(
            TextureSamplingRepresentationCatalog&&) noexcept = default;
        TextureSamplingRepresentationCatalog& operator=(
            TextureSamplingRepresentationCatalog&&) noexcept = default;
        TextureSamplingRepresentationCatalog(
            const TextureSamplingRepresentationCatalog&) = delete;
        TextureSamplingRepresentationCatalog& operator=(
            const TextureSamplingRepresentationCatalog&) = delete;

        [[nodiscard]] static lux::cxx::expected<
            TextureSamplingRepresentationCatalog,
            ETextureSamplingCatalogError>
        build(
            std::vector<TextureSamplingRepresentationDescriptor> descriptors)
            noexcept;

        [[nodiscard]] const TextureSamplingRepresentationDescriptor* find(
            lux::rdesc::TextureSamplingRepresentationIdView id) const
            noexcept;
        [[nodiscard]] const TextureSamplingRepresentationDescriptor* find(
            std::uint32_t representation_index) const noexcept;

        [[nodiscard]] std::span<const
            TextureSamplingRepresentationDescriptor>
        descriptors() const noexcept
        {
            return descriptors_;
        }

    private:
        explicit TextureSamplingRepresentationCatalog(
            std::vector<TextureSamplingRepresentationDescriptor> descriptors)
            noexcept
            : descriptors_(std::move(descriptors))
        {}

        std::vector<TextureSamplingRepresentationDescriptor> descriptors_;
    };

    [[nodiscard]] LUX_FUNCTION_PUBLIC TextureSamplingRepresentationCatalog
    builtinTextureSamplingRepresentationCatalog() noexcept;
} // namespace lux::render
