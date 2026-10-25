#include <lux/engine/render/extensions/graph/RenderGraphBuilder.hpp>

#include <bit>
#include <lux/engine/render/graph/Range.hpp>
#include <numeric>

namespace lux::render
{
    using detail::graphError;

    namespace
    {
        bool compatibleUse(const VGraphResourceDesc& description, EGraphUse usage) noexcept
        {
            if (usage > EGraphUse::DEPTH_ATTACHMENT)
            {
                return false;
            }
            const auto* image = std::get_if<GraphImageDesc>(&description);
            switch (usage)
            {
            case EGraphUse::COLOR_ATTACHMENT:
                return image && rdesc::supportsTextureUsage(image->format, rdesc::ETextureUsage::COLOR_ATTACHMENT);
            case EGraphUse::DEPTH_ATTACHMENT:
                return image &&
                       rdesc::supportsTextureUsage(image->format, rdesc::ETextureUsage::DEPTH_STENCIL_ATTACHMENT);
            case EGraphUse::SAMPLED:
                return image && rdesc::supportsTextureUsage(image->format, rdesc::ETextureUsage::SAMPLED);
            case EGraphUse::STORAGE_READ:
            case EGraphUse::STORAGE_WRITE:
            case EGraphUse::STORAGE_READ_WRITE:
                return !image || rdesc::supportsTextureUsage(image->format, rdesc::ETextureUsage::STORAGE);
            case EGraphUse::VERTEX:
            case EGraphUse::INDEX:
            case EGraphUse::INDIRECT:
                return !image;
            default:
                return true;
            }
        }

        // Empty means the footprint depends on the instance extent or backend-selected pitch.
        RenderResult<std::vector<GraphBufferRange>> copyBufferRanges(
            const CopyImageBuffer& copy,
            const GraphImageDesc& image
        ) noexcept
        {
            const bool source_image = std::holds_alternative<GraphImageRange>(copy.source.range);
            const auto& image_range =
                std::get<GraphImageRange>(source_image ? copy.source.range : copy.destination.range);
            const auto& capacity =
                std::get<GraphBufferRange>(source_image ? copy.destination.range : copy.source.range);
            const bool is_invalid_image = image.samples != 1 || image_range.mip_count != 1 ||
                                          image_range.layer_count != 1 || !std::has_single_bit(image_range.aspects);
            if (is_invalid_image)
            {
                return cxx::unexpected(graphError(kGraphInvalidRange));
            }
            if (image.extent.source != EImageExtentSource::FIXED && !copy.image_region)
            {
                return std::vector<GraphBufferRange>{};
            }
            const auto width =
                copy.image_region ? copy.image_region->width : std::max(1u, image.width >> image_range.first_mip);
            const auto height =
                copy.image_region ? copy.image_region->height : std::max(1u, image.height >> image_range.first_mip);
            const auto block = rdesc::isCompressedTextureFormat(image.format) ? 4u : 1u;
            const auto bytes = rdesc::textureBlockBytes(image.format, image_range.aspects);
            const auto row_bytes = ((std::uint64_t(width) + block - 1) / block) * bytes;
            const auto rows = (std::uint64_t(height) + block - 1) / block;
            if (!width || !height || !bytes)
            {
                return cxx::unexpected(graphError(kGraphInvalidRange));
            }
            if (rows > 1 && !copy.row_pitch)
            {
                return std::vector<GraphBufferRange>{};
            }
            const std::uint64_t pitch = copy.row_pitch ? copy.row_pitch : row_bytes;
            const bool is_invalid_capacity =
                pitch < row_bytes || row_bytes > capacity.size ||
                (row_bytes <= capacity.size && rows - 1 > (capacity.size - row_bytes) / pitch);
            if (is_invalid_capacity)
            {
                return cxx::unexpected(graphError(kGraphInvalidRange));
            }
            std::vector<GraphBufferRange> ranges;
            if (pitch == row_bytes)
            {
                ranges.push_back({capacity.offset, rows * row_bytes});
            }
            else
            {
                ranges.reserve(static_cast<std::size_t>(rows));
                for (std::uint64_t row = 0; row < rows; ++row)
                {
                    ranges.push_back({capacity.offset + row * pitch, row_bytes});
                }
            }
            return ranges;
        }

