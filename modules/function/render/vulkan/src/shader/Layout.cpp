#include <lux/engine/render/vulkan/shader/Layout.hpp>

#include <algorithm>
#include <array>
#include <limits>

namespace lux::render::vulkan
{
    namespace
    {
        VkDescriptorType descriptorType(rdesc::EPassFieldRole role) noexcept
        {
            using R = rdesc::EPassFieldRole;
            switch (role)
            {
            case R::SAMPLED_READ:
                return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            case R::SAMPLER:
                return VK_DESCRIPTOR_TYPE_SAMPLER;
            case R::UNIFORM_READ:
                return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            case R::READ_ONLY_STORAGE:
            case R::READ_WRITE_STORAGE:
                return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            case R::INPUT_ATTACHMENT:
                return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            default:
                return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            }
        }

        VkDescriptorType baseType(VkDescriptorType type) noexcept
        {
            if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
            {
                return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            }
            if (type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
            {
                return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            }
            return type;
        }

        std::string_view semantic(const rdesc::PassResourceField& field) noexcept
        {
            return field.semantic.empty() ? field.path : field.semantic;
        }

        bool isBlockScalar(const rdesc::PassShaderContract& schema, std::string_view path) noexcept
        {
            return std::any_of(
                schema.resources.begin(),
                schema.resources.end(),
                [&](const auto& field)
                {
                    const auto type = descriptorType(field.role);
                    const bool is_buffer =
                        type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    return is_buffer && path.starts_with(field.path) && path.size() > field.path.size() &&
                           path[field.path.size()] == '.';
                }
            );
        }

        struct Budget
        {
            std::uint64_t sampler{}, uniform{}, storage{}, sampled{}, image{}, input{}, total{}, dynamic_uniform{},
                dynamic_storage{};

            bool add(const OwnerField& field) noexcept
            {
                const auto count = field.count;
                switch (field.type)
                {
                case VK_DESCRIPTOR_TYPE_SAMPLER:
                    sampler += count;
                    break;
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                    sampler += count;
                    sampled += count;
                    break;
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                    sampled += count;
                    break;
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                    image += count;
                    break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                    uniform += count;
                    break;
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                    storage += count;
                    break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                    uniform += count;
                    dynamic_uniform += count;
                    break;
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                    storage += count;
                    dynamic_storage += count;
                    break;
                case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                    input += count;
                    break;
                default:
                    return false;
                }
                if (field.type != VK_DESCRIPTOR_TYPE_SAMPLER)
                {
                    total += count;
                }
                return true;
            }

            bool fits(const VkPhysicalDeviceLimits& limits, bool stage) const noexcept
            {
                if (stage)
                {
                    return sampler <= limits.maxPerStageDescriptorSamplers &&
                           uniform <= limits.maxPerStageDescriptorUniformBuffers &&
                           storage <= limits.maxPerStageDescriptorStorageBuffers &&
                           sampled <= limits.maxPerStageDescriptorSampledImages &&
                           image <= limits.maxPerStageDescriptorStorageImages &&
                           input <= limits.maxPerStageDescriptorInputAttachments &&
                           total <= limits.maxPerStageResources;
                }
                return sampler <= limits.maxDescriptorSetSamplers && uniform <= limits.maxDescriptorSetUniformBuffers &&
                       storage <= limits.maxDescriptorSetStorageBuffers &&
                       sampled <= limits.maxDescriptorSetSampledImages &&
                       image <= limits.maxDescriptorSetStorageImages &&
                       input <= limits.maxDescriptorSetInputAttachments &&
                       dynamic_uniform <= limits.maxDescriptorSetUniformBuffersDynamic &&
                       dynamic_storage <= limits.maxDescriptorSetStorageBuffersDynamic;
            }
        };
    } // namespace

    VkShaderStageFlags nativeStages(std::uint32_t stages) noexcept
    {
        return ((stages & 1u) ? VK_SHADER_STAGE_VERTEX_BIT : 0u) | ((stages & 2u) ? VK_SHADER_STAGE_FRAGMENT_BIT : 0u) |
               ((stages & 4u) ? VK_SHADER_STAGE_COMPUTE_BIT : 0u);
    }

    DeviceLayoutCaps queryLayoutCaps(const VulkanDevice& device) noexcept
    {
        DeviceLayoutCaps result;
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        vkGetPhysicalDeviceProperties2(device.physical(), &properties);
        result.limits = properties.properties.limits;
        result.api_version = properties.properties.apiVersion;
        result.vendor_id = properties.properties.vendorID;
        result.device_id = properties.properties.deviceID;
        result.driver_version = properties.properties.driverVersion;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &result.supported_indexing;
        vkGetPhysicalDeviceFeatures2(device.physical(), &features);
        return result;
    }

