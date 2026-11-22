#include <lux/engine/render/extensions/graph/RenderGraphBuilder.hpp>
#include <lux/engine/render/graph/Range.hpp>
#include <bit>

namespace lux::render
{
    using detail::graphError;

    namespace
    {
        struct NormalizedPassDraft final
        {
            GraphPass pass;
            std::vector<ResourceVersion> written;
            bool selected_value = false;
        };

        // Empty means the footprint depends on the instance extent or backend-selected pitch.
        RenderResult<std::vector<GraphBufferRange>>
        copyBufferRanges(const CopyImageBuffer& copy, const GraphImageDesc& image) noexcept
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

        RenderResult<void>
        validateAccess(const RenderGraph& graph, const GraphPass& pass, GraphPassId id, const ResourceAccess& use) noexcept
        {
            const auto resource_id = use.version.resource;
            const auto kind = pass.kind();
            // Typed ingestion already resolved the resource and normalized its range.
            const auto& resource = graph.resources()[resource_id.index];
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
                                         (kind != EGraphPassKind::GRAPHICS || !is_attachment_format);
            if (is_invalid_access || is_invalid_predecessor || is_invalid_load)
            {
                return cxx::unexpected(graphError(kGraphInvalidDescriptor, id, resource_id));
            }
            // BOUNDARY: validate the orthogonal execution role here, once at authoring.
            const bool is_invalid_color =
                use.usage == EGraphUse::COLOR_ATTACHMENT &&
                (!image || kind != EGraphPassKind::GRAPHICS ||
                 !rdesc::supportsTextureUsage(image->format, rdesc::ETextureUsage::COLOR_ATTACHMENT) ||
                 (use.access != EGraphAccess::WRITE && use.access != EGraphAccess::ATTACHMENT_LOAD));
            const bool is_invalid_source = use.usage == EGraphUse::TRANSFER_SOURCE &&
                                           (kind != EGraphPassKind::TRANSFER || use.access != EGraphAccess::READ);
            const bool is_invalid_destination =
                use.usage == EGraphUse::TRANSFER_DESTINATION &&
                (kind != EGraphPassKind::TRANSFER ||
                 (use.access != EGraphAccess::WRITE && use.access != EGraphAccess::READ_WRITE));
            const bool is_invalid_sample =
                use.usage == EGraphUse::SAMPLED &&
                (!image || kind == EGraphPassKind::TRANSFER || use.access != EGraphAccess::READ ||
                 !rdesc::supportsTextureUsage(image->format, rdesc::ETextureUsage::SAMPLED));
            const bool is_storage = use.usage == EGraphUse::STORAGE_READ || use.usage == EGraphUse::STORAGE_WRITE ||
                                    use.usage == EGraphUse::STORAGE_READ_WRITE;
            const bool invalid_storage =
                is_storage && (kind == EGraphPassKind::TRANSFER ||
                               (use.usage == EGraphUse::STORAGE_WRITE && use.access != EGraphAccess::WRITE) ||
                               (use.usage == EGraphUse::STORAGE_READ_WRITE && use.access != EGraphAccess::READ_WRITE));
            const bool geometry =
                use.usage == EGraphUse::VERTEX || use.usage == EGraphUse::INDEX || use.usage == EGraphUse::INDIRECT;
            const bool invalid_geometry =
                geometry && (image || kind == EGraphPassKind::TRANSFER || use.access != EGraphAccess::READ ||
                             (kind == EGraphPassKind::COMPUTE && use.usage != EGraphUse::INDIRECT));
            const bool invalid_depth =
                use.usage == EGraphUse::DEPTH_ATTACHMENT &&
                (!image || kind != EGraphPassKind::GRAPHICS ||
                 !rdesc::supportsTextureUsage(image->format, rdesc::ETextureUsage::DEPTH_STENCIL_ATTACHMENT) ||
                 (use.access != EGraphAccess::WRITE && use.access != EGraphAccess::ATTACHMENT_LOAD));
            const bool invalid_uniform = use.usage == EGraphUse::UNIFORM_READ &&
                (image || kind == EGraphPassKind::TRANSFER || use.access != EGraphAccess::READ);
            const bool is_invalid_usage =
                use.usage > EGraphUse::UNIFORM_READ || invalid_storage || invalid_geometry || invalid_depth ||
                invalid_uniform ||
                (use.usage == EGraphUse::STORAGE_READ && use.access != EGraphAccess::READ) || is_invalid_color ||
                is_invalid_source || is_invalid_destination || is_invalid_sample;
            if (is_invalid_usage)
            {
                return cxx::unexpected(graphError(kGraphInvalidDescriptor, id, resource_id));
            }
            if (use.fallback)
            {
                const auto source = use.fallback->resource;
                if (source.index >= graph.resources().size())
                {
                    return cxx::unexpected(graphError(kGraphUnknownResource, id, source));
                }
                const bool is_compatible = graph.resources()[source.index].description == resource.description;
                const bool is_write_fallback =
                    is_write && pass.condition && source == resource_id && use.fallback->number < use.version.number;
                const bool is_history_fallback = !is_write && resource.origin == EGraphOrigin::HISTORY_PREVIOUS &&
                                                 use.version.number == 0 && source != resource_id;
                if (!is_compatible || (!is_write_fallback && !is_history_fallback))
                {
                    return cxx::unexpected(graphError(kGraphInvalidDescriptor, id, resource_id));
                }
            }
            return {};
        }

