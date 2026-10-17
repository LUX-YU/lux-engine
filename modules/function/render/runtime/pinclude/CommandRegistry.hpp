#pragma once

#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>

namespace lux::render::runtime_detail
{
    struct SupportedHandler final
    {
        HandlerId id;
        HandlerCall call;
    };

    struct CommandMetadata final
    {
        CommandId id;
        CommandBinding binding;
        std::vector<SupportedHandler> supported;
        HandlerId selected;
    };

    struct CommandEntry final
    {
        HandlerCall active;
        std::unique_ptr<CommandMetadata> cold;
    };

    class CommandRegistry final
    {
    public:
        using HandlerEntry = RenderServer::HandlerEntry;
        CommandId registerCommand(CommandBinding) noexcept;
        RenderResult<CommandId> findCommand(const CommandDescriptor&) const noexcept;
        RenderResult<HandlerId> findHandler(CommandValueType, std::string_view) const noexcept;
        RenderResult<std::vector<CommandId>> prepareHandler(const HandlerEntry&, std::span<const CommandBinding>)
            const noexcept;
        HandlerId
            registerHandler(std::unique_ptr<HandlerEntry>, std::span<const CommandBinding>, std::vector<CommandId>) noexcept;
        CommandList listCommands() const noexcept;

        [[nodiscard]] const CommandEntry* findCommand(CommandId id) const noexcept
        {
            return command_entries_.find(id);
        }

        [[nodiscard]] HandlerEntry* findHandler(HandlerId id) noexcept
        {
            return handler_owners_.find(id)->get();
        }

        void listCommandBindings(const ListCommandBindings::Request&, ListCommandBindings::Response&) noexcept;
        void selectHandler(const SelectCommandBinding::Request&, SelectCommandBinding::Response&) noexcept;

        void clear() noexcept
        {
            command_entries_.clear();
            handler_owners_.clear();
        }

    private:
        cxx::SlotMap<CommandEntry, CommandTag> command_entries_;
        cxx::SlotMap<std::unique_ptr<HandlerEntry>, HandlerTag> handler_owners_;
    };
} // namespace lux::render::runtime_detail
