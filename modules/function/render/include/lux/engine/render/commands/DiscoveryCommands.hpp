#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/capabilities/CapabilityComposition.hpp>
#include <string>
#include <vector>

namespace lux::render
{
    struct ListCapabilities final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name = "render.capabilities.list";
        static constexpr bool requires_frame_boundary = false;
        using Request = CapabilityQuery;
        using Response = CapabilityList;
    };

    // Checks the selected implementation's configuration without creating any resource or scope.
    struct ValidateCapabilityConfiguration final
    {
        static constexpr std::string_view name = "render.capabilities.validate_configuration";
        static constexpr bool requires_frame_boundary = false;
        static constexpr bool failure_safe = true;

        struct Request final
        {
            CapabilityImplementationId implementation_id;
            VCapabilityConfiguration configuration;
        };

        using Response = AckResponse;
    };

    struct CommandInfo final
    {
        // Names are owned; type names borrow the defining module, which must outlive clients and the Server.
        std::string name;
        CommandId id;
        cxx::type_info request;
        cxx::type_info response;
        bool bound = false;
        bool requires_frame_boundary = false;
        bool failure_safe = false;
        cxx::type_info data;
        bool capability_command = false;

        [[nodiscard]] CommandDescriptor descriptor() const noexcept
        {
            return {
                CommandName{name},
                request,
                response,
                requires_frame_boundary,
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

    struct HandlerInfo final
    {
        HandlerId id;
        std::string name;
        std::string instance;
    };

    struct CommandBindings final
    {
        RenderError error{};
        HandlerId selected;
        std::vector<HandlerInfo> handlers;
    };

    struct CommandSelection final
    {
        CommandId command;
        HandlerId handler;
    };

    struct QueryCommandIds final
    {
        // The sole bootstrap slot, installed first and retained for this Server lifetime.
        static constexpr CommandId id{0, 1};
        static constexpr std::string_view name = "render.commands.query_ids";
        static constexpr bool requires_frame_boundary = false;
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
        static constexpr bool requires_frame_boundary = false;
        using Request = EmptyRequest;
        using Response = CommandList;
    };

    struct ListCommandBindings final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name{"render.commands.bindings"};
        static constexpr bool requires_frame_boundary = false;
        using Request = CommandId;
        using Response = CommandBindings;
    };

    struct SelectCommandBinding final
    {
        static constexpr std::string_view name{"render.commands.select"};
        static constexpr bool requires_frame_boundary = false;
        using Request = CommandSelection;
        using Response = AckResponse;
    };
} // namespace lux::render
