#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/effect/EffectCatalog.hpp>
#include <string>
#include <vector>

namespace lux::render
{
    struct EffectCapabilityInfo final
    {
        std::shared_ptr<const CapabilityDefinition> definition;
        std::vector<ImplementationOption> implementations;
    };

    struct ListEffectCapabilities final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name{"render.effect.capabilities.list"};
        using Request = EmptyRequest;

        struct Response final
        {
            RenderError error{};
            std::vector<EffectCapabilityInfo> capabilities;
        };
    };

    struct CommandInfo final
    {
        // Names are owned; type names borrow the defining module, which must outlive clients and the Server.
        std::string name;
        CommandId id;
        cxx::basic_type_info request;
        cxx::basic_type_info response;
        bool bound = false;
        bool requires_idle_resource = false;
        bool failure_safe = false;
        cxx::basic_type_info data;
        bool capability_command = false;

        [[nodiscard]] CommandDescriptor descriptor() const noexcept
        {
            return {
                CommandName{name},
                request,
                response,
                requires_idle_resource,
                failure_safe,
                data,
                capability_command
            };
        }
    };

    struct CommandList final
    {
        RenderError error{};
        std::vector<CommandInfo> commands;
    };

    struct QueryCommandIds final
    {
        // The sole bootstrap slot, installed first and retained for this Server lifetime.
        static constexpr CommandId id{0, 1};
        static constexpr std::string_view name = "render.commands.query_ids";
        static constexpr bool failure_safe = true;
        using Request = std::span<const CommandName>;

        struct Response final
        {
            RenderError error{};
            // Same order as Request; a failure clears the whole result and identifies its input index.
            std::vector<CommandId> ids;
        };
    };

    struct ListCommands final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name{"render.commands.list"};
        using Request = EmptyRequest;
        using Response = CommandList;
    };

} // namespace lux::render
