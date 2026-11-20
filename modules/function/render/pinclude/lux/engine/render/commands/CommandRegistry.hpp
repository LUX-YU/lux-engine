#pragma once

#include <lux/engine/render/commands/DiscoveryCommands.hpp>
#include <lux/engine/render/extensions/CommandBinding.hpp>
#include <lux/engine/render/extensions/FrameDataCall.hpp>
#include <lux/engine/render/extensions/detail/HandlerRecord.hpp>
#include <map>

namespace lux::render::runtime_detail
{
    struct HandlerBinding final
    {
        HandlerId handler;
        HandlerCall call;
        cxx::basic_type_info method_type;
        cxx::basic_type_info storage_type;
        cxx::basic_type_info effect_state_type;

        [[nodiscard]] bool callable() const noexcept
        {
            return call.invoke && (call.object || (handler.isNull() && !effect_state_type.isValid()));
        }
    };

    struct CommandRecord final
    {
        // Command definitions and their error constructors have module lifetime.
        const CommandDescriptor* description;
        void (*make_error)(platform::ipc::IpcMessage&, RenderError) noexcept;
        std::vector<HandlerBinding> bindings;
        EffectId (*effect)(const platform::ipc::IpcMessage&) noexcept = nullptr;
        ResourceInstanceId (*storage)(const platform::ipc::IpcMessage&) noexcept = nullptr;
        graph::NodeId (*effect_node)(const platform::ipc::IpcMessage&) noexcept = nullptr;

        [[nodiscard]] const HandlerBinding* binding(std::uint32_t index) const noexcept
        {
            return index < bindings.size() ? &bindings[index] : nullptr;
        }

    };

    class CommandRegistry final
    {
    public:
        CommandId registerCommand(CommandBinding) noexcept;
        [[nodiscard]] RenderResult<void>
        validateStaticBindings(std::span<const CommandBinding>) const noexcept;
        void publishStaticBindings(std::span<const CommandBinding>) noexcept;
        [[nodiscard]] RenderResult<void> validateResourceBindings(std::span<const CommandBinding>) const noexcept;
        [[nodiscard]] CommandBindingLocation publishResourceBinding(const CommandBinding&) noexcept;
        RenderResult<CommandId> findCommand(const CommandDescriptor&) const noexcept;
        RenderResult<HandlerId> findHandler(cxx::basic_type_info, std::string_view) const noexcept;
        // Builtin and external definitions share qualification and publication.
        RenderResult<void>
        validateHandler(const HandlerRecord&, std::span<const CommandBinding>)
            const noexcept;
        HandlerId publishHandler(HandlerRecord) noexcept;
        CommandBindingLocation publishBinding(HandlerId, const CommandBinding&) noexcept;
        [[nodiscard]] RenderResult<CommandBindingLocation>
        findBinding(CommandId, cxx::basic_type_info method, HandlerId) const noexcept;
        [[nodiscard]] const HandlerBinding* binding(CommandBindingLocation) const noexcept;
        void activateHandlers(ResourceRegistry&) noexcept;
        CommandList listCommands() const noexcept;
        void queryCommandIds(const QueryCommandIds::Request&, QueryCommandIds::Response&) const noexcept;

        void reserve(std::size_t added_commands, std::size_t added_handlers)
        {
            commands_.reserve(commands_.size() + added_commands);
            handlers_.reserve(handlers_.size() + added_handlers);
        }

        [[nodiscard]] const CommandRecord* findCommand(CommandId id) const noexcept
        {
            return commands_.find(id);
        }

        [[nodiscard]] HandlerRecord* findHandler(HandlerId id) noexcept
        {
            const auto* entry = handlers_.find(id);
            return entry ? entry->get() : nullptr;
        }

        [[nodiscard]] const HandlerRecord* findHandler(HandlerId id) const noexcept
        {
            const auto* entry = handlers_.find(id);
            return entry ? entry->get() : nullptr;
        }


    private:
        void activateHandler(HandlerId, ResourceRegistry&) noexcept;

        // Handler records have stable addresses; these views borrow their owned names.
        // Setup and Server-owner access only; runtime discovery is an IPC command.
        using HandlerName = std::pair<std::string_view, std::string_view>;
        cxx::SlotMap<CommandRecord, CommandTag> commands_;
        cxx::SlotMap<std::unique_ptr<HandlerRecord>, HandlerTag> handlers_;
        std::map<std::string_view, CommandId, std::less<>> commands_by_name_;
        std::map<HandlerName, HandlerId> handlers_by_name_;
    };
} // namespace lux::render::runtime_detail