    ShaderOwnerId shaderOwnerId(const OwnerShape& shape) noexcept
    {
        std::string key;
        const auto number = [&](std::uint64_t value)
        {
            for (unsigned byte = 0; byte < 8; ++byte)
            {
                key.push_back(static_cast<char>(value >> (byte * 8)));
            }
        };
        const auto text = [&](std::string_view value)
        {
            number(value.size());
            key.append(value);
        };
        text(shape.canonical_name);
        number(shape.revision);
        number(static_cast<std::uint32_t>(shape.category));
        number(shape.fields.size());
        std::vector<const OwnerField*> ordered;
        ordered.reserve(shape.fields.size());
        for (const auto& field : shape.fields)
        {
            ordered.push_back(&field);
        }
        std::sort(
            ordered.begin(),
            ordered.end(),
            [](const auto* a, const auto* b) { return a->semantic < b->semantic; }
        );
        for (const auto* value : ordered)
        {
            const auto& field = *value;
            text(field.semantic);
            number(field.type);
            number(field.count);
            number(field.stages);
            number(field.flags);
            number(field.element_stride);
            number(field.element_alignment);
            text(field.dimension);
            text(field.image_format);
            text(field.data_identity);
        }
        return ShaderOwnerId{cxx::Fnv1a64::hash(key)};
    }