        RenderResult<void> validateResource(const GraphResource& resource) noexcept
        {
            const bool is_invalid_origin = resource.origin > EGraphOrigin::HISTORY_CURRENT ||
                                           !compatibleUse(resource.description, resource.initial_use);
            const bool has_illegal_initial = !resource.initial_ranges.empty() &&
                                             resource.origin != EGraphOrigin::IMPORTED &&
                                             resource.origin != EGraphOrigin::HISTORY_PREVIOUS;
            if (resource.key.empty() || is_invalid_origin || has_illegal_initial)
            {
                return cxx::unexpected(graphError(kGraphInvalidDescriptor));
            }
            if (const auto* buffer = std::get_if<GraphBufferDesc>(&resource.description))
            {
                if (buffer->size == 0)
                {
                    return cxx::unexpected(graphError(kGraphInvalidDescriptor));
                }
            }
            else
            {
                const auto& image = std::get<GraphImageDesc>(resource.description);
                const bool is_invalid_extent = image.width == 0 || image.height == 0 || image.layers == 0;
                const bool is_invalid_mips =
                    image.mips == 0 || image.mips > std::bit_width(std::max(image.width, image.height));
                const bool is_invalid_samples =
                    !std::has_single_bit(image.samples) || image.samples > 64 || (image.samples > 1 && image.mips != 1);
                const bool is_invalid_format = rdesc::textureAspectMask(image.format) == 0;
                const bool invalid_expression =
                    !image.extent.numerator || !image.extent.denominator || !image.extent.alignment ||
                    image.extent.source > EImageExtentSource::IMPORTED ||
                    (image.extent.source == EImageExtentSource::IMPORTED && resource.origin != EGraphOrigin::IMPORTED);
                if (is_invalid_extent || is_invalid_mips || is_invalid_samples || is_invalid_format ||
                    invalid_expression)
                {
                    return cxx::unexpected(graphError(kGraphInvalidDescriptor));
                }
            }
            return {};
        }

        template <typename T> RenderResult<void> uniqueKeys(std::span<const T> values, bool passes) noexcept
        {
            std::vector<std::uint32_t> order(values.size());
            std::iota(order.begin(), order.end(), 0u);
            std::sort(order.begin(), order.end(), [&](auto a, auto b) { return values[a].key < values[b].key; });
            for (std::size_t i = 1; i < order.size(); ++i)
            {
                if (values[order[i - 1]].key == values[order[i]].key)
                {
                    return cxx::unexpected(graphError(
                        kGraphDuplicateKey,
                        {passes ? order[i] : kGraphInvalidIndex},
                        {passes ? kGraphInvalidIndex : order[i]},
                        order[i - 1]
                    ));
                }
            }
            return {};
        }
    } // namespace

    RenderResult<GraphResourceId> RenderGraphBuilder::declareResource(GraphResource resource) noexcept
    {
        const GraphResourceId id{static_cast<std::uint32_t>(candidate_.resources_.size())};
        if (candidate_.resources_.size() >= kGraphInvalidIndex)
        {
            return cxx::unexpected(graphError(kGraphInvalidDescriptor));
        }
        if (auto valid = validateResource(resource); !valid)
        {
            return cxx::unexpected(graphError(valid.error().type, {}, id));
        }
        for (auto& range : resource.initial_ranges)
        {
            auto normalized = detail::normalize(resource, range);
            if (!normalized)
            {
                return cxx::unexpected(graphError(kGraphInvalidRange, {}, id));
            }
            range = *normalized;
        }
        candidate_.resources_.push_back(std::move(resource));
        versions_.push_back(0);
        return id;
    }

