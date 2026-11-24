#pragma once

#include <lux/engine/render/runtime/commands/DiscoveryCommands.hpp>
#include <lux/engine/render/runtime/commands/CommandBinding.hpp>
#include <lux/engine/render/runtime/commands/detail/HandlerRecord.hpp>
#include <algorithm>
#include <deque>
#include <map>
#include <lux/engine/render/resource/ResourceId.hpp>

namespace lux::render { struct ImplementationEntry; }

namespace lux::render::runtime_detail
{
    struct HandlerBinding final
    {
        HandlerId handler;
        HandlerCall call;
        cxx::basic_type_info method_type;
        cxx::basic_type_info storage_type;
        cxx::basic_type_info effect_state_type;
        ResourceTypeId resource{};

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

        [[nodiscard]] const HandlerBinding* resourceBinding(ResourceTypeId type) const noexcept
        {
            const auto found = std::ranges::find(bindings, type, &HandlerBinding::resource);
            return found == bindings.end() ? nullptr : &*found;
        }

        [[nodiscard]] const HandlerBinding* binding(std::uint32_t index) const noexcept
        {
            return index < bindings.size() ? &bindings[index] : nullptr;
        }

    };

    class CommandRegistry final
    {
    public:
        template <RenderCommand C, auto Method, class Owner>
        CommandId registerRoutedCommand(Owner& owner) noexcept
        {
            CommandBinding binding;
            binding.initializeDeclaration<C>();
            binding.call_ = {
                [](void* object, CommandDispatchContext& context) noexcept
                {
                    auto& input = context.request.get<typename C::Request>();
                    (static_cast<Owner*>(object)->*Method)(std::move(input), context.response_route);
                },
                &owner
            };
            return registerCommand(std::move(binding));
        }

        void publishNodeCommands(const EffectCapability&, std::vector<CommandId>) noexcept;
        void publishImplementationBindings(const ImplementationEntry&, std::vector<CommandBindingLocation>) noexcept;
        [[nodiscard]] std::span<const CommandId> nodeCommands(const EffectCapability&) const noexcept;
        [[nodiscard]] std::span<const CommandBindingLocation> effectBindings(const ImplementationEntry&) const noexcept;

        CommandId registerCommand(CommandBinding) noexcept;
        [[nodiscard]] RenderResult<void>
        validateStaticBindings(std::span<const CommandBinding>) const noexcept;
        void publishStaticBindings(std::span<const CommandBinding>) noexcept;
        [[nodiscard]] RenderResult<void> validateResourceBindings(cxx::basic_type_info storage, std::span<const CommandBinding>) const noexcept;
        void publishResourceBinding(ResourceTypeId, const CommandBinding&) noexcept;
        RenderResult<HandlerId> findHandler(cxx::basic_type_info, std::string_view) const noexcept;
        // Builtin and external definitions share qualification and publication.
        RenderResult<void>
        validateHandler(const HandlerRecord&, std::span<const CommandBinding>)
            const noexcept;
        HandlerId publishHandler(HandlerRecord) noexcept;
        CommandBindingLocation publishBinding(HandlerId, const CommandBinding&) noexcept;
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

    private:
        void activateHandler(HandlerId, ResourceRegistry&) noexcept;

        // Handler records have stable addresses; these views borrow their owned names.
        // Setup and Server-owner access only; runtime discovery is an IPC command.
        std::map<const EffectCapability*, std::vector<CommandId>> node_commands_;
        std::map<const ImplementationEntry*, std::vector<CommandBindingLocation>> implementation_bindings_;
        std::deque<std::string> resource_names_;
        std::deque<CommandDescriptor> resource_descriptors_;
        using HandlerName = std::pair<std::string_view, std::string_view>;
        cxx::SlotMap<CommandRecord, CommandTag> commands_;
        cxx::SlotMap<std::unique_ptr<HandlerRecord>, HandlerTag> handlers_;
        std::map<std::string_view, CommandId, std::less<>> commands_by_name_;
        std::map<HandlerName, HandlerId> handlers_by_name_;
    };
} // namespace lux::render::runtime_detail
