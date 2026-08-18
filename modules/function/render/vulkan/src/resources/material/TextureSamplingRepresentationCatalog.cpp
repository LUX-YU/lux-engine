#include <lux/engine/render/resources/material/TextureSamplingRepresentationCatalog.hpp>

#include <algorithm>
#include <exception>
#include <string_view>

namespace lux::render
{
    namespace
    {
        [[nodiscard]] bool isCanonicalTextureSamplingName(
            std::string_view name) noexcept
        {
            if (name.empty() || name.front() == '.' || name.back() == '.')
                return false;
            bool has_dot = false;
            bool previous_dot = false;
            for (const char value : name)
            {
                const bool dot = value == '.';
                if (dot)
                {
                    if (previous_dot)
                        return false;
                    has_dot = true;
                }
                else if (!((value >= 'a' && value <= 'z') ||
                           (value >= '0' && value <= '9') ||
                           value == '_' || value == '-'))
                {
                    return false;
                }
                previous_dot = dot;
            }
            return has_dot;
        }

        template <class Tag>
        [[nodiscard]] bool stableIdCollision(
            lux::cxx::StableNameIdView<Tag> lhs,
            lux::cxx::StableNameIdView<Tag> rhs) noexcept
        {
            return lhs.hash() == rhs.hash() && lhs.name() != rhs.name();
        }

        lux::cxx::expected<TextureRefGPU, std::string> resolveBindless(
            std::uint32_t resource_index,
            std::uint32_t aux,
            std::uint32_t flags) noexcept
        {
            return TextureRefGPU{0u, resource_index, aux, flags};
        }

    } // namespace

    lux::cxx::expected<
        TextureSamplingRepresentationCatalog,
        ETextureSamplingCatalogError>
    TextureSamplingRepresentationCatalog::build(
        std::vector<TextureSamplingRepresentationDescriptor> descriptors)
        noexcept
    {
        for (const auto& descriptor : descriptors)
        {
            if (!descriptor.id.isValid() ||
                !isCanonicalTextureSamplingName(descriptor.id.name()))
            {
                return lux::cxx::unexpected<ETextureSamplingCatalogError>(
                    ETextureSamplingCatalogError::INVALID_ID);
            }
            if (!descriptor.resolve_reference)
            {
                return lux::cxx::unexpected<ETextureSamplingCatalogError>(
                    ETextureSamplingCatalogError::MISSING_RESOLVER);
            }
            if (descriptor.shader_sampling_implementation.empty())
            {
                return lux::cxx::unexpected<ETextureSamplingCatalogError>(
                    ETextureSamplingCatalogError::
                        MISSING_SHADER_IMPLEMENTATION);
            }
        }
        std::ranges::sort(
            descriptors,
            {},
            &TextureSamplingRepresentationDescriptor::representation_index);
        for (std::size_t index = 0u; index < descriptors.size(); ++index)
        {
            if (descriptors[index].representation_index != index)
            {
                return lux::cxx::unexpected<ETextureSamplingCatalogError>(
                    index != 0u &&
                            descriptors[index - 1u].representation_index ==
                                descriptors[index].representation_index
                        ? ETextureSamplingCatalogError::DUPLICATE_INDEX
                        : ETextureSamplingCatalogError::NON_CONTIGUOUS_INDEX);
            }
        }
        auto by_id = descriptors;
        std::ranges::sort(
            by_id,
            {},
            [](const TextureSamplingRepresentationDescriptor& descriptor)
            {
                return descriptor.id.hash();
            });
        for (std::size_t index = 1u; index < by_id.size(); ++index)
        {
            const auto previous = by_id[index - 1u].id.view();
            const auto current = by_id[index].id.view();
            if (stableIdCollision(previous, current))
            {
                return lux::cxx::unexpected<ETextureSamplingCatalogError>(
                    ETextureSamplingCatalogError::HASH_COLLISION);
            }
            if (previous == current)
            {
                return lux::cxx::unexpected<ETextureSamplingCatalogError>(
                    ETextureSamplingCatalogError::DUPLICATE_ID);
            }
        }
        return TextureSamplingRepresentationCatalog{
            std::move(descriptors)};
    }

    const TextureSamplingRepresentationDescriptor*
    TextureSamplingRepresentationCatalog::find(
        TextureSamplingRepresentationIdView id) const noexcept
    {
        for (const auto& descriptor : descriptors_)
        {
            if (descriptor.id.view() == id)
                return &descriptor;
        }
        return nullptr;
    }

    const TextureSamplingRepresentationDescriptor*
    TextureSamplingRepresentationCatalog::find(
        std::uint32_t representation_index) const noexcept
    {
        return representation_index < descriptors_.size()
            ? &descriptors_[representation_index]
            : nullptr;
    }

    TextureSamplingRepresentationCatalog
    builtinTextureSamplingRepresentationCatalog() noexcept
    {
        std::vector<TextureSamplingRepresentationDescriptor> descriptors;
        descriptors.push_back({
            ownTextureSamplingRepresentationId(
                kBindlessTextureSamplingRepresentation),
            0u,
            &resolveBindless,
            {"sampled_image_2d", "sampler"},
            "luxSampleTexture.bindless"});
        auto result = TextureSamplingRepresentationCatalog::build(
            std::move(descriptors));
        if (!result)
            std::terminate();
        return std::move(*result);
    }
} // namespace lux::render