    RenderResult<HistoryDescription> RenderGraphBuilder::declareHistory(
        std::string key,
        VGraphResourceDesc description,
        HistoryInitialValue initial,
        EGraphUse canonical_use,
        std::uint32_t distance
    ) noexcept
    {
        const bool invalid_capacity = candidate_.resources_.size() > kGraphInvalidIndex - 2;
        const bool invalid_use = canonical_use != EGraphUse::SAMPLED && canonical_use != EGraphUse::STORAGE_READ &&
                                 canonical_use != EGraphUse::TRANSFER_SOURCE;
        if (key.empty() || distance != 1 || invalid_capacity || invalid_use ||
            !compatibleUse(description, canonical_use))
        {
            return cxx::unexpected(graphError(kGraphInvalidDescriptor));
        }
        if (const auto* buffer = std::get_if<GraphBufferDesc>(&description);
            buffer && (buffer->size % 4 || initial.buffer))
        {
            return cxx::unexpected(graphError(kGraphInvalidDescriptor));
        }
        GraphResource previous{key + ".previous", description, EGraphOrigin::HISTORY_PREVIOUS, {{}}, canonical_use};
        if (auto valid = validateResource(previous); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        const auto whole = detail::normalize(previous, {});
        previous.initial_ranges = {*whole};
        GraphResource
            current{key + ".current", std::move(description), EGraphOrigin::HISTORY_CURRENT, {}, canonical_use};
        HistoryDescription history;
        history.previous = {static_cast<std::uint32_t>(candidate_.resources_.size())};
        history.current = {history.previous.index + 1};
        history.distance = distance;
        history.initial = initial;
        history.canonical_use = canonical_use;
        candidate_.resources_.push_back(std::move(previous));
        candidate_.resources_.push_back(std::move(current));
        versions_.insert(versions_.end(), 2, 0);
        candidate_.histories_.push_back(history);
        return history;
    }

    RenderResult<void> RenderGraphBuilder::publishHistory(ResourceVersion version) noexcept
    {
        auto history = std::ranges::find(candidate_.histories_, version.resource, &HistoryDescription::current);
        if (history == candidate_.histories_.end() || history->publication || version.number == 0)
        {
            return cxx::unexpected(graphError(kGraphInvalidDescriptor, {}, version.resource));
        }
        // Analysis resolves this whole-resource observation separately from external Graph exports.
        history->publication = version;
        return {};
    }

    RenderResult<std::uint32_t> RenderGraphBuilder::declareCondition(std::string key) noexcept
    {
        if (key.empty() || candidate_.conditions_.size() >= kGraphInvalidIndex)
        {
            return cxx::unexpected(graphError(kGraphInvalidDescriptor));
        }
        if (std::ranges::find(candidate_.conditions_, key) != candidate_.conditions_.end())
        {
            return cxx::unexpected(graphError(kGraphDuplicateKey));
        }
        const auto index = static_cast<std::uint32_t>(candidate_.conditions_.size());
        candidate_.conditions_.push_back(std::move(key));
        return index;
    }

    RenderResult<std::uint32_t> RenderGraphBuilder::addProgram(rdesc::SpirvArtifact program) noexcept
    {
        if (candidate_.programs_.size() >= kGraphInvalidIndex)
        {
            return cxx::unexpected(graphError(kGraphInvalidDescriptor));
        }
        const auto index = static_cast<std::uint32_t>(candidate_.programs_.size());
        candidate_.programs_.push_back(std::move(program));
        return index;
    }

    RenderResult<PassResult> RenderGraphBuilder::addRasterPass(
        std::string key,
        RasterWork work,
        GraphPassOptions options
    ) noexcept
    {
        return addWork(std::move(key), std::move(work), EGraphPassKind::GRAPHICS, options);
    }

    RenderResult<PassResult> RenderGraphBuilder::addComputePass(
        std::string key,
        ComputeWork work,
        GraphPassOptions options
    ) noexcept
    {
        return addWork(std::move(key), std::move(work), EGraphPassKind::COMPUTE, options);
    }

    RenderResult<PassResult> RenderGraphBuilder::addTransferPass(
        std::string key,
        TransferWork work,
        GraphPassOptions options
    ) noexcept
    {
        return addWork(std::move(key), std::move(work), EGraphPassKind::TRANSFER, options);
    }

    RenderResult<PassResult> RenderGraphBuilder::addWork(
        std::string key,
        VPassWork work,
        EGraphPassKind kind,
        GraphPassOptions options
    ) noexcept
    {
        GraphPass pass{std::move(key), kind, {}, std::move(work), options.side_effect, options.condition};
        PassResult result;
        RenderError failure{};
        const auto append = [&](ResourceAccess& access, EGraphUse usage, bool record = true) noexcept
        {
            if (failure.type != 0)
            {
                return;
            }
            const auto resource = access.version.resource;
            if (resource.index >= versions_.size())
            {
                failure = graphError(kGraphUnknownResource, {}, resource);
                return;
            }
            access.usage = usage;
            access.whole_resource = std::holds_alternative<std::monostate>(access.range);
            auto range = detail::normalize(candidate_.resources_[resource.index], access.range);
            if (!range)
            {
                failure = graphError(kGraphInvalidRange, {}, resource);
                return;
            }
            access.range = *range;
            if (detail::writes(access.access))
            {
                const auto output = std::ranges::find(result.written, resource, &ResourceVersion::resource);
                access.input_version = access.version.number;
                if (output != result.written.end())
                {
                    access.version = *output;
                }
                else
                {
                    if (versions_[resource.index] == UINT32_MAX)
                    {
                        failure = graphError(kGraphInvalidDescriptor, {}, resource);
                        return;
                    }
                    access.version.number = versions_[resource.index] + 1;
                    result.written.push_back(access.version);
                }
            }
            // Multiple shader fields may read the same version and range; one dependency is sufficient.
            const bool duplicate = std::ranges::any_of(
                pass.uses,
                [&](const auto& old) noexcept
                {
                    return old.version == access.version && old.range == access.range && old.access == access.access &&
                           old.usage == access.usage && old.input_version == access.input_version;
                }
            );
            if (record && (!duplicate || detail::writes(access.access)))
            {
                pass.uses.push_back(access);
            }
        };
        const auto read = [&](ResourceVersion version, EGraphUse usage, VGraphRange range = {}) noexcept
        {
            ResourceAccess access{version, range, EGraphAccess::READ};
            append(access, usage);
        };
        const auto interface = [&](PassInterface& input) noexcept
        {
            if (input.program >= candidate_.programs_.size())
            {
                failure = graphError(kGraphInvalidDescriptor);
                return;
            }
            for (const auto& field : input.parameters.fields)
            {
                const auto index = field.resource.index;
                if (index == kGraphInvalidIndex)
                {
                    continue;
                }
                const bool valid = index < candidate_.resources_.size() &&
                                   candidate_.resources_[index].origin == EGraphOrigin::IMPORTED;
                if (!valid)
                {
                    failure = graphError(kGraphInvalidDescriptor, {}, field.resource);
                    return;
                }
            }
            std::vector<const rdesc::EDescriptorBindingInfo*> descriptors;
            for (const auto& set : candidate_.programs_[input.program].interface.sets)
            {
                for (const auto& descriptor : set.bindings)
                {
                    descriptors.push_back(&descriptor);
                }
            }
            std::vector<bool> selected(descriptors.size());
            const auto resolveField = [&](std::string_view name) -> std::uint32_t
            {
                const auto found =
                    std::ranges::find_if(descriptors, [&](const auto* field) { return field->name == name; });
                if (found == descriptors.end())
                {
                    failure = graphError(kGraphInvalidDescriptor);
                    return kGraphInvalidIndex;
                }
                const auto index = static_cast<std::uint32_t>(found - descriptors.begin());
                if (selected[index])
                {
                    failure = graphError(kGraphInvalidDescriptor);
                    return kGraphInvalidIndex;
                }
                selected[index] = true;
                return index;
            };
            for (auto& sampler : input.samplers)
            {
                sampler.field = resolveField(sampler.shader_name);
            }
            for (auto& binding : input.resources)
            {
                binding.field = resolveField(binding.shader_name);
                if (binding.field == kGraphInvalidIndex)
                {
                    return;
                }
                const auto* descriptor = descriptors[binding.field];
                const bool sampled = descriptor->type == rdesc::EDescriptorType::SAMPLED_IMAGE ||
                                     descriptor->type == rdesc::EDescriptorType::SAMPLER;
                const auto usage = sampled                                             ? EGraphUse::SAMPLED
                                   : binding.access.access == EGraphAccess::WRITE      ? EGraphUse::STORAGE_WRITE
                                   : binding.access.access == EGraphAccess::READ_WRITE ? EGraphUse::STORAGE_READ_WRITE
                                                                                       : EGraphUse::STORAGE_READ;
                append(binding.access, usage);
                for (auto& element : binding.elements)
                {
                    const auto element_usage = sampled                                 ? EGraphUse::SAMPLED
                                               : element.access == EGraphAccess::WRITE ? EGraphUse::STORAGE_WRITE
                                               : element.access == EGraphAccess::READ_WRITE
                                                   ? EGraphUse::STORAGE_READ_WRITE
                                                   : EGraphUse::STORAGE_READ;
                    append(element, element_usage);
                }
            }
        };
        std::visit(
            [&](auto& value) noexcept
            {
                using Work = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::is_same_v<Work, RasterWork>)
                {
                    interface(value.interface);
                    if (value.state_input)
                    {
                        const auto resource = value.state_input->resource;
                        const bool valid = resource.index < candidate_.resources_.size() &&
                                           candidate_.resources_[resource.index].origin == EGraphOrigin::IMPORTED;
                        if (!valid)
                        {
                            failure = graphError(kGraphInvalidDescriptor, {}, resource);
                            return;
                        }
                    }
                    for (auto& color : value.colors)
                    {
                        color.access.access =
                            color.load == EAttachmentLoad::LOAD ? EGraphAccess::ATTACHMENT_LOAD : EGraphAccess::WRITE;
                        append(color.access, EGraphUse::COLOR_ATTACHMENT);
                        if (color.resolve)
                        {
                            color.resolve->access = EGraphAccess::WRITE;
                            append(*color.resolve, EGraphUse::COLOR_ATTACHMENT);
                        }
                    }
                    if (value.depth)
                    {
                        value.depth->access.access = value.depth->load == EAttachmentLoad::LOAD
                                                         ? EGraphAccess::ATTACHMENT_LOAD
                                                         : EGraphAccess::WRITE;
                        append(value.depth->access, EGraphUse::DEPTH_ATTACHMENT);
                    }
                    for (const auto& vertex : value.vertex_buffers)
                    {
                        read(vertex.resource, EGraphUse::VERTEX);
                    }
                    std::visit(
                        [&](auto& draw) noexcept
                        {
                            using Draw = std::remove_cvref_t<decltype(draw)>;
                            if constexpr (!std::is_same_v<Draw, DrawVertices>)
                            {
                                read(draw.index_resource, EGraphUse::INDEX);
                            }
                            if constexpr (std::is_same_v<Draw, DrawIndexedIndirect>)
                            {
                                const bool is_invalid_count = draw.count == 0 || draw.stride < 20;
                                if (is_invalid_count)
                                {
                                    failure = graphError(kGraphInvalidRange, {}, draw.commands_resource.resource);
                                    return;
                                }
                                if (draw.stride == 20)
                                {
                                    read(
                                        draw.commands_resource,
                                        EGraphUse::INDIRECT,
                                        GraphBufferRange{draw.commands_offset, std::uint64_t(draw.count) * 20}
                                    );
                                }
                                else
                                {
                                    for (std::uint32_t command = 0; command < draw.count; ++command)
                                    {
                                        read(
                                            draw.commands_resource,
                                            EGraphUse::INDIRECT,
                                            GraphBufferRange{
                                                draw.commands_offset + std::uint64_t(command) * draw.stride,
                                                20
                                            }
                                        );
                                    }
                                }
                                if (draw.count_resource)
                                {
                                    read(
                                        *draw.count_resource,
                                        EGraphUse::INDIRECT,
                                        GraphBufferRange{draw.count_offset, 4}
                                    );
                                }
                            }
                        },
                        value.draw
                    );
                }
                else if constexpr (std::is_same_v<Work, ComputeWork>)
                {
                    interface(value.interface);
                    if (const auto* indirect = std::get_if<IndirectDispatchArguments>(&value.dispatch))
                    {
                        read(indirect->buffer, EGraphUse::INDIRECT, GraphBufferRange{indirect->offset, 12});
                    }
                }
                else
                {
                    for (auto& operation : value.operations)
                    {
                        std::visit(
                            [&](auto& copy) noexcept
                            {
                                using Operation = std::remove_cvref_t<decltype(copy)>;
                                if constexpr (std::is_same_v<Operation, CopyImageBuffer>)
                                {
                                    const bool preserve = copy.destination.access == EGraphAccess::READ_WRITE;
                                    copy.source.access = EGraphAccess::READ;
                                    copy.destination.access = preserve ? EGraphAccess::READ_WRITE : EGraphAccess::WRITE;
                                    append(copy.source, EGraphUse::TRANSFER_SOURCE, false);
                                    append(copy.destination, EGraphUse::TRANSFER_DESTINATION, false);
                                    if (failure.type != 0)
                                    {
                                        return;
                                    }
                                    const bool source_image =
                                        std::holds_alternative<GraphImageRange>(copy.source.range);
                                    const bool destination_image =
                                        std::holds_alternative<GraphImageRange>(copy.destination.range);
                                    if (source_image == destination_image)
                                    {
                                        failure = graphError(kGraphInvalidDescriptor);
                                        return;
                                    }
                                    auto image_access = source_image ? copy.source : copy.destination;
                                    if (!source_image && copy.image_region)
                                    {
                                        image_access.access = EGraphAccess::READ_WRITE;
                                    }
                                    const auto& image = std::get<GraphImageDesc>(
                                        candidate_.resources_[image_access.version.resource.index].description
                                    );
                                    auto ranges = copyBufferRanges(copy, image);
                                    if (!ranges)
                                    {
                                        failure = ranges.error();
                                        return;
                                    }
                                    auto buffer_access = source_image ? copy.destination : copy.source;
                                    if (source_image && ranges->empty() && !preserve)
                                    {
                                        failure =
                                            graphError(kGraphInvalidDescriptor, {}, buffer_access.version.resource);
                                        return;
                                    }
                                    pass.uses.push_back(image_access);
                                    if (ranges->empty() || (source_image && preserve))
                                    {
                                        pass.uses.push_back(buffer_access);
                                    }
                                    else
                                    {
                                        buffer_access.whole_resource = false;
                                        for (const auto& range : *ranges)
                                        {
                                            buffer_access.range = range;
                                            pass.uses.push_back(buffer_access);
                                        }
                                    }
                                    return;
                                }
                                if constexpr (requires { copy.source; })
                                {
                                    copy.source.access = EGraphAccess::READ;
                                    append(copy.source, EGraphUse::TRANSFER_SOURCE);
                                }
                                copy.destination.access = EGraphAccess::WRITE;
                                if constexpr (std::is_same_v<std::remove_cvref_t<decltype(copy)>, CopyImage>)
                                {
                                    if (copy.destination_region)
                                    {
                                        copy.destination.access = EGraphAccess::READ_WRITE;
                                    }
                                }
                                append(copy.destination, EGraphUse::TRANSFER_DESTINATION);
                                if (failure.type != 0)
                                {
                                    return;
                                }
                                const bool destination_image =
                                    std::holds_alternative<GraphImageRange>(copy.destination.range);
                                const bool matches_operation = [&]
                                {
                                    if constexpr (std::is_same_v<Operation, ClearImage>)
                                    {
                                        return destination_image;
                                    }
                                    else if constexpr (std::is_same_v<Operation, FillBuffer>)
                                    {
                                        return !destination_image;
                                    }
                                    else
                                    {
                                        const bool source_image =
                                            std::holds_alternative<GraphImageRange>(copy.source.range);
                                        if constexpr (std::is_same_v<Operation, CopyImage>)
                                        {
                                            return source_image && destination_image;
                                        }
                                        else if constexpr (std::is_same_v<Operation, CopyBuffer>)
                                        {
                                            return !source_image && !destination_image;
                                        }
                                        else
                                        {
                                            return source_image != destination_image;
                                        }
                                    }
                                }();
                                if (!matches_operation)
                                {
                                    failure =
                                        graphError(kGraphInvalidDescriptor, {}, copy.destination.version.resource);
                                }
                            },
                            operation
                        );
                    }
                }
            },
            pass.work
        );
        if (failure.type != 0)
        {
            return cxx::unexpected(failure);
        }
        auto added = addPass(std::move(pass));
        if (!added)
        {
            return cxx::unexpected(added.error());
        }
        result.pass = *added;
        for (const auto version : result.written)
        {
            versions_[version.resource.index] = version.number;
        }
        return result;
    }

