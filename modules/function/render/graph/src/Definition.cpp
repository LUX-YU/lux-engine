#include <lux/engine/render/graph/Definition.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace lux::render
{
    namespace
    {
        constexpr std::array kErrors{
            error::ErrorDescriptor{
                "lux.render.graph.ambiguous_producer",
                "Ambiguous version or provider (pass, resource)",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            },
            error::ErrorDescriptor{
                "lux.render.graph.scope_conflict",
                "Invocation scope conflict (pass, resource)",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            },
            error::ErrorDescriptor{
                "lux.render.graph.conditional_input",
                "Conditional input lacks a valid alternative (pass, resource)",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            },
            error::ErrorDescriptor{
                "lux.render.graph.invalid_output",
                "Invalid output effect or version (pass, resource)",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            },
            error::ErrorDescriptor{
                "lux.render.graph.invalid_import",
                "Invalid logical import initialization contract (pass, resource)",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            },

            error::ErrorDescriptor{
                "lux.render.graph.invalid_resource",
                "Invalid resource declaration (resource)",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED}
            },
            error::ErrorDescriptor{
                "lux.render.graph.invalid_use",
                "Invalid or duplicate resource use (pass, resource)",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            },
            error::ErrorDescriptor{
                "lux.render.graph.invalid_dependency",
                "Invalid dependency (before, after)",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            },
            error::ErrorDescriptor{
                "lux.render.graph.cycle",
                "Cyclic dependencies (path endpoints; detailed diagnostic available)",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            },
            error::ErrorDescriptor{
                "lux.render.graph.missing_producer",
                "Transient read before write (pass, resource)",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            },
            error::ErrorDescriptor{
                "lux.render.graph.invalid_binding",
                "Invalid frame import (binding position)",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED}
            }
        };

        bool validUse(const GraphResource& resource, const GraphResourceUse& use) noexcept
        {
            const bool is_valid_access = use.access == EGraphAccess::READ || use.access == EGraphAccess::WRITE ||
                                         use.access == EGraphAccess::READ_WRITE;
            if (!is_valid_access)
            {
                return false;
            }
            const bool is_image = resource.kind() == EGraphResourceKind::IMAGE;
            const bool is_read = use.access == EGraphAccess::READ;
            switch (use.usage)
            {
            case EGraphUsage::TRANSFER:
                return use.access != EGraphAccess::READ_WRITE;
            case EGraphUsage::SHADER:
                return true;
            case EGraphUsage::COLOR_ATTACHMENT:
            case EGraphUsage::RESOLVE:
                return is_image &&
                       rdesc::supportsTextureUsage(resource.texture().format, rdesc::ETextureUsage::COLOR_ATTACHMENT);
            case EGraphUsage::DEPTH_ATTACHMENT:
                return is_image && rdesc::supportsTextureUsage(
                                       resource.texture().format,
                                       rdesc::ETextureUsage::DEPTH_STENCIL_ATTACHMENT
                                   );
            case EGraphUsage::INPUT_ATTACHMENT:
                return is_image;
            case EGraphUsage::VERTEX:
            case EGraphUsage::INDEX:
            case EGraphUsage::INDIRECT:
            case EGraphUsage::UNIFORM:
                return !is_image && is_read;
            case EGraphUsage::PRESENT:
                return is_image && is_read && resource.origin == EGraphResourceOrigin::IMPORTED;
            }
            return false;
        }

        bool validFieldKind(const CapturedFieldBinding& field, const GraphResource& resource) noexcept
        {
            if (field.resource_kind != resource.kind())
            {
                return false;
            }
            using ERole = rdesc::EPassFieldRole;
            switch (field.role)
            {
            case ERole::SAMPLED_READ:
            case ERole::STORAGE_READ:
            case ERole::STORAGE_WRITE:
            case ERole::STORAGE_READ_WRITE:
            case ERole::COLOR_ATTACHMENT:
            case ERole::DEPTH_STENCIL:
            case ERole::RESOLVE:
            case ERole::INPUT_ATTACHMENT:
                return resource.kind() == EGraphResourceKind::IMAGE;
            case ERole::UNIFORM_READ:
            case ERole::READ_ONLY_STORAGE:
            case ERole::READ_WRITE_STORAGE:
            case ERole::VERTEX:
            case ERole::INDEX:
            case ERole::INDIRECT:
                return resource.kind() == EGraphResourceKind::BUFFER;
            case ERole::TRANSFER_SOURCE:
            case ERole::TRANSFER_DESTINATION:
                return true; // Both native kinds; captured wrapper kind must still match.
            case ERole::SAMPLER:
                return false; // A sampler is not a Graph resource.
            }
            return false;
        }

        bool validDepthStencil(const CapturedFieldBinding& field, const GraphResource& resource) noexcept
        {
            const auto aspects = static_cast<std::uint32_t>(field.image_range.aspect);
            const auto available = rdesc::textureAspectMask(resource.texture().format);
            const bool has_depth = (aspects & static_cast<std::uint32_t>(EAspect::DEPTH)) != 0;
            const bool has_stencil = (aspects & static_cast<std::uint32_t>(EAspect::STENCIL)) != 0;
            const bool uses_depth = field.load != ELoadOp::DISCARD || field.store != EStoreOp::DISCARD;
            const bool uses_stencil =
                field.stencil_load != ELoadOp::DISCARD || field.stencil_store != EStoreOp::DISCARD;
            const bool is_invalid_aspect = aspects == 0 || (aspects & ~available) != 0 ||
                                           (aspects & ~static_cast<std::uint32_t>(EAspect::DEPTH_STENCIL)) != 0;
            const bool is_missing_aspect = (uses_depth && !has_depth) || (uses_stencil && !has_stencil);
            return !is_invalid_aspect && !is_missing_aspect;
        }

        bool overlaps(const GraphResourceUse& a, const GraphResourceUse& b, bool image) noexcept
        {
            if (!image)
            {
                return std::get<BufferRange>(a.range).byte_offset <
                           std::get<BufferRange>(b.range).byte_offset + std::get<BufferRange>(b.range).byte_count &&
                       std::get<BufferRange>(b.range).byte_offset <
                           std::get<BufferRange>(a.range).byte_offset + std::get<BufferRange>(a.range).byte_count;
            }
            const auto& x = std::get<ImageRange>(a.range);
            const auto& y = std::get<ImageRange>(b.range);
            const bool same_aspect = (static_cast<unsigned>(x.aspect) & static_cast<unsigned>(y.aspect)) != 0;
            const bool same_mip = x.base_mip < y.base_mip + y.mip_count && y.base_mip < x.base_mip + x.mip_count;
            const bool same_layer =
                x.base_layer < y.base_layer + y.layer_count && y.base_layer < x.base_layer + x.layer_count;
            return same_aspect && same_mip && same_layer;
        }

        bool validRange(const GraphResource& resource, const GraphResourceUse& use) noexcept
        {
            if (resource.kind() == EGraphResourceKind::BUFFER)
            {
                const auto& range = std::get<BufferRange>(use.range);
                const bool valid_layout = use.byte_alignment != 0 && range.byte_offset % use.byte_alignment == 0 &&
                                          range.byte_count >= use.minimum_bytes &&
                                          (use.element_stride == 0 || range.byte_count % use.element_stride == 0);
                return valid_layout && range.byte_count != 0 && range.byte_offset <= resource.buffer().byte_size &&
                       range.byte_count <= resource.buffer().byte_size - range.byte_offset;
            }
            const auto& range = std::get<ImageRange>(use.range);
            const auto& texture = resource.texture();
            const bool valid_mips = range.mip_count != 0 && range.base_mip <= texture.mip_count &&
                                    range.mip_count <= texture.mip_count - range.base_mip;
            const bool valid_layers = range.layer_count != 0 && range.base_layer <= texture.array_layers &&
                                      range.layer_count <= texture.array_layers - range.base_layer;
            const auto valid_aspects = rdesc::textureAspectMask(texture.format);
            const auto aspects = static_cast<unsigned>(range.aspect);
            const bool valid_aspect = aspects != 0 && (aspects & ~valid_aspects) == 0;
            return valid_mips && valid_layers && valid_aspect;
        }
    } // namespace

    std::span<const error::ErrorDescriptor> renderGraphErrorDescriptors() noexcept
    {
        return kErrors;
    }

    RenderGraphDefinition::RenderGraphDefinition(
        std::vector<GraphResource> resources,
        std::vector<GraphPass> passes,
        std::vector<GraphDependency> dependencies,
        std::vector<GraphOutput> outputs,
        std::vector<GraphProvider> providers
    ) noexcept
        : resources_(std::move(resources)), passes_(std::move(passes)), dependencies_(std::move(dependencies)),
          outputs_(std::move(outputs)), providers_(std::move(providers))
    {
    }

    RenderResult<RenderGraphDefinition> RenderGraphDefinition::create(
        std::vector<GraphResource> resources,
        std::vector<GraphPass> passes,
        std::vector<GraphDependency> dependencies,
        std::vector<GraphOutput> outputs,
        std::vector<GraphProvider> providers
    ) noexcept
    {
        const bool is_too_large = resources.size() > std::numeric_limits<std::uint32_t>::max() ||
                                  passes.size() > std::numeric_limits<std::uint32_t>::max();
        if (is_too_large)
        {
            return cxx::unexpected(RenderError{kGraphInvalidResource, {resources.size()}});
        }
        for (std::size_t index = 0; index < resources.size(); ++index)
        {
            const auto& resource = resources[index];
            const bool is_invalid_origin =
                resource.origin != EGraphResourceOrigin::TRANSIENT && resource.origin != EGraphResourceOrigin::IMPORTED;
            const bool is_invalid_target =
                resource.target_semantic.isValid() && resource.kind() != EGraphResourceKind::IMAGE;
            const bool is_image = resource.kind() == EGraphResourceKind::IMAGE;
            const auto texture = is_image ? resource.texture() : TextureDesc{};
            const auto buffer = is_image ? BufferDesc{} : resource.buffer();
            const bool is_invalid_texture =
                is_image && (texture.width == 0 || texture.height == 0 || texture.depth == 0 ||
                             texture.mip_count == 0 || texture.array_layers == 0 || texture.samples == 0 ||
                             (texture.samples & (texture.samples - 1)) != 0 || texture.samples > 64 ||
                             rdesc::textureFormatClass(texture.format) == rdesc::ETextureFormatClass::INVALID ||
                             texture.dimension > ETextureDimension::CUBE || texture.extent_kind > EExtentKind::DYNAMIC);
            const bool is_invalid_buffer = !is_image && (buffer.byte_size == 0 || buffer.alignment == 0 ||
                                                         (buffer.alignment & (buffer.alignment - 1)) != 0);
            const bool invalid_identity =
                !resource.canonical_name.empty() && resource.semantic != graphResourceKey(resource.canonical_name);
            const bool is_invalid_scope = invalid_identity || resource.persistent_scope > EPersistentScope::VIEW ||
                                          (resource.origin == EGraphResourceOrigin::TRANSIENT &&
                                           resource.persistent_scope != EPersistentScope::NONE);
            const bool is_invalid_resource =
                is_invalid_origin || is_invalid_target || is_invalid_texture || is_invalid_buffer || is_invalid_scope;
            if (is_invalid_resource)
            {
                return cxx::unexpected(RenderError{kGraphInvalidResource, {index + 1}});
            }
            for (std::size_t previous = 0; previous < index; ++previous)
            {
                const bool is_duplicate_target =
                    (resource.target_semantic.isValid() &&
                     resource.target_semantic == resources[previous].target_semantic) ||
                    (resource.semantic.isValid() && resource.semantic == resources[previous].semantic);
                if (is_duplicate_target)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidResource, {index + 1}});
                }
            }
        }
        for (std::size_t index = 0; index < passes.size(); ++index)
        {
            auto& pass = passes[index];
            if (pass.canonical_name.empty())
            {
                pass.canonical_name = "anonymous.pass." + std::to_string(index + 1);
                pass.key = passKey(pass.canonical_name);
            }
            const bool invalid_pass_identity = pass.key != passKey(pass.canonical_name) ||
                                               std::any_of(
                                                   passes.begin(),
                                                   passes.begin() + index,
                                                   [&](const auto& previous) { return previous.key == pass.key; }
                                               );
            const bool invalid_pass = pass.kind > EPassKind::HOST_READBACK || pass.scope > EExecutionScope::TARGET ||
                                      (pass.invocation_inputs & ~15u) != 0 || invalid_pass_identity;
            if (invalid_pass)
            {
                return cxx::unexpected(RenderError{kGraphInvalidUse, {index + 1, 0}});
            }
            for (const auto& field : pass.bindings)
            {
                const bool is_invalid_attachment =
                    field.load > ELoadOp::CLEAR || field.store > EStoreOp::STORE ||
                    field.stencil_load > ELoadOp::CLEAR || field.stencil_store > EStoreOp::STORE ||
                    !std::isfinite(field.clear_depth) || field.clear_depth < 0.0f || field.clear_depth > 1.0f;
                if (is_invalid_attachment)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidUse, {index + 1, field.resource.value()}});
                }
                if (field.role != rdesc::EPassFieldRole::SAMPLER)
                {
                    const bool is_invalid_id = !field.resource.isValid() || field.resource.value() > resources.size();
                    if (is_invalid_id || !validFieldKind(field, resources[field.resource.value() - 1]))
                    {
                        return cxx::unexpected(RenderError{kGraphInvalidUse, {index + 1, field.resource.value()}});
                    }
                    const bool is_invalid_depth_stencil =
                        field.role == rdesc::EPassFieldRole::DEPTH_STENCIL &&
                        !validDepthStencil(field, resources[field.resource.value() - 1]);
                    if (is_invalid_depth_stencil)
                    {
                        return cxx::unexpected(RenderError{kGraphInvalidUse, {index + 1, field.resource.value()}});
                    }
                    const bool is_invalid_clear =
                        field.role == rdesc::EPassFieldRole::COLOR_ATTACHMENT && field.load == ELoadOp::CLEAR &&
                        !matchesClearFormat(field.clear, resources[field.resource.value() - 1].texture().format);
                    if (is_invalid_clear)
                    {
                        return cxx::unexpected(RenderError{kGraphInvalidUse, {index + 1, field.resource.value()}});
                    }
                }
                if (field.role == rdesc::EPassFieldRole::RESOLVE)
                {
                    const auto paired = std::find_if(
                        pass.bindings.begin(),
                        pass.bindings.end(),
                        [&](const auto& candidate)
                        {
                            return candidate.path == field.paired_texture &&
                                   candidate.array_element == field.array_element &&
                                   candidate.role == rdesc::EPassFieldRole::COLOR_ATTACHMENT;
                        }
                    );
                    const bool valid_ids = paired != pass.bindings.end() && field.resource.isValid() &&
                                           field.resource.value() <= resources.size() && paired->resource.isValid() &&
                                           paired->resource.value() <= resources.size();
                    if (!valid_ids)
                    {
                        return cxx::unexpected(RenderError{kGraphInvalidUse, {index + 1, field.resource.value()}});
                    }
                    const auto& source = resources[paired->resource.value() - 1].texture();
                    const auto& target = resources[field.resource.value() - 1].texture();
                    const bool is_invalid_resolve = source.samples <= 1 || target.samples != 1 ||
                                                    source.format != target.format || source.width != target.width ||
                                                    source.height != target.height || source.depth != target.depth ||
                                                    source.array_layers != target.array_layers;
                    if (is_invalid_resolve)
                    {
                        return cxx::unexpected(RenderError{kGraphInvalidUse, {index + 1, field.resource.value()}});
                    }
                }
                if (field.role != rdesc::EPassFieldRole::STORAGE_WRITE &&
                    field.role != rdesc::EPassFieldRole::STORAGE_READ &&
                    field.role != rdesc::EPassFieldRole::STORAGE_READ_WRITE)
                {
                    continue;
                }
                if (!field.resource.isValid() || field.resource.value() > resources.size())
                {
                    return cxx::unexpected(RenderError{kGraphInvalidUse, {index + 1, field.resource.value()}});
                }
                const auto format = resources[field.resource.value() - 1].texture().format;
                const bool is_mismatch =
                    !rdesc::supportsTextureUsage(format, rdesc::ETextureUsage::STORAGE) ||
                    (field.image_format == "rgba16f" && format != rdesc::ETextureFormat::RGBA16_SFLOAT) ||
                    (field.image_format == "r32f" && format != lux::rdesc::ETextureFormat::R32_SFLOAT) ||
                    (field.image_format == "rgba32f" && format != lux::rdesc::ETextureFormat::RGBA32_SFLOAT) ||
                    (field.image_format == "rgba8" && format != lux::rdesc::ETextureFormat::RGBA8_UNORM);
                if (is_mismatch)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidUse, {index + 1, field.resource.value()}});
                }
            }
            auto& uses = passes[index].uses;
            std::stable_sort(
                uses.begin(),
                uses.end(),
                [](const auto& left, const auto& right) { return left.resource.value() < right.resource.value(); }
            );
            // Prove bounds before overlap arithmetic; subtraction in validRange avoids overflow.
            for (auto& use : uses)
            {
                const bool is_invalid_id = !use.resource.isValid() || use.resource.value() > resources.size();
                if (!is_invalid_id)
                {
                    const auto& resource = resources[use.resource.value() - 1];
                    if (std::holds_alternative<WholeResource>(use.range))
                    {
                        use.range = resource.kind() == EGraphResourceKind::IMAGE
                                        ? VGraphRange{ImageRange{
                                              static_cast<EAspect>(rdesc::textureAspectMask(resource.texture().format)),
                                              0,
                                              resource.texture().mip_count,
                                              0,
                                              resource.texture().array_layers
                                          }}
                                        : VGraphRange{BufferRange{0, resource.buffer().byte_size}};
                    }
                    const bool is_wrong_range =
                        (resource.kind() == EGraphResourceKind::IMAGE) != std::holds_alternative<ImageRange>(use.range);
                    if (is_wrong_range)
                    {
                        return cxx::unexpected(RenderError{kGraphInvalidUse, {index + 1, use.resource.value()}});
                    }
                    ImageRange image =
                        resource.kind() == EGraphResourceKind::IMAGE ? std::get<ImageRange>(use.range) : ImageRange{};
                    BufferRange buffer = resource.kind() == EGraphResourceKind::BUFFER
                                             ? std::get<BufferRange>(use.range)
                                             : BufferRange{};
                    if (resource.kind() == EGraphResourceKind::IMAGE)
                    {
                        if (image.base_mip <= resource.texture().mip_count && image.mip_count == kRemainingSubresources)
                        {
                            image.mip_count = resource.texture().mip_count - image.base_mip;
                        }
                        if (image.base_layer <= resource.texture().array_layers &&
                            image.layer_count == kRemainingSubresources)
                        {
                            image.layer_count = resource.texture().array_layers - image.base_layer;
                        }
                    }
                    else if (buffer.byte_offset <= resource.buffer().byte_size && buffer.byte_count == kRemainingBytes)
                    {
                        buffer.byte_count = resource.buffer().byte_size - buffer.byte_offset;
                    }
                    use.range = resource.kind() == EGraphResourceKind::IMAGE ? VGraphRange{image} : VGraphRange{buffer};
                    if (use.field_index < passes[index].bindings.size())
                    {
                        auto& field = passes[index].bindings[use.field_index];
                        field.image_range = image;
                        field.buffer_range = buffer;
                        const bool shader_image = field.role == rdesc::EPassFieldRole::SAMPLED_READ ||
                                                  field.role == rdesc::EPassFieldRole::STORAGE_READ ||
                                                  field.role == rdesc::EPassFieldRole::STORAGE_WRITE ||
                                                  field.role == rdesc::EPassFieldRole::STORAGE_READ_WRITE;
                        if (shader_image)
                        {
                            const auto expected_dimension =
                                field.dimension.starts_with("1D")
                                    ? ETextureDimension::D1
                                    : (field.dimension.starts_with("3D")
                                           ? ETextureDimension::D3
                                           : (field.dimension.starts_with("Cube") ? ETextureDimension::CUBE
                                                                                  : ETextureDimension::D2));
                            const bool arrayed = field.dimension.ends_with("Array");
                            const bool cube = expected_dimension == ETextureDimension::CUBE;
                            const bool invalid_layers =
                                cube ? (image.layer_count % 6 != 0 || (!arrayed && image.layer_count != 6))
                                     : (!arrayed && image.layer_count != 1);
                            const bool multisampled = field.dimension.find("MS") != std::string::npos;
                            const bool invalid_view = resource.texture().dimension != expected_dimension ||
                                                      invalid_layers ||
                                                      multisampled != (resource.texture().samples > 1) ||
                                                      image.aspect == EAspect::DEPTH_STENCIL;
                            if (invalid_view)
                            {
                                return cxx::unexpected(RenderError{kGraphInvalidUse, {index + 1, use.resource.value()}}
                                );
                            }
                        }
                    }
                }
                const bool is_invalid_use = is_invalid_id || !validUse(resources[use.resource.value() - 1], use) ||
                                            !validRange(resources[use.resource.value() - 1], use);
                if (is_invalid_use)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidUse, {index + 1, use.resource.value()}});
                }
            }
            const bool has_depth_stencil = std::any_of(
                uses.begin(),
                uses.end(),
                [&](const auto& use)
                {
                    return use.usage == EGraphUsage::DEPTH_ATTACHMENT && use.field_index < pass.bindings.size() &&
                           pass.bindings[use.field_index].role == rdesc::EPassFieldRole::DEPTH_STENCIL;
                }
            );
            if (has_depth_stencil)
            {
                std::vector<GraphResourceUse> aspect_uses;
                for (const auto& use : uses)
                {
                    const bool has_depth_field =
                        use.usage == EGraphUsage::DEPTH_ATTACHMENT && use.field_index < pass.bindings.size() &&
                        pass.bindings[use.field_index].role == rdesc::EPassFieldRole::DEPTH_STENCIL;
                    if (!has_depth_field)
                    {
                        aspect_uses.push_back(use);
                        continue;
                    }
                    const auto& field = pass.bindings[use.field_index];
                    const auto range = std::get<ImageRange>(use.range);
                    for (const auto aspect : {EAspect::DEPTH, EAspect::STENCIL})
                    {
                        if ((static_cast<unsigned>(range.aspect) & static_cast<unsigned>(aspect)) == 0)
                        {
                            continue;
                        }
                        auto split = use;
                        auto split_range = range;
                        split_range.aspect = aspect;
                        split.range = split_range;
                        const auto load = aspect == EAspect::STENCIL ? field.stencil_load : field.load;
                        split.access = load == ELoadOp::LOAD ? EGraphAccess::READ_WRITE : EGraphAccess::WRITE;
                        aspect_uses.push_back(std::move(split));
                    }
                }
                uses = std::move(aspect_uses);
            }

            for (auto& use : uses)
            {
                const bool is_invalid_id = !use.resource.isValid() || use.resource.value() > resources.size();
                bool is_duplicate = false;
                if (!is_invalid_id)
                {
                    for (const auto& other : uses)
                    {
                        if (&other == &use)
                        {
                            break;
                        }
                        const bool same_resource = other.resource == use.resource;
                        const bool is_image = resources[use.resource.value() - 1].kind() == EGraphResourceKind::IMAGE;
                        const bool reads_only = other.access == EGraphAccess::READ && use.access == EGraphAccess::READ;
                        const auto& read = use.local_read ? use : other;
                        const auto& write = use.local_read ? other : use;
                        const bool explicit_local_read =
                            pass.kind == EPassKind::GRAPHICS && read.local_read &&
                            read.usage == EGraphUsage::INPUT_ATTACHMENT && read.access == EGraphAccess::READ &&
                            !write.local_read && write.access != EGraphAccess::READ && read.range == write.range &&
                            (write.usage == EGraphUsage::COLOR_ATTACHMENT ||
                             write.usage == EGraphUsage::DEPTH_ATTACHMENT);
                        is_duplicate = is_duplicate || (same_resource && !reads_only && !explicit_local_read &&
                                                        overlaps(other, use, is_image));
                    }
                }
                const bool is_invalid_use = is_invalid_id || is_duplicate ||
                                            !validUse(resources[use.resource.value() - 1], use) ||
                                            !validRange(resources[use.resource.value() - 1], use);
                if (is_invalid_use)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidUse, {index + 1, use.resource.value()}});
                }
                const auto& resource = resources[use.resource.value() - 1];
            }
        }
        for (const auto& dependency : dependencies)
        {
            const bool is_invalid_id = !dependency.before.isValid() || !dependency.after.isValid() ||
                                       dependency.before.value() > passes.size() ||
                                       dependency.after.value() > passes.size();
            const bool is_self_dependency = dependency.before == dependency.after;
            const bool is_invalid_dependency = is_invalid_id || is_self_dependency;
            if (is_invalid_dependency)
            {
                return cxx::unexpected(
                    RenderError{kGraphInvalidDependency, {dependency.before.value(), dependency.after.value()}}
                );
            }
        }
        std::sort(
            dependencies.begin(),
            dependencies.end(),
            [](const auto& left, const auto& right)
            {
                if (left.before != right.before)
                {
                    return left.before.value() < right.before.value();
                }
                return left.after.value() < right.after.value();
            }
        );
        dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
        return RenderGraphDefinition{
            std::move(resources),
            std::move(passes),
            std::move(dependencies),
            std::move(outputs),
            std::move(providers)
        };
    }
} // namespace lux::render
