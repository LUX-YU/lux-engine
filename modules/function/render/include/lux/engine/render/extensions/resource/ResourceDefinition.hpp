#pragma once

#include <lux/engine/render/visibility.h>

#include <array>
#include <algorithm>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/Resource.hpp>
#include <lux/engine/render/extensions/resource/ResourceIdentity.hpp>
#include <tuple>

namespace lux::render
{
    template <RenderCommand... Commands>
    [[nodiscard]] std::span<const CommandDescriptor* const>
    resourceCommandDescriptors(std::tuple<Commands...>*) noexcept
    {
        static const std::array<const CommandDescriptor*, sizeof...(Commands)> values{&commandDescriptor<Commands>()...
        };
        return values;
    }

    template <class... Inputs>
    [[nodiscard]] std::span<const cxx::basic_type_info> resourceFrameTypes(std::tuple<Inputs...>*) noexcept
    {
        static const std::array<cxx::basic_type_info, sizeof...(Inputs)> types{cxx::make_basic_type_info<Inputs>()...};
        return types;
    }

    enum class EStorageCapability : std::uint32_t
    {
        NONE = 0,
        SAMPLED_IMAGE_ARRAY = 1,
        BLOCK_COMPRESSION_BC = 2,
        STORAGE_BUFFER_ARRAY = 4
    };

    struct ResourceRequirement final
    {
        ResourceName type;
        bool operator==(const ResourceRequirement&) const noexcept = default;
    };

    // Static metadata. A descriptor is not an instance, a resource budget or a GPU owner.
    // Config/Ops qualification belongs to the typed Factory, not a manual version or encoded config schema.
    struct ResourceDefinition final
    {
        ResourceNameView    type;
        std::uint32_t       required_capabilities = 0;
        std::uint32_t       optional_capabilities = 0;
        cxx::basic_type_info binding_type;
        std::string_view description;
        EResourceDomain domain = EResourceDomain::DEVICE;
        std::span<const CommandDescriptor* const> commands;
        std::span<const cxx::basic_type_info> frame_inputs;

        [[nodiscard]] bool supports(std::uint32_t capabilities) const noexcept
        {
            return (required_capabilities & ~capabilities) == 0;
        }

        [[nodiscard]] bool hasSameContract(const ResourceDefinition& other) const noexcept
        {
            const bool same_policy =
                type == other.type &&
                required_capabilities == other.required_capabilities &&
                optional_capabilities == other.optional_capabilities &&
                binding_type == other.binding_type &&
                domain == other.domain;
            const bool same_commands_count = commands.size() == other.commands.size();
            if (!(same_policy && same_commands_count))
            {
                return false;
            }
            for (std::size_t i = 0; i < commands.size(); ++i)
            {
                const bool has_commands = commands[i] && other.commands[i];
                const bool is_command_mismatch = !has_commands || !compatibleCommands(*commands[i], *other.commands[i]);
                if (is_command_mismatch)
                {
                    return false;
                }
            }
            return std::ranges::equal(frame_inputs, other.frame_inputs);
        }
    };

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<void>
    validateStorage(const ResourceDefinition& descriptor) noexcept;

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<void>
    compatibleStorage(const ResourceDefinition& expected, const ResourceDefinition& actual) noexcept;

    [[nodiscard]] LUX_RENDER_PUBLIC std::span<const error::ErrorDescriptor>
    renderResourceErrorDescriptors() noexcept;
} // namespace lux::render