        RenderResult<void> validatePass(const RenderGraph& graph, const GraphPass& pass, GraphPassId id) noexcept
        {
            const bool is_invalid_pass =
                id.index >= kGraphInvalidIndex || pass.key.empty();
            if (is_invalid_pass)
            {
                return cxx::unexpected(graphError(kGraphInvalidDescriptor, id));
            }
            if (pass.condition && !graph.validCondition(*pass.condition))
                return cxx::unexpected(graphError(kGraphInvalidDescriptor, id));
            for (const auto& use : pass.uses)
            {
                if (auto valid = validateAccess(graph, pass, id, use); !valid) return valid;
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
            return {};
        }
    }

    // All access, version, and field claims belong to this candidate until validation succeeds.
    class RenderGraphBuilder::PassNormalizer final
    {
    public:
        PassNormalizer(
            const RenderGraphBuilder& builder,
            std::span<const std::uint32_t> versions,
            GraphPassId id
        ) noexcept
            : builder_(builder), versions_(versions), id_(id)
        {
        }

        template <class Work>
        RenderResult<NormalizedPassDraft> build(std::string key, Work work, GraphPassOptions options) noexcept
        {
            auto& pass = draft_.pass;
            pass.key = std::move(key);
            pass.side_effect = options.side_effect;
            if (builder_.branch_ && options.condition && options.condition != builder_.branch_)
                return cxx::unexpected(graphError(kGraphInvalidDescriptor, id_));
            pass.condition = options.condition ? options.condition : builder_.branch_;
            pass.estimated_gpu_ns = options.estimated_gpu_ns;
            auto& command = [&]() -> auto&
            {
                if constexpr (std::same_as<Work, TransferWork>)
                    return pass.command.emplace<GraphTransferCommand>(std::move(work));
                else
                    return pass.command.emplace<Work>(std::move(work));
            }();
            if (auto result = normalize(command); !result) return cxx::unexpected(result.error());
            auto valid = validatePass(builder_.candidate_, pass, id_);
            if (!valid) return cxx::unexpected(valid.error());
            return std::move(draft_);
        }

    private:
        [[nodiscard]] RenderResult<void> select(ResourceVersion& value) noexcept
        {
            if (value.selection == kGraphInvalidIndex) return {};
            if (value.selection >= builder_.choices_.size())
                return cxx::unexpected(graphError(kGraphUnknownResource, id_, value.resource));
            const auto& choice = builder_.choices_[value.selection];
            if (value.resource != choice.first.resource || value.number != choice.first.number)
                return cxx::unexpected(graphError(kGraphInvalidDescriptor, id_, value.resource));
            auto& guard = draft_.pass.condition;
            if (!guard) guard = choice.predicate;
            if (guard->source != choice.predicate.source || guard->input != choice.predicate.input)
                return cxx::unexpected(graphError(kGraphInvalidDescriptor, id_, value.resource));
            value = guard->expected == choice.predicate.expected ? choice.first : choice.second;
            draft_.selected_value = true;
            return {};
        }