    RenderResult<GraphPassId> RenderGraphBuilder::addPass(GraphPass pass) noexcept
    {
        const GraphPassId id{static_cast<std::uint32_t>(candidate_.passes_.size())};
        const bool is_invalid_pass =
            candidate_.passes_.size() >= kGraphInvalidIndex || pass.key.empty() || pass.kind > EGraphPassKind::TRANSFER;
        if (is_invalid_pass)
        {
            return cxx::unexpected(graphError(kGraphInvalidDescriptor, id));
        }
        if (pass.condition)
        {
            const auto& condition = *pass.condition;
            const bool valid_input =
                condition.source == EConditionSource::INPUT && condition.input < candidate_.conditions_.size();
            const bool valid_history =
                condition.source == EConditionSource::HISTORY_VALID &&
                std::ranges::any_of(
                    candidate_.histories_,
                    [&](const auto& history) { return history.previous.index == condition.input; }
                );
            if (!valid_input && !valid_history)
            {
                return cxx::unexpected(graphError(kGraphInvalidDescriptor, id));
            }
        }
        for (auto& use : pass.uses)
        {
            const auto resource_id = use.version.resource;
            // Typed ingestion already resolved the resource and normalized its range.
            const auto& resource = candidate_.resources_[resource_id.index];
            const bool is_write = detail::writes(use.access);
            const bool is_invalid_access =
                use.access > EGraphAccess::ATTACHMENT_LOAD ||
                (is_write && (use.version.number == 0 || resource.origin == EGraphOrigin::HISTORY_PREVIOUS));
            const bool is_invalid_predecessor =
                is_write && detail::reads(use.access) && use.input_version >= use.version.number;
            const auto* image = std::get_if<GraphImageDesc>(&resource.description);
            const bool is_attachment_format =
                image && (rdesc::supportsTextureUsage(image->format, rdesc::ETextureUsage::COLOR_ATTACHMENT) ||
                          rdesc::supportsTextureUsage(image->format, rdesc::ETextureUsage::DEPTH_STENCIL_ATTACHMENT));
            const bool is_invalid_load = use.access == EGraphAccess::ATTACHMENT_LOAD &&
                                         (pass.kind != EGraphPassKind::GRAPHICS || !is_attachment_format);
            if (is_invalid_access || is_invalid_predecessor || is_invalid_load)
            {
                return cxx::unexpected(graphError(kGraphInvalidDescriptor, id, resource_id));
            }
            // BOUNDARY: validate the orthogonal execution role here, once at authoring.
            const bool is_invalid_color =
                use.usage == EGraphUse::COLOR_ATTACHMENT &&
                (!image || pass.kind != EGraphPassKind::GRAPHICS ||
                 !rdesc::supportsTextureUsage(image->format, rdesc::ETextureUsage::COLOR_ATTACHMENT) ||
                 (use.access != EGraphAccess::WRITE && use.access != EGraphAccess::ATTACHMENT_LOAD));
            const bool is_invalid_source = use.usage == EGraphUse::TRANSFER_SOURCE &&
                                           (pass.kind != EGraphPassKind::TRANSFER || use.access != EGraphAccess::READ);
            const bool is_invalid_destination =
                use.usage == EGraphUse::TRANSFER_DESTINATION &&
                (pass.kind != EGraphPassKind::TRANSFER ||
                 (use.access != EGraphAccess::WRITE && use.access != EGraphAccess::READ_WRITE));
            const bool is_invalid_sample =
                use.usage == EGraphUse::SAMPLED &&
                (!image || pass.kind == EGraphPassKind::TRANSFER || use.access != EGraphAccess::READ ||
                 !rdesc::supportsTextureUsage(image->format, rdesc::ETextureUsage::SAMPLED));
            const bool is_storage = use.usage == EGraphUse::STORAGE_READ || use.usage == EGraphUse::STORAGE_WRITE ||
                                    use.usage == EGraphUse::STORAGE_READ_WRITE;
            const bool invalid_storage =
                is_storage && (pass.kind == EGraphPassKind::TRANSFER ||
                               (use.usage == EGraphUse::STORAGE_WRITE && use.access != EGraphAccess::WRITE) ||
                               (use.usage == EGraphUse::STORAGE_READ_WRITE && use.access != EGraphAccess::READ_WRITE));
            const bool geometry =
                use.usage == EGraphUse::VERTEX || use.usage == EGraphUse::INDEX || use.usage == EGraphUse::INDIRECT;
            const bool invalid_geometry =
                geometry && (image || pass.kind == EGraphPassKind::TRANSFER || use.access != EGraphAccess::READ ||
                             (pass.kind == EGraphPassKind::COMPUTE && use.usage != EGraphUse::INDIRECT));
            const bool invalid_depth =
                use.usage == EGraphUse::DEPTH_ATTACHMENT &&
                (!image || pass.kind != EGraphPassKind::GRAPHICS ||
                 !rdesc::supportsTextureUsage(image->format, rdesc::ETextureUsage::DEPTH_STENCIL_ATTACHMENT) ||
                 (use.access != EGraphAccess::WRITE && use.access != EGraphAccess::ATTACHMENT_LOAD));
            const bool is_invalid_usage =
                use.usage > EGraphUse::DEPTH_ATTACHMENT || invalid_storage || invalid_geometry || invalid_depth ||
                (use.usage == EGraphUse::STORAGE_READ && use.access != EGraphAccess::READ) || is_invalid_color ||
                is_invalid_source || is_invalid_destination || is_invalid_sample;
            if (is_invalid_usage)
            {
                return cxx::unexpected(graphError(kGraphInvalidDescriptor, id, resource_id));
            }
            if (use.fallback)
            {
                const auto source = use.fallback->resource;
                if (source.index >= candidate_.resources_.size())
                {
                    return cxx::unexpected(graphError(kGraphUnknownResource, id, source));
                }
                const bool is_compatible = candidate_.resources_[source.index].description == resource.description;
                const bool is_write_fallback =
                    is_write && pass.condition && source == resource_id && use.fallback->number < use.version.number;
                const bool is_history_fallback = !is_write && resource.origin == EGraphOrigin::HISTORY_PREVIOUS &&
                                                 use.version.number == 0 && source != resource_id;
                if (!is_compatible || (!is_write_fallback && !is_history_fallback))
                {
                    return cxx::unexpected(graphError(kGraphInvalidDescriptor, id, resource_id));
                }
            }
        }
        // One pass must express an overlapping in-place update as READ_WRITE/LOAD,
        // not two independent accesses with unprovable internal ordering.
        for (std::size_t i = 0; i < pass.uses.size(); ++i)
        {
            for (std::size_t j = 0; j < i; ++j)
            {
                const auto& a = pass.uses[i];
                const auto& b = pass.uses[j];
                const bool has_write = detail::writes(a.access) || detail::writes(b.access);
                const bool fallback_conflict =
                    (a.access == EGraphAccess::READ && a.fallback && detail::writes(b.access) &&
                     a.fallback->resource == b.version.resource) ||
                    (b.access == EGraphAccess::READ && b.fallback && detail::writes(a.access) &&
                     b.fallback->resource == a.version.resource);
                const bool is_conflict =
                    ((a.version.resource == b.version.resource && has_write) || fallback_conflict) &&
                    detail::overlaps(a.range, b.range);
                if (is_conflict)
                {
                    return cxx::unexpected(graphError(kGraphConflictingOutput, id, a.version.resource, j));
                }
            }
        }
        candidate_.passes_.push_back(std::move(pass));
        return id;
    }

