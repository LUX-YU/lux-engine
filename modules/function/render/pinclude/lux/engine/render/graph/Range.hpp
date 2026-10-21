#pragma once

#include <algorithm>
#include <array>
#include <lux/engine/render/graph/RenderGraph.hpp>

namespace lux::render::detail
{
    inline RenderError graphError(
        error::ErrorId type,
        GraphPassId pass = {},
        GraphResourceId resource = {},
        std::uint64_t detail = 0
    ) noexcept
    {
        return {type, {pass.index, resource.index, detail}};
    }

    inline bool writes(EGraphAccess access) noexcept
    {
        return access != EGraphAccess::READ;
    }

    inline bool reads(EGraphAccess access) noexcept
    {
        return access != EGraphAccess::WRITE;
    }

    inline bool validInterval(std::uint64_t start, std::uint64_t count, std::uint64_t limit) noexcept
    {
        return count != 0 && start <= limit && count <= limit - start;
    }

    inline RenderResult<VGraphRange> normalize(const GraphResource& resource, VGraphRange range) noexcept
    {
        if (const auto* buffer = std::get_if<GraphBufferDesc>(&resource.description))
        {
            if (std::holds_alternative<std::monostate>(range))
            {
                range = GraphBufferRange{0, buffer->size};
            }
            const auto* bytes = std::get_if<GraphBufferRange>(&range);
            const bool is_valid = bytes && validInterval(bytes->offset, bytes->size, buffer->size);
            if (!is_valid)
            {
                return cxx::unexpected(graphError(kGraphInvalidRange));
            }
        }
        else
        {
            const auto& image = std::get<GraphImageDesc>(resource.description);
            const auto aspects = rdesc::textureAspectMask(image.format);
            if (std::holds_alternative<std::monostate>(range))
            {
                range = GraphImageRange{aspects, 0, image.mips, 0, image.layers};
            }
            const auto* part = std::get_if<GraphImageRange>(&range);
            const bool is_valid = part && part->aspects != 0 && (part->aspects & ~aspects) == 0 &&
                                  validInterval(part->first_mip, part->mip_count, image.mips) &&
                                  validInterval(part->first_layer, part->layer_count, image.layers);
            if (!is_valid)
            {
                return cxx::unexpected(graphError(kGraphInvalidRange));
            }
        }
        return range;
    }

    // Internal half-open boxes: byte interval, or one aspect x mip x layer.
    // Splitting on actual overlaps avoids constructing a Cartesian grid of all
    // resource boundaries. Each subtraction emits at most six disjoint pieces.
    struct Box final
    {
        std::array<std::uint64_t, 3> begin{};
        std::array<std::uint64_t, 3> end{};
    };

    inline std::vector<Box> boxes(const VGraphRange& range) noexcept
    {
        if (const auto* b = std::get_if<GraphBufferRange>(&range))
        {
            return {{{b->offset, 0, 0}, {b->offset + b->size, 1, 1}}};
        }
        const auto& i = std::get<GraphImageRange>(range);
        std::vector<Box> result;
        for (std::uint32_t bit = 0; bit < 3; ++bit)
        {
            if ((i.aspects & (1u << bit)) == 0)
            {
                continue;
            }
            result.push_back(
                {{bit, i.first_mip, i.first_layer},
                 {bit + 1u, std::uint64_t(i.first_mip) + i.mip_count, std::uint64_t(i.first_layer) + i.layer_count}}
            );
        }
        return result;
    }

    inline std::optional<Box> intersection(Box a, const Box& b) noexcept
    {
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            a.begin[axis] = std::max(a.begin[axis], b.begin[axis]);
            a.end[axis] = std::min(a.end[axis], b.end[axis]);
            if (a.begin[axis] >= a.end[axis])
            {
                return {};
            }
        }
        return a;
    }

    inline void subtract(Box a, const Box& cut, std::vector<Box>& result) noexcept
    {
        const auto overlap = intersection(a, cut);
        if (!overlap)
        {
            result.push_back(a);
            return;
        }
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            if (a.begin[axis] < overlap->begin[axis])
            {
                Box piece = a;
                piece.end[axis] = overlap->begin[axis];
                result.push_back(piece);
                a.begin[axis] = overlap->begin[axis];
            }
            if (a.end[axis] > overlap->end[axis])
            {
                Box piece = a;
                piece.begin[axis] = overlap->end[axis];
                result.push_back(piece);
                a.end[axis] = overlap->end[axis];
            }
        }
    }

    inline VGraphRange toRange(const Box& box, bool image) noexcept
    {
        if (!image)
        {
            return GraphBufferRange{box.begin[0], box.end[0] - box.begin[0]};
        }
        return GraphImageRange{
            1u << static_cast<std::uint32_t>(box.begin[0]),
            static_cast<std::uint32_t>(box.begin[1]),
            static_cast<std::uint32_t>(box.end[1] - box.begin[1]),
            static_cast<std::uint32_t>(box.begin[2]),
            static_cast<std::uint32_t>(box.end[2] - box.begin[2])
        };
    }

    inline bool overlaps(const VGraphRange& left, const VGraphRange& right) noexcept
    {
        // Callers compare normalized ranges of the same logical or physical resource.
        // Box decomposition belongs to subtraction, not this allocation-free predicate.
        if (const auto* a = std::get_if<GraphBufferRange>(&left))
        {
            const auto& b = std::get<GraphBufferRange>(right);
            return a->offset < b.offset + b.size && b.offset < a->offset + a->size;
        }
        const auto& a = std::get<GraphImageRange>(left);
        const auto& b = std::get<GraphImageRange>(right);
        const bool has_common_aspect = (a.aspects & b.aspects) != 0;
        const bool has_mip_overlap = a.first_mip < std::uint64_t(b.first_mip) + b.mip_count &&
                                     b.first_mip < std::uint64_t(a.first_mip) + a.mip_count;
        const bool has_layer_overlap = a.first_layer < std::uint64_t(b.first_layer) + b.layer_count &&
                                       b.first_layer < std::uint64_t(a.first_layer) + a.layer_count;
        return has_common_aspect && has_mip_overlap && has_layer_overlap;
    }
} // namespace lux::render::detail