        [[nodiscard]] RenderResult<void> resolve(ResourceAccess& access, EGraphUse usage) noexcept
        {
            if (access.version.selection != kGraphInvalidIndex && detail::writes(access.access))
                return cxx::unexpected(graphError(kGraphInvalidDescriptor, id_, access.version.resource));
            if (auto selected = select(access.version); !selected) return selected;
            if (access.fallback)
            {
                if (auto selected = select(*access.fallback); !selected) return selected;
            }
            const auto resource = access.version.resource;
            if (resource.index >= versions_.size())
                return cxx::unexpected(graphError(kGraphUnknownResource, {}, resource));
            access.usage = usage;
            access.whole_resource = std::holds_alternative<std::monostate>(access.range);
            const auto range = detail::normalize(builder_.candidate_.resources()[resource.index], access.range);
            if (!range) return cxx::unexpected(graphError(kGraphInvalidRange, {}, resource));
            access.range = *range;
            if (!detail::writes(access.access)) return {};

            access.input_version = access.version.number;
            const auto written = std::ranges::find(draft_.written, resource, &ResourceVersion::resource);
            if (written != draft_.written.end()) access.version = *written;
            else
            {
                if (versions_[resource.index] == UINT32_MAX)
                    return cxx::unexpected(graphError(kGraphInvalidDescriptor, {}, resource));
                access.version.number = versions_[resource.index] + 1;
                draft_.written.push_back(access.version);
            }
            return {};
        }

        [[nodiscard]] RenderResult<void> append(ResourceAccess& access, EGraphUse usage) noexcept
        {
            if (auto result = resolve(access, usage); !result) return result;
            const auto duplicate = [&](const ResourceAccess& old) noexcept
            {
                return old.version == access.version && old.range == access.range && old.access == access.access &&
                    old.usage == access.usage && old.input_version == access.input_version &&
                    old.fallback == access.fallback && old.whole_resource == access.whole_resource;
            };
            auto& uses = draft_.pass.uses;
            if (detail::writes(access.access) || !std::ranges::any_of(uses, duplicate)) uses.push_back(access);
            return {};
        }

        [[nodiscard]] RenderResult<void>
        read(ResourceVersion& version, EGraphUse usage, VGraphRange range = {}) noexcept
        {
            ResourceAccess access{version, range, EGraphAccess::READ};
            auto result = append(access, usage);
            if (result) version = access.version;
            return result;
        }

        [[nodiscard]] RenderResult<void> bindInterface(PassInterface& input) noexcept
        {
            const auto programs = builder_.candidate_.programs();
            if (input.program >= programs.size())
                return cxx::unexpected(graphError(kGraphInvalidDescriptor));
            const auto& sets = programs[input.program].targets.front().interface.sets;
            const auto& fields = builder_.program_fields_[input.program];
            std::vector<bool> claimed(fields.size());
            const auto claim = [&](std::string_view name) noexcept -> const ProgramField*
            {
                const auto fieldName = [&](const ProgramField& field) noexcept -> std::string_view
                {
                    return sets[field.set].bindings[field.binding].name;
                };
                const auto field = std::ranges::lower_bound(fields, name, {}, fieldName);
                if (field == fields.end() || fieldName(*field) != name || claimed[field->ordinal]) return nullptr;
                claimed[field->ordinal] = true;
                return &*field;
            };
            for (auto& sampler : input.samplers)
            {
                const auto* field = claim(sampler.shader_name);
                if (!field) return cxx::unexpected(graphError(kGraphInvalidDescriptor));
                sampler.field = field->ordinal;
            }
            for (auto& binding : input.resources)
            {
                const auto* field = claim(binding.shader_name);
                if (!field) return cxx::unexpected(graphError(kGraphInvalidDescriptor));
                binding.field = field->ordinal;
                const auto type = sets[field->set].bindings[field->binding].type;
                const bool sampled = type == rdesc::EDescriptorType::SAMPLED_IMAGE ||
                    type == rdesc::EDescriptorType::SAMPLER;
                const auto bind = [&](ResourceAccess& access) noexcept
                {
                    if (sampled) return append(access, EGraphUse::SAMPLED);
                    if (access.access == EGraphAccess::WRITE) return append(access, EGraphUse::STORAGE_WRITE);
                    if (access.access == EGraphAccess::READ_WRITE)
                        return append(access, EGraphUse::STORAGE_READ_WRITE);
                    return append(access, EGraphUse::STORAGE_READ);
                };
                if (auto result = bind(binding.access); !result) return result;
                for (auto& element : binding.elements)
                    if (auto result = bind(element); !result) return result;
            }
            return {};
        }