    RenderResult<void> RenderGraphBuilder::dependsOn(GraphPassId before, GraphPassId after) noexcept
    {
        const bool is_unknown = before.index >= candidate_.passes_.size() || after.index >= candidate_.passes_.size();
        if (is_unknown)
        {
            return cxx::unexpected(graphError(kGraphUnknownPass, before, {}, after.index));
        }
        if (before == after)
        {
            return cxx::unexpected(graphError(kGraphDependencyCycle, before));
        }
        const GraphDependency edge{before, after};
        auto& edges = candidate_.dependencies_;
        if (std::find(edges.begin(), edges.end(), edge) == edges.end())
        {
            edges.push_back(edge);
        }
        return {};
    }

    RenderResult<void> RenderGraphBuilder::exportOutput(GraphExport output) noexcept
    {
        const auto id = output.version.resource;
        if (output.final_use &&
            (*output.final_use == EGraphUse::UNSPECIFIED || *output.final_use > EGraphUse::DEPTH_ATTACHMENT))
        {
            return cxx::unexpected(graphError(kGraphInvalidDescriptor, {}, id));
        }
        if (id.index >= candidate_.resources_.size())
        {
            return cxx::unexpected(graphError(kGraphUnknownResource, {}, id));
        }
        const auto& resource = candidate_.resources_[id.index];
        if (output.final_use && !compatibleUse(resource.description, *output.final_use))
        {
            return cxx::unexpected(graphError(kGraphInvalidDescriptor, {}, id));
        }
        auto range = detail::normalize(resource, output.range);
        if (!range)
        {
            return cxx::unexpected(graphError(kGraphInvalidRange, {}, id));
        }
        output.range = *range;
        if (output.target)
        {
            const auto* image = std::get_if<GraphImageDesc>(&resource.description);
            const auto* part = std::get_if<GraphImageRange>(&output.range);
            const auto& target = *output.target;
            const bool is_relative = target.width == 0 && target.height == 0;
            const bool is_compatible =
                image && part && (is_relative || (image->width == target.width && image->height == target.height)) &&
                image->format == target.format && image->samples == target.samples && part->aspects == 1 &&
                part->first_mip == 0 && part->mip_count == 1 && part->first_layer == 0 && part->layer_count == 1 &&
                image->mips == 1 && image->layers == 1;
            if (!is_compatible)
            {
                return cxx::unexpected(graphError(kGraphConflictingOutput, {}, id));
            }
        }
        for (const auto& existing : candidate_.outputs_)
        {
            const bool is_conflict = existing.version.resource == id && detail::overlaps(existing.range, output.range);
            if (is_conflict)
            {
                return cxx::unexpected(graphError(kGraphConflictingOutput, {}, id));
            }
        }
        candidate_.outputs_.push_back(std::move(output));
        return {};
    }

