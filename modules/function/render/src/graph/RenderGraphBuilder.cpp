#include <lux/engine/render/extensions/graph/RenderGraphBuilder.hpp>

#include <bit>
#include <lux/engine/render/graph/Range.hpp>
#include <numeric>
#include <tuple>

namespace lux::render
{
    using detail::graphError;

    namespace
    {
        bool compatibleUse(const VGraphResourceDesc& description, EGraphUse usage) noexcept
        {
            if (usage > EGraphUse::UNIFORM_READ)
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
            case EGraphUse::UNIFORM_READ:
                return !image;
            default:
                return true;
            }
        }

        RenderResult<void> prepareResource(GraphResource& resource) noexcept
        {
            if (auto* image = std::get_if<GraphImageDesc>(&resource.description);
                image && image->extent.source != EImageExtentSource::FIXED)
            {
                image->width = image->height = 0;
            }

            const bool is_invalid_origin = resource.origin > EGraphOrigin::FRAME_BUFFER ||
                                           !compatibleUse(resource.description, resource.initial_use);
            const bool has_illegal_initial = !resource.initial_ranges.empty() &&
                                             resource.origin != EGraphOrigin::IMPORTED &&
                                             resource.origin != EGraphOrigin::HISTORY_PREVIOUS;
            const bool is_invalid_frame_buffer = resource.origin == EGraphOrigin::FRAME_BUFFER &&
                (!std::holds_alternative<GraphBufferDesc>(resource.description) ||
                    resource.initial_use != EGraphUse::UNSPECIFIED);
            if (resource.key.empty() || is_invalid_origin || has_illegal_initial || is_invalid_frame_buffer)
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
                const bool fixed = image.extent.source == EImageExtentSource::FIXED;
                const bool is_invalid_extent = image.layers == 0 || (fixed && (!image.width || !image.height));
                const bool is_invalid_mips =
                    image.mips == 0 || (fixed && image.mips > std::bit_width(std::max(image.width, image.height)));
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
        if (auto valid = prepareResource(resource); !valid)
        {
            return cxx::unexpected(graphError(valid.error().type, {}, id));
        }
        if (resource.output_range)
        {
            const bool is_buffer = std::holds_alternative<GraphBufferDesc>(resource.description);
            if (resource.origin != EGraphOrigin::IMPORTED || !is_buffer)
                return cxx::unexpected(graphError(kGraphInvalidDescriptor, {}, id));
            auto range = detail::normalize(resource, *resource.output_range);
            if (!range) return cxx::unexpected(graphError(kGraphInvalidRange, {}, id));
            resource.output_range = std::get<GraphBufferRange>(*range);
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

    RenderResult<HistoryDescription>
    RenderGraphBuilder::declareHistory(std::string key, HistoryDeclaration declaration) noexcept
    {
        const auto& [description, initial, canonical_use, distance] = declaration;
        const bool invalid_capacity = candidate_.resources_.size() > kGraphInvalidIndex - 2;
        const bool invalid_use = canonical_use != EGraphUse::SAMPLED && canonical_use != EGraphUse::STORAGE_READ &&
                                 canonical_use != EGraphUse::TRANSFER_SOURCE;
        const bool invalid_distance = distance == 0 || distance == kGraphInvalidIndex;
        if (key.empty() || invalid_distance || invalid_capacity || invalid_use ||
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
        if (auto valid = prepareResource(previous); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        const auto whole = detail::normalize(previous, {});
        previous.initial_ranges = {*whole};
        GraphResource
            current{key + ".current", previous.description, EGraphOrigin::HISTORY_CURRENT, {}, canonical_use};
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

    RenderResult<void>
    RenderGraphBuilder::publishHistory(ResourceVersion version, std::optional<PassCondition> when) noexcept
    {
        if (branch_)
            return cxx::unexpected(graphError(kGraphInvalidDescriptor, {}, version.resource));
        if (version.selection != kGraphInvalidIndex)
        {
            if (version.selection >= choices_.size() || (when && !candidate_.validCondition(*when)))
                return cxx::unexpected(graphError(kGraphInvalidDescriptor, {}, version.resource));
            const auto& choice = choices_[version.selection];
            if (choice.first.resource != version.resource || choice.first.number != version.number)
                return cxx::unexpected(graphError(kGraphInvalidDescriptor, {}, version.resource));
            auto primary = choice.first;
            auto alternate = choice.second;
            auto predicate = choice.predicate;
            auto history = std::ranges::find(candidate_.histories_, primary.resource, &HistoryDescription::current);
            if (history == candidate_.histories_.end())
            {
                std::swap(primary, alternate);
                predicate.expected = !predicate.expected;
                history = std::ranges::find(candidate_.histories_, primary.resource, &HistoryDescription::current);
            }
            if (history == candidate_.histories_.end() || history->publication || !primary.number)
                return cxx::unexpected(graphError(kGraphInvalidDescriptor, {}, primary.resource));
            const bool produces = alternate.resource == history->current && alternate.number != 0;
            const bool holds = alternate == ResourceVersion{history->previous, 0};
            if (!produces && !holds) return cxx::unexpected(graphError(kGraphInvalidDescriptor, {}, alternate.resource));
            history->publication = primary;
            if (produces) history->alternate_sample = alternate;
            history->publish_when = when;
            history->select_primary = predicate;
            return {};
        }
        auto history = std::ranges::find(candidate_.histories_, version.resource, &HistoryDescription::current);
        if (history == candidate_.histories_.end() || history->publication || version.number == 0)
        {
            return cxx::unexpected(graphError(kGraphInvalidDescriptor, {}, version.resource));
        }
        // Analysis resolves this whole-resource observation separately from external Graph exports.
        if (when && !candidate_.validCondition(*when))
            return cxx::unexpected(graphError(kGraphInvalidDescriptor, {}, version.resource));
        if (!when)
        {
            // An explicit write fallback preserves an older value when its producing pass is skipped.
            for (const auto& pass : candidate_.passes())
            {
                if (!pass.condition) continue;
                const bool preserves_sample = std::ranges::any_of(pass.uses, [&](const ResourceAccess& use)
                {
                    return detail::writes(use.access) && use.version == version && use.fallback.has_value();
                });
                if (preserves_sample)
                {
                    when = pass.condition;
                    break;
                }
            }
        }
        history->publication = version;
        history->publish_when = when;
        return {};
    }

    RenderResult<ResourceVersion>
    RenderGraphBuilder::declareChoice(PassCondition predicate, ChoiceArm when_true, ChoiceArm when_false) noexcept
    {
        if (branch_ || !candidate_.validCondition(predicate) || choices_.size() >= kGraphInvalidIndex)
            return cxx::unexpected(graphError(kGraphInvalidDescriptor));

        // Arms append declarations to this same builder. A rejected arm must also undo any writes to old resources.
        const auto resources = resourceCount();
        const auto passes = passCount();
        const auto dependencies = dependencyCount();
        const auto histories = candidate_.histories_.size();
        const auto programs = candidate_.programs_.size();
        const auto samplers = samplerCount();
        const auto conditions = candidate_.conditions_.size();
        const auto predicates = candidate_.binding_predicates_.size();
        auto versions = versions_;
        const auto reject = [&](RenderError error) -> RenderResult<ResourceVersion>
        {
            candidate_.resources_.resize(resources);
            candidate_.passes_.resize(passes);
            candidate_.dependencies_.resize(dependencies);
            candidate_.histories_.resize(histories);
            candidate_.programs_.resize(programs);
            program_fields_.resize(programs);
            candidate_.samplers_.resize(samplers);
            candidate_.conditions_.resize(conditions);
            candidate_.binding_predicates_.resize(predicates);
            versions_ = std::move(versions);
            branch_.reset();
            return cxx::unexpected(error);
        };
        const auto valid = [&](ResourceVersion value) noexcept
        {
            return value.selection == kGraphInvalidIndex && value.resource.index < versions_.size() &&
                value.number <= versions_[value.resource.index];
        };
        branch_ = predicate;
        const auto first = when_true(*this);
        if (!first) return reject(first.error());
        if (!valid(*first)) return reject(graphError(kGraphUndefinedRead, {}, first->resource));
        branch_->expected = !predicate.expected;
        const auto second = when_false(*this);
        if (!second) return reject(second.error());
        if (!valid(*second)) return reject(graphError(kGraphUndefinedRead, {}, second->resource));
        const auto& a = *resourceDescription(first->resource);
        const auto& b = *resourceDescription(second->resource);
        if (a.index() != b.index()) return reject(graphError(kGraphInvalidDescriptor, {}, second->resource));
        if (const auto* image = std::get_if<GraphImageDesc>(&a))
        {
            const auto& other = std::get<GraphImageDesc>(b);
            const bool fixed = image->extent.source == EImageExtentSource::FIXED &&
                other.extent.source == EImageExtentSource::FIXED;
            const bool incompatible_extent = fixed && (image->width != other.width || image->height != other.height);
            const bool compatible_views = image->format == other.format && image->samples == other.samples &&
                image->mips == other.mips && image->layers == other.layers &&
                image->runtime_layers == other.runtime_layers;
            if (incompatible_extent || !compatible_views)
                return reject(graphError(kGraphInvalidDescriptor, {}, second->resource));
        }
        else if (a != b) return reject(graphError(kGraphInvalidDescriptor, {}, second->resource));
        branch_.reset();
        if (*first == *second) return *first;
        const auto index = static_cast<std::uint32_t>(choices_.size());
        choices_.push_back({predicate, *first, *second});
        return ResourceVersion{first->resource, first->number, index};
    }

    RenderResult<PassCondition> RenderGraphBuilder::sameBinding(GraphResourceId left, GraphResourceId right) noexcept
    {
        const auto resources = candidate_.resources();
        for (const auto id : {left, right})
        {
            if (id.index >= resources.size()) return cxx::unexpected(graphError(kGraphUnknownResource, {}, id));
            if (resources[id.index].origin != EGraphOrigin::IMPORTED)
                return cxx::unexpected(graphError(kGraphInvalidDescriptor, {}, id));
        }
        if (left.index > right.index) std::swap(left, right);
        auto& table = candidate_.binding_predicates_;
        const BindingPredicate predicate{left, right};
        const auto found = std::ranges::find(table, predicate);
        const auto index = static_cast<std::uint32_t>(found - table.begin());
        if (found == table.end())
        {
            if (table.size() >= kGraphInvalidIndex) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            table.push_back(predicate);
        }
        return PassCondition{EConditionSource::SAME_BINDING, index};
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

    RenderResult<std::uint32_t> RenderGraphBuilder::addProgram(GraphProgram program) noexcept
    {
        if (!program.valid() || candidate_.programs_.size() >= kGraphInvalidIndex)
        {
            return cxx::unexpected(graphError(kGraphInvalidDescriptor));
        }
        const auto index = static_cast<std::uint32_t>(candidate_.programs_.size());
        // Store numeric locations so moving or copying a Builder preserves the reflected field index.
        const auto& sets = program.targets.front().interface.sets;
        std::vector<ProgramField> fields;
        for (std::size_t set = 0; set < sets.size(); ++set)
        {
            for (std::size_t binding = 0; binding < sets[set].bindings.size(); ++binding)
            {
                if (fields.size() == kGraphInvalidIndex || set >= kGraphInvalidIndex)
                    return cxx::unexpected(graphError(kGraphInvalidDescriptor));
                fields.push_back({
                    static_cast<std::uint32_t>(set), static_cast<std::uint32_t>(binding),
                    static_cast<std::uint32_t>(fields.size())
                });
            }
        }
        std::ranges::sort(fields, [&](const ProgramField& first, const ProgramField& second) noexcept
        {
            const auto& a = sets[first.set].bindings[first.binding].name;
            const auto& b = sets[second.set].bindings[second.binding].name;
            return std::tie(a, first.ordinal) < std::tie(b, second.ordinal);
        });
        program_fields_.push_back(std::move(fields));
        candidate_.programs_.push_back(std::move(program));
        return index;
    }

    RenderResult<ResourceVersion> RenderGraphBuilder::nextWriteVersion(GraphResourceId resource) const noexcept
    {
        if (resource.index >= versions_.size())
        {
            return cxx::unexpected(graphError(kGraphUnknownResource, {}, resource));
        }
        const auto version = versions_[resource.index];
        if (version == UINT32_MAX)
        {
            return cxx::unexpected(graphError(kGraphInvalidDescriptor, {}, resource));
        }
        return ResourceVersion{resource, version + 1};
    }

    RenderResult<void> RenderGraphBuilder::dependsOn(GraphPassId before, GraphPassId after) noexcept
    {
        const bool is_unknown = before.index >= candidate_.passes_.size() || after.index >= candidate_.passes_.size();
        if (is_unknown)
        {
            return cxx::unexpected(graphError(kGraphUnknownPass, before, {}, after.index));
        }
        const auto& predecessor = candidate_.passes_[before.index];
        const auto& successor = candidate_.passes_[after.index];
        const bool same_sequence = predecessor.sequence_first.index != kGraphInvalidIndex &&
                                   predecessor.sequence_first == successor.sequence_first;
        if (before == after || predecessor.choice_peer == after || same_sequence)
        {
            return cxx::unexpected(graphError(kGraphDependencyCycle, before));
        }
        const GraphDependency edge{
            predecessor.sequence_last.index == kGraphInvalidIndex ? before : predecessor.sequence_last,
            successor.sequence_first.index == kGraphInvalidIndex ? after : successor.sequence_first
        };
        auto& edges = candidate_.dependencies_;
        for (const auto source : {edge.before, candidate_.passes_[edge.before.index].choice_peer})
        {
            if (source.index == kGraphInvalidIndex) continue;
            for (const auto destination : {edge.after, candidate_.passes_[edge.after.index].choice_peer})
            {
                if (destination.index == kGraphInvalidIndex) continue;
                const GraphDependency dependency{source, destination};
                if (std::ranges::find(edges, dependency) == edges.end()) edges.push_back(dependency);
            }
        }
        return {};
    }

    RenderResult<void> RenderGraphBuilder::exportOutput(GraphExport output) noexcept
    {
        if (branch_ || output.alternate) return cxx::unexpected(graphError(kGraphInvalidDescriptor));
        const auto selection = output.version.selection;
        if (selection != kGraphInvalidIndex)
        {
            if (selection >= choices_.size()) return cxx::unexpected(graphError(kGraphInvalidDescriptor));
            const auto& choice = choices_[selection];
            if (output.version.resource != choice.first.resource || output.version.number != choice.first.number)
                return cxx::unexpected(graphError(kGraphInvalidDescriptor, {}, output.version.resource));
            output.version = choice.first;
            output.alternate = GraphExportAlternative{choice.predicate, choice.second};
        }
        if (output.alternate)
        {
            const auto& alternate = *output.alternate;
            if (!candidate_.validCondition(alternate.primary_when) || alternate.value.selection != kGraphInvalidIndex)
                return cxx::unexpected(graphError(kGraphInvalidDescriptor));
            // Both alternatives of a native Target must address the one installed writable output.
            if (output.target && alternate.value.resource != output.version.resource)
                return cxx::unexpected(graphError(kGraphConflictingOutput, {}, alternate.value.resource));
            auto other = output;
            other.version = alternate.value;
            other.alternate.reset();
            if (auto valid = validateOutput(other); !valid) return valid;
            if (auto valid = validateOutput(output); !valid) return valid;
            if (other.range != output.range) return cxx::unexpected(graphError(kGraphInvalidRange));
        }
        else if (auto valid = validateOutput(output); !valid) return valid;
        candidate_.outputs_.push_back(std::move(output));
        return {};
    }

    RenderResult<void> RenderGraphBuilder::validateOutput(GraphExport& output) const noexcept
    {
        const auto id = output.version.resource;
        if (output.final_use &&
            (*output.final_use == EGraphUse::UNSPECIFIED || *output.final_use > EGraphUse::UNIFORM_READ))
        {
            return cxx::unexpected(graphError(kGraphInvalidDescriptor, {}, id));
        }
        if (id.index >= candidate_.resources_.size())
        {
            return cxx::unexpected(graphError(kGraphUnknownResource, {}, id));
        }
        const auto& resource = candidate_.resources_[id.index];
        if (resource.output_range && std::holds_alternative<std::monostate>(output.range))
            output.range = *resource.output_range;
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
        if (resource.output_range)
        {
            const auto bytes = std::get<GraphBufferRange>(output.range);
            const auto claim = *resource.output_range;
            if (!detail::contains(claim, bytes))
                return cxx::unexpected(graphError(kGraphInvalidRange, {}, id));
        }
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
            const bool shared = existing.version.resource == id ||
                (existing.alternate && existing.alternate->value.resource == id);
            const bool is_conflict = shared && detail::overlaps(existing.range, output.range);
            if (is_conflict)
            {
                return cxx::unexpected(graphError(kGraphConflictingOutput, {}, id));
            }
        }
        return {};
    }

    RenderResult<RenderGraph> RenderGraphBuilder::finish() && noexcept
    {
        if (branch_) return cxx::unexpected(graphError(kGraphInvalidDescriptor));
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
        choices_.clear();
        program_fields_.clear();
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