        [[nodiscard]] RenderResult<void> bindDynamicRaster(RasterPassTemplate& dynamic, bool has_depth) noexcept
        {
            const bool no_bias_source = !dynamic.usesStreamProducer() || !has_depth;
            const bool invalid_bias = dynamic.section_depth_bias && no_bias_source;
            const bool mixed_program_sources = dynamic.runtime_stream_programs && !dynamic.stream_programs.empty();
            const bool external_owners = dynamic.shared_textures || dynamic.mesh_geometry ||
                dynamic.shared_materials;
            const bool invalid_owners = external_owners && !dynamic.usesStreamProducer();
            if (dynamic.purpose.empty() || invalid_bias || mixed_program_sources || invalid_owners)
            {
                return cxx::unexpected(graphError(kGraphInvalidDescriptor));
            }
            for (const auto& program : dynamic.stream_programs)
            {
                const bool unknown = program.program >= builder_.candidate_.programs().size();
                const bool duplicate =
                    std::ranges::count(dynamic.stream_programs, program.program,
                        &RasterPassTemplate::StreamProgram::program) != 1;
                if (unknown || duplicate)
                {
                    return cxx::unexpected(graphError(kGraphInvalidDescriptor));
                }
            }
            for (auto sampler : dynamic.stream_samplers)
            {
                const bool unknown = sampler >= builder_.samplerCount();
                const bool duplicate = std::ranges::count(dynamic.stream_samplers, sampler) != 1;
                if (unknown || duplicate || !dynamic.usesStreamProducer())
                {
                    return cxx::unexpected(graphError(kGraphInvalidDescriptor));
                }
            }
            for (const auto& input : dynamic.inputs)
            {
                const bool is_read = input.usage == EGraphUse::SAMPLED ||
                                     input.usage == EGraphUse::STORAGE_READ ||
                                     input.usage == EGraphUse::VERTEX || input.usage == EGraphUse::INDEX ||
                                     input.usage == EGraphUse::INDIRECT;
                const bool is_shader_input =
                    input.usage == EGraphUse::SAMPLED || input.usage == EGraphUse::STORAGE_READ;
                const bool is_invalid_stage =
                    is_shader_input ? input.stages == 0 || (input.stages & ~3u) != 0 : input.stages != 0;
                const bool is_invalid_kind = !is_shader_input && input.kind.has_value();
                const bool is_invalid_comparison = input.comparison_sampler &&
                    input.kind != rdesc::EDescriptorType::SAMPLER;
                const bool is_image_or_sampler = input.kind == rdesc::EDescriptorType::SAMPLED_IMAGE ||
                    input.kind == rdesc::EDescriptorType::SAMPLER;
                const auto descriptor_usage =
                    is_image_or_sampler ? EGraphUse::SAMPLED : EGraphUse::STORAGE_READ;
                const bool is_wrong_stream_use = dynamic.usesStreamProducer() && input.kind &&
                    input.usage != descriptor_usage;
                const bool duplicate =
                    std::ranges::count(dynamic.inputs, input.name, &RasterPassTemplate::Input::name) != 1;
                const bool is_invalid_slot = input.name.empty() || !is_read || is_invalid_stage ||
                    is_invalid_kind || is_invalid_comparison || is_wrong_stream_use || duplicate;
                if (is_invalid_slot)
                {
                    return cxx::unexpected(graphError(kGraphInvalidDescriptor));
                }
            }
            for (auto& resource : dynamic.resources)
            {
                if (resource.access != EGraphAccess::READ)
                    return cxx::unexpected(graphError(kGraphInvalidDescriptor, {}, resource.version.resource));
                if (auto result = append(resource, resource.usage); !result) return result;
            }
            return {};
        }

        [[nodiscard]] RenderResult<void> attachments(RasterScope& scope) noexcept
        {
            const auto attach = [&](RasterAttachment& target, EGraphUse usage) noexcept
            {
                target.access.access = target.load == EAttachmentLoad::LOAD ?
                    EGraphAccess::ATTACHMENT_LOAD : EGraphAccess::WRITE;
                return append(target.access, usage);
            };
            for (auto& color : scope.colors)
            {
                if (auto result = attach(color, EGraphUse::COLOR_ATTACHMENT); !result) return result;
                if (color.resolve)
                {
                    color.resolve->access = EGraphAccess::WRITE;
                    if (auto result = append(*color.resolve, EGraphUse::COLOR_ATTACHMENT); !result) return result;
                }
            }
            if (scope.depth) return attach(*scope.depth, EGraphUse::DEPTH_ATTACHMENT);
            return {};
        }