    RenderResult<RenderGraph> RenderGraphBuilder::finish() && noexcept
    {
        if (auto valid = uniqueKeys(candidate_.resources(), false); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        if (auto valid = uniqueKeys(candidate_.passes(), true); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        auto result = std::move(candidate_);
        candidate_ = {};
        versions_.clear();
        return result;
    }

    std::span<const error::ErrorDescriptor> renderGraphErrorDescriptors() noexcept
    {
        using error::EArgument;
        static constexpr auto args = std::array{EArgument::UNSIGNED, EArgument::UNSIGNED, EArgument::UNSIGNED};
        static constexpr error::ErrorDescriptor descriptors[]{
            {"lux.render.graph.unknown_resource",
             "Unknown definition-local resource (details {0}, {1}, {2})",
             error::ERecovery::PERMANENT,
             args},
            {"lux.render.graph.unknown_pass",
             "Unknown definition-local pass (details {0}, {1}, {2})",
             error::ERecovery::PERMANENT,
             args},
            {"lux.render.graph.invalid_descriptor",
             "Invalid graph declaration (details {0}, {1}, {2})",
             error::ERecovery::PERMANENT,
             args},
            {"lux.render.graph.invalid_range",
             "Invalid resource range (details {0}, {1}, {2})",
             error::ERecovery::PERMANENT,
             args},
            {"lux.render.graph.duplicate_key",
             "Duplicate complete key (details {0}, {1}, {2})",
             error::ERecovery::PERMANENT,
             args},
            {"lux.render.graph.conflicting_output",
             "Conflicting output (details {0}, {1}, {2})",
             error::ERecovery::PERMANENT,
             args},
            {"lux.render.graph.undefined_read",
             "Version range has no producer (details {0}, {1}, {2})",
             error::ERecovery::PERMANENT,
             args},
            {"lux.render.graph.ambiguous_producer",
             "Overlapping version producers (details {0}, {1}, {2})",
             error::ERecovery::PERMANENT,
             args},
            {"lux.render.graph.dependency_cycle",
             "Dependency cycle (details {0}, {1}, {2})",
             error::ERecovery::PERMANENT,
             args},
            {"lux.render.graph.missing_fallback",
             "Conditional fallback missing (details {0}, {1}, {2})",
             error::ERecovery::PERMANENT,
             args},
            {"lux.render.graph.missing_initial",
             "Initial range unavailable (details {0}, {1}, {2})",
             error::ERecovery::PERMANENT,
             args}
        };
        return descriptors;
    }
} // namespace lux::render