    RenderResult<OwnerShape> declareOwnerShape(
        std::string_view name,
        std::uint32_t revision,
        rdesc::EFieldOwner category,
        std::span<const rdesc::PassShaderContract> complete_schemas
    ) noexcept
    {
        if (name.empty() || revision == 0 || category > rdesc::EFieldOwner::PASS_LOCAL)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        OwnerShape shape{ShaderOwnerId{cxx::Fnv1a64::hash(name)}, std::string(name), revision, category, {}};
        for (const auto& schema : complete_schemas)
        {
            for (const auto& field : schema.resources)
            {
                if (field.owner != category || !rdesc::isShaderDescriptorRole(field.role))
                {
                    continue;
                }
                OwnerField value{
                    std::string(semantic(field)),
                    descriptorType(field.role),
                    field.array_count,
                    field.stages,
                    0,
                    field.element_stride,
                    field.element_alignment,
                    std::string(field.dimension),
                    std::string(field.image_format),
                    field.data ? rdesc::dataBindingIdentity(*field.data) : std::string{}
                };
                const auto found = std::find_if(
                    shape.fields.begin(),
                    shape.fields.end(),
                    [&](const auto& previous) { return previous.semantic == value.semantic; }
                );
                if (found != shape.fields.end())
                {
                    auto merged = value;
                    merged.stages = found->stages;
                    if (*found != merged)
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument, {shape.identity.value()}});
                    }
                    found->stages |= value.stages;
                }
                else
                {
                    shape.fields.push_back(std::move(value));
                }
            }
        }
        if (shape.fields.empty())
        {
            return cxx::unexpected(RenderError{kInvalidArgument, {shape.identity.value()}});
        }
        std::sort(
            shape.fields.begin(),
            shape.fields.end(),
            [](const auto& a, const auto& b) { return a.semantic < b.semantic; }
        );
        shape.identity = shaderOwnerId(shape);
        return shape;
    }

    RenderResult<LayoutPlan> compileLayout(
        const rdesc::PassShaderContract& schema,
        std::span<const OwnerAssignment> assignment,
        std::span<const OwnerShape> complete_owners,
        const DeviceLayoutCaps& caps
    ) noexcept
    {
        // Every descriptor field has exactly one explicit structural owner association.
        // Non-descriptor fields and unknown/duplicate paths cannot silently enter a layout.
        for (std::size_t i = 0; i < assignment.size(); ++i)
        {
            const auto& value = assignment[i];
            const auto source = std::find_if(
                schema.resources.begin(),
                schema.resources.end(),
                [&](const auto& field)
                { return field.path == value.field_path && rdesc::isShaderDescriptorRole(field.role); }
            );
            const bool is_unknown_field = source == schema.resources.end();
            const bool is_duplicate_field = std::any_of(
                assignment.begin(),
                assignment.begin() + i,
                [&](const auto& old) { return old.field_path == value.field_path; }
            );
            if (is_unknown_field || is_duplicate_field || !value.owner.isValid())
            {
                return cxx::unexpected(RenderError{kInvalidArgument, {i}});
            }
        }
        const auto descriptor_count = std::count_if(
            schema.resources.begin(),
            schema.resources.end(),
            [](const auto& field) { return rdesc::isShaderDescriptorRole(field.role); }
        );
        if (assignment.size() != descriptor_count)
        {
            return cxx::unexpected(
                RenderError{kInvalidArgument, {assignment.size(), static_cast<std::uint64_t>(descriptor_count)}}
            );
        }
        LayoutIdentity result;
        result.owners.assign(complete_owners.begin(), complete_owners.end());
        std::sort(
            result.owners.begin(),
            result.owners.end(),
            [](const auto& a, const auto& b) { return a.canonical_name < b.canonical_name; }
        );
        std::array<Budget, 3> stage_budgets{};
        Budget budget;
        bool has_shared = false, has_local = false;
        for (std::size_t i = 0; i < result.owners.size(); ++i)
        {
            auto& owner = result.owners[i];
            const bool invalid_owner = owner.canonical_name.empty() || owner.revision == 0 || owner.fields.empty() ||
                                       owner.category > rdesc::EFieldOwner::PASS_LOCAL || !owner.identity.isValid() ||
                                       owner.identity != shaderOwnerId(owner);
            if (invalid_owner)
            {
                return cxx::unexpected(RenderError{kInvalidArgument, {i}});
            }
            for (std::size_t j = 0; j < i; ++j)
            {
                if (owner.identity == result.owners[j].identity ||
                    owner.canonical_name == result.owners[j].canonical_name)
                {
                    return cxx::unexpected(RenderError{kInvalidArgument, {i, j}});
                }
            }
            has_shared |= owner.category != rdesc::EFieldOwner::PASS_LOCAL;
            has_local |= owner.category == rdesc::EFieldOwner::PASS_LOCAL;
            std::sort(
                owner.fields.begin(),
                owner.fields.end(),
                [](const auto& a, const auto& b) { return a.semantic < b.semantic; }
            );
            for (std::size_t f = 0; f < owner.fields.size(); ++f)
            {
                const auto& field = owner.fields[f];
                const bool invalid_field = field.semantic.empty() || field.count == 0 || field.stages == 0 ||
                                           (field.stages & ~7u) != 0 || field.element_alignment == 0 ||
                                           (field.element_alignment & (field.element_alignment - 1)) != 0 ||
                                           (f != 0 && owner.fields[f - 1].semantic == field.semantic);
                if (invalid_field)
                {
                    return cxx::unexpected(RenderError{kInvalidArgument, {i, f}});
                }
                // Support is not enablement. This device factory has not enabled descriptor indexing.
                if (field.flags != 0)
                {
                    return cxx::unexpected(RenderError{kUnsupported, {i, f, field.flags}});
                }
                if (!budget.add(field))
                {
                    return cxx::unexpected(RenderError{kUnsupported, {i, f}});
                }
                for (std::uint32_t stage = 0; stage < 3; ++stage)
                {
                    if ((field.stages & (1u << stage)) != 0)
                    {
                        stage_budgets[stage].add(field);
                    }
                }
                const auto type = baseType(field.type);
                const bool invalid_range = (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                                            field.element_stride > caps.limits.maxUniformBufferRange) ||
                                           (type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
                                            field.element_stride > caps.limits.maxStorageBufferRange);
                if (invalid_range)
                {
                    return cxx::unexpected(RenderError{kCapacity, {i, f, field.element_stride}});
                }
            }
        }
        for (const auto& field : schema.resources)
        {
            if (field.role == rdesc::EPassFieldRole::COLOR_ATTACHMENT)
            {
                stage_budgets[1].total += field.array_count;
            }
        }
        if (!budget.fits(caps.limits, false))
        {
            return cxx::unexpected(RenderError{kCapacity, {0, budget.total}});
        }
        for (std::size_t stage = 0; stage < 3; ++stage)
        {
            if (!stage_budgets[stage].fits(caps.limits, true))
            {
                return cxx::unexpected(RenderError{kCapacity, {stage + 1, stage_budgets[stage].total}});
            }
        }
        const std::uint32_t set_count = has_shared + has_local;
        if (set_count > caps.limits.maxBoundDescriptorSets)
        {
            return cxx::unexpected(RenderError{kCapacity, {set_count, caps.limits.maxBoundDescriptorSets}});
        }
        result.sets.resize(set_count);
        for (const auto& owner : result.owners)
        {
            const std::uint32_t set = owner.category == rdesc::EFieldOwner::PASS_LOCAL && has_shared ? 1u : 0u;
            for (std::uint32_t f = 0; f < owner.fields.size(); ++f)
            {
                result.sets[set].bindings.push_back(
                    {owner.identity, f, static_cast<std::uint32_t>(result.sets[set].bindings.size())}
                );
            }
        }
        std::uint32_t dynamic_index = 0;
        for (std::uint32_t set = 0; set < result.sets.size(); ++set)
        {
            for (const auto& binding : result.sets[set].bindings)
            {
                const auto& owner = *std::find_if(
                    result.owners.begin(),
                    result.owners.end(),
                    [&](const auto& candidate) { return candidate.identity == binding.owner; }
                );
                const auto& field = owner.fields[binding.owner_field];
                const bool dynamic = field.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                                     field.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
                for (std::uint32_t f = 0; f < schema.resources.size(); ++f)
                {
                    const auto& source = schema.resources[f];
                    if (!rdesc::isShaderDescriptorRole(source.role))
                    {
                        continue;
                    }
                    const auto assigned = std::find_if(
                                              assignment.begin(),
                                              assignment.end(),
                                              [&](const auto& value) { return value.field_path == source.path; }
                    )->owner;
                    if (assigned != owner.identity || semantic(source) != field.semantic)
                    {
                        continue;
                    }
                    const bool mismatch =
                        source.owner != owner.category || descriptorType(source.role) != baseType(field.type) ||
                        source.array_count != field.count || (source.stages & field.stages) != source.stages ||
                        source.element_stride != field.element_stride ||
                        source.element_alignment != field.element_alignment || source.dimension != field.dimension ||
                        source.image_format != field.image_format ||
                        (source.data ? rdesc::dataBindingIdentity(*source.data) : std::string{}) != field.data_identity;
                    if (mismatch)
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument, {f, owner.identity.value()}});
                    }
                    result.fields.push_back(
                        {f,
                         set,
                         binding.binding,
                         std::string(source.path),
                         owner.identity,
                         field.type,
                         field.count,
                         field.stages,
                         field.flags,
                         dynamic ? dynamic_index : std::numeric_limits<std::uint32_t>::max()}
                    );
                }
                if (dynamic)
                {
                    dynamic_index += field.count;
                }
            }
        }
        result.dynamic_count = dynamic_index;
        const auto expected = std::count_if(
            schema.resources.begin(),
            schema.resources.end(),
            [](const auto& field) { return rdesc::isShaderDescriptorRole(field.role); }
        );
        if (result.fields.size() != expected)
        {
            return cxx::unexpected(
                RenderError{kInvalidArgument, {result.fields.size(), static_cast<std::uint64_t>(expected)}}
            );
        }
        std::sort(
            result.fields.begin(),
            result.fields.end(),
            [](const auto& a, const auto& b) { return a.field_index < b.field_index; }
        );
        for (std::uint32_t stage = 0; stage < 3; ++stage)
        {
            std::uint32_t first = std::numeric_limits<std::uint32_t>::max(), last = 0;
            for (const auto& scalar : schema.scalars)
            {
                if ((scalar.stages & (1u << stage)) == 0 || isBlockScalar(schema, scalar.path))
                {
                    continue;
                }
                const bool invalid_scalar = scalar.offset % 4 != 0 || scalar.size != 4 ||
                                            scalar.offset > caps.limits.maxPushConstantsSize ||
                                            scalar.size > caps.limits.maxPushConstantsSize - scalar.offset;
                if (invalid_scalar)
                {
                    return cxx::unexpected(RenderError{kCapacity, {scalar.offset, scalar.size}});
                }
                first = std::min(first, scalar.offset);
                last = std::max(last, scalar.offset + scalar.size);
            }
            if (last != 0)
            {
                result.push_ranges.push_back({first, last - first, 1u << stage});
            }
        }
        return LayoutPlan{std::move(result)};
    }

    std::string LayoutPlan::diagnostics() const noexcept
    {
        std::string result = "LayoutPlan\n";
        for (const auto& owner : identity_.owners)
        {
            result += "owner " + owner.canonical_name + " revision=" + std::to_string(owner.revision) +
                      " full-fields=" + std::to_string(owner.fields.size()) + "\n";
        }
        for (const auto& field : identity_.fields)
        {
            result += "field " + std::to_string(field.field_index) + " owner=" + std::to_string(field.owner.value()) +
                      " set=" + std::to_string(field.set) + " binding=" + std::to_string(field.binding) +
                      " count=" + std::to_string(field.count) + "\n";
        }
        return result;
    }
} // namespace lux::render::vulkan