        [[nodiscard]] RenderResult<void> bindGeometry(RasterWork::FixedProgram& fixed) noexcept
        {
            for (auto& vertex : fixed.vertex_buffers)
                if (auto result = read(vertex.resource, EGraphUse::VERTEX); !result) return result;
            if (auto* indexed = std::get_if<DrawIndexed>(&fixed.draw))
                return read(indexed->index_resource, EGraphUse::INDEX);
            auto* indirect = std::get_if<DrawIndexedIndirect>(&fixed.draw);
            if (!indirect) return {};
            if (auto result = read(indirect->index_resource, EGraphUse::INDEX); !result) return result;
            constexpr std::uint64_t command_bytes = 20;
            if (!indirect->count || indirect->stride < command_bytes)
                return cxx::unexpected(graphError(kGraphInvalidRange, {}, indirect->commands_resource.resource));
            if (indirect->stride == command_bytes)
            {
                auto result = read(indirect->commands_resource, EGraphUse::INDIRECT,
                    GraphBufferRange{indirect->commands_offset, command_bytes * indirect->count}
                );
                if (!result) return result;
            }
            else
            {
                for (std::uint32_t i = 0; i < indirect->count; ++i)
                {
                    const auto relative = std::uint64_t(i) * indirect->stride;
                    if (relative > UINT64_MAX - indirect->commands_offset)
                        return cxx::unexpected(graphError(kGraphInvalidRange, {}, indirect->commands_resource.resource));
                    auto result = read(indirect->commands_resource, EGraphUse::INDIRECT,
                        GraphBufferRange{indirect->commands_offset + relative, command_bytes}
                    );
                    if (!result) return result;
                }
            }
            if (indirect->count_resource)
                return read(*indirect->count_resource, EGraphUse::INDIRECT,
                    GraphBufferRange{indirect->count_offset, 4}
                );
            return {};
        }

        [[nodiscard]] RenderResult<void> normalize(RasterWork& work) noexcept
        {
            if (!work.hasAttachments()) return cxx::unexpected(graphError(kGraphInvalidDescriptor));
            auto* fixed = std::get_if<RasterWork::FixedProgram>(&work.program);
            if (fixed)
            {
                if (auto result = bindInterface(fixed->interface); !result) return result;
            }
            else
            {
                auto& dynamic = std::get<RasterPassTemplate>(work.program);
                if (auto result = bindDynamicRaster(dynamic, work.depth.has_value()); !result) return result;
            }
            if (auto result = attachments(work); !result) return result;
            if (fixed) return bindGeometry(*fixed);
            return {};
        }

        [[nodiscard]] RenderResult<void> normalize(ComputeWork& work) noexcept
        {
            if (auto result = bindInterface(work.interface); !result) return result;
            if (auto* indirect = std::get_if<IndirectDispatchArguments>(&work.dispatch))
            {
                auto result = read(indirect->buffer, EGraphUse::INDIRECT, GraphBufferRange{indirect->offset, 12});
                if (!result) return result;
            }
            return {};
        }

        [[nodiscard]] RenderResult<void> transfer(CopyImageBuffer& copy) noexcept
        {
            const bool preserve = copy.destination.access == EGraphAccess::READ_WRITE;
            copy.source.access = EGraphAccess::READ;
            copy.destination.access = preserve ? EGraphAccess::READ_WRITE : EGraphAccess::WRITE;
            if (auto result = resolve(copy.source, EGraphUse::TRANSFER_SOURCE); !result) return result;
            if (auto result = resolve(copy.destination, EGraphUse::TRANSFER_DESTINATION); !result) return result;
            const bool source_image = std::holds_alternative<GraphImageRange>(copy.source.range);
            const bool destination_image = std::holds_alternative<GraphImageRange>(copy.destination.range);
            if (source_image == destination_image) return cxx::unexpected(graphError(kGraphInvalidDescriptor));
            auto image_access = source_image ? copy.source : copy.destination;
            if (!source_image && copy.image_region) image_access.access = EGraphAccess::READ_WRITE;
            const auto resources = builder_.candidate_.resources();
            const auto& image = std::get<GraphImageDesc>(resources[image_access.version.resource.index].description);
            auto ranges = copyBufferRanges(copy, image);
            if (!ranges) return cxx::unexpected(ranges.error());
            auto buffer_access = source_image ? copy.destination : copy.source;
            if (source_image && ranges->empty() && !preserve)
                return cxx::unexpected(graphError(kGraphInvalidDescriptor, {}, buffer_access.version.resource));
            draft_.pass.uses.push_back(image_access);
            if (ranges->empty() || (source_image && preserve)) draft_.pass.uses.push_back(buffer_access);
            else
            {
                buffer_access.whole_resource = false;
                for (const auto& range : *ranges)
                {
                    buffer_access.range = range;
                    draft_.pass.uses.push_back(buffer_access);
                }
            }
            return {};
        }

        template <class Operation>
        [[nodiscard]] RenderResult<void> transfer(Operation& op) noexcept
        {
            constexpr bool image = std::same_as<Operation, CopyImage> || std::same_as<Operation, ClearImage>;
            if constexpr (requires { op.source; })
            {
                op.source.access = EGraphAccess::READ;
                if (auto result = append(op.source, EGraphUse::TRANSFER_SOURCE); !result) return result;
            }
            op.destination.access = EGraphAccess::WRITE;
            if constexpr (std::same_as<Operation, CopyImage>)
                if (op.destination_region) op.destination.access = EGraphAccess::READ_WRITE;
            if (auto result = append(op.destination, EGraphUse::TRANSFER_DESTINATION); !result) return result;
            const bool valid_destination = std::holds_alternative<GraphImageRange>(op.destination.range) == image;
            if constexpr (requires { op.source; })
                if (std::holds_alternative<GraphImageRange>(op.source.range) != image)
                    return cxx::unexpected(graphError(kGraphInvalidDescriptor, {}, op.destination.version.resource));
            if (!valid_destination)
                return cxx::unexpected(graphError(kGraphInvalidDescriptor, {}, op.destination.version.resource));
            return {};
        }

        [[nodiscard]] RenderResult<void> normalize(GraphTransferCommand& transfer_command) noexcept
        {
            auto& work = transfer_command.work;
            std::vector<GraphResourceId> storage_fills;
            for (auto& operation : work.operations)
            {
                auto result = std::visit([this](auto& op) noexcept { return transfer(op); }, operation);
                if (!result) return result;
                const auto* fill = std::get_if<FillBuffer>(&operation);
                if (fill && fill->value) storage_fills.push_back(fill->destination.version.resource);
            }
            // All fills of one buffer use the same native write scope within this pass.
            for (const auto& operation : work.operations)
            {
                const auto* fill = std::get_if<FillBuffer>(&operation);
                if (!fill) continue;
                const auto resource = fill->destination.version.resource;
                if (std::ranges::find(storage_fills, resource) != storage_fills.end())
                    transfer_command.typed_buffer_writes.push_back(fill->destination);
            }
            return {};
        }

        const RenderGraphBuilder& builder_;
        std::span<const std::uint32_t> versions_;
        GraphPassId id_;
        NormalizedPassDraft draft_;
    };

    template <class Work>
    RenderResult<PassResult>
    RenderGraphBuilder::appendPass(std::string key, Work work, GraphPassOptions options) noexcept
    {
        const GraphPassId id{static_cast<std::uint32_t>(passCount())};
        PassNormalizer operation{*this, versions_, id};
        const bool may_split = !choices_.empty() && !branch_ && !options.condition;
        auto draft = may_split ? operation.build(key, work, options) :
            operation.build(std::move(key), std::move(work), options);
        if (!draft) return cxx::unexpected(draft.error());
        if (may_split && draft->selected_value)
        {
            if (passCount() >= kGraphInvalidIndex - 1) return cxx::unexpected(graphError(kGraphInvalidDescriptor));
            options.condition = draft->pass.condition;
            options.condition->expected = !options.condition->expected;
            const GraphPassId peer{id.index + 1};
            PassNormalizer alternate{*this, versions_, peer};
            auto other = alternate.build(key + "/alternate", std::move(work), options);
            if (!other) return cxx::unexpected(other.error());
            if (draft->written != other->written) return cxx::unexpected(graphError(kGraphInvalidDescriptor, id));
            draft->pass.choice_peer = peer;
            other->pass.choice_peer = id;
            candidate_.passes_.push_back(std::move(draft->pass));
            candidate_.passes_.push_back(std::move(other->pass));
        }
        else
        {
            candidate_.passes_.push_back(std::move(draft->pass));
        }
        for (const auto version : draft->written) versions_[version.resource.index] = version.number;
        return PassResult{id, std::move(draft->written)};
    }

    RenderResult<PassResult>
    RenderGraphBuilder::addRasterPass(std::string key, RasterWork work, GraphPassOptions options) noexcept
    {
        return appendPass(std::move(key), std::move(work), options);
    }

    RenderResult<PassResult>
    RenderGraphBuilder::addComputePass(std::string key, ComputeWork work, GraphPassOptions options) noexcept
    {
        return appendPass(std::move(key), std::move(work), options);
    }

    RenderResult<PassResult>
    RenderGraphBuilder::addTransferPass(std::string key, TransferWork work, GraphPassOptions options) noexcept
    {
        return appendPass(std::move(key), std::move(work), options);
    }

    RenderResult<PassResult> RenderGraphBuilder::addComputeSequence(
        std::string key, std::vector<ComputeWork> commands, GraphPassOptions options
    ) noexcept
    {
        const bool invalid_count = commands.empty() || commands.size() >= kGraphInvalidIndex - passCount();
        if (key.empty() || invalid_count) return cxx::unexpected(graphError(kGraphInvalidDescriptor));
        if (commands.size() == 1) return addComputePass(std::move(key), std::move(commands.front()), options);

        const GraphPassId first{static_cast<std::uint32_t>(passCount())};
        const auto count = static_cast<std::uint32_t>(commands.size());
        std::vector<GraphPass> staged;
        staged.reserve(commands.size());
        const bool may_split = !choices_.empty() && !branch_ && !options.condition;
        const auto stage = [&](std::string name, GraphPassId start, bool preserve) -> RenderResult<PassResult>
        {
            auto staged_versions = versions_;
            PassResult result{start, {}};
            const auto offset = staged.size();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                PassNormalizer operation{*this, staged_versions, {start.index + i}};
                auto command = preserve ? commands[i] : std::move(commands[i]);
                auto draft = operation.build(i ? name + "/" + std::to_string(i) : name, std::move(command), options);
                if (!draft) return cxx::unexpected(draft.error());
                if (draft->selected_value) options.condition = draft->pass.condition;
                draft->pass.sequence_first = start;
                draft->pass.sequence_last = {start.index + count - 1};
                staged.push_back(std::move(draft->pass));
                for (const auto version : draft->written)
                {
                    staged_versions[version.resource.index] = version.number;
                    const auto previous = std::ranges::find(result.written, version.resource, &ResourceVersion::resource);
                    if (previous == result.written.end()) result.written.push_back(version);
                    else *previous = version;
                }
            }
            if (options.condition)
                for (auto& member : std::span{staged}.subspan(offset)) member.condition = options.condition;
            return result;
        };
        auto result = stage(key, first, may_split);
        if (!result) return cxx::unexpected(result.error());
        if (may_split && options.condition)
        {
            if (count > (kGraphInvalidIndex - first.index) / 2)
                return cxx::unexpected(graphError(kGraphInvalidDescriptor));
            options.condition->expected = !options.condition->expected;
            const GraphPassId second{first.index + count};
            auto other = stage(key + "/alternate", second, false);
            if (!other) return cxx::unexpected(other.error());
            if (result->written != other->written) return cxx::unexpected(graphError(kGraphInvalidDescriptor, first));
            for (std::uint32_t i = 0; i < count; ++i)
            {
                staged[i].choice_peer = {second.index + i};
                staged[count + i].choice_peer = {first.index + i};
            }
        }
        for (auto& pass : staged)
        {
            const auto index = static_cast<std::uint32_t>(passCount());
            if (index < pass.sequence_last.index) candidate_.dependencies_.push_back({{index}, {index + 1}});
            candidate_.passes_.push_back(std::move(pass));
        }
        for (const auto version : result->written) versions_[version.resource.index] = version.number;
        return result;
    }
} // namespace lux::render
