#pragma once

#include <limits>
#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/runtime/CommandBinding.hpp>
#include <lux/engine/render/runtime/detail/HandlerRecord.hpp>
#include <map>

namespace lux::render::runtime_detail
{
    struct HandlerBinding final
    {
        HandlerId handler;
        HandlerCall call;
    };

    struct CommandRecord final
    {
        static constexpr std::size_t kUnbound = std::numeric_limits<std::size_t>::max();

        // Command definitions and their error constructors have module lifetime.
        const CommandDescriptor* description;
        void (*make_error)(platform::ipc::IpcMessage&, RenderError) noexcept;
        std::vector<HandlerBinding> bindings;
        std::size_t selected_binding = kUnbound;

        [[nodiscard]] const HandlerBinding* selectedBinding() const noexcept
        {
            return selected_binding == kUnbound ? nullptr : &bindings[selected_binding];
        }
    };

    class CommandRegistry final
    {
    public:
        CommandId registerCommand(CommandBinding) noexcept;
        RenderResult<CommandId> findCommand(const CommandDescriptor&) const noexcept;
        RenderResult<HandlerId> findHandler(cxx::TypeToken, std::string_view) const noexcept;
        // Validation is used only for external declarations. Publication is shared with built-ins.
        RenderResult<void> validateHandler(const HandlerRecord&, std::span<const CommandBinding>) const noexcept;
        HandlerId publishHandler(HandlerRecord) noexcept;
        void publishBinding(HandlerId, const CommandBinding&) noexcept;
        [[nodiscard]] HandlerId handlerId(cxx::TypeToken, std::string_view) const noexcept;
        void activateSelectedHandlers(ResourceRegistry&) noexcept;
        CommandList listCommands() const noexcept;

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

        void listCommandBindings(const ListCommandBindings::Request&, ListCommandBindings::Response&) noexcept;
        void
        selectHandler(const SelectCommandBinding::Request&, SelectCommandBinding::Response&, ResourceRegistry&) noexcept;

        void clear() noexcept
        {
            commands_by_name_.clear();
            handlers_by_name_.clear();
            commands_.clear();
            handlers_.clear();
        }

    private:
        void activateHandler(HandlerId, ResourceRegistry&) noexcept;

        // Handler records have stable addresses; these views borrow their owned names until clear().
        using HandlerName = std::pair<std::string_view, std::string_view>;
        cxx::SlotMap<CommandRecord, CommandTag> commands_;
        cxx::SlotMap<std::unique_ptr<HandlerRecord>, HandlerTag> handlers_;
        std::map<std::string_view, CommandId, std::less<>> commands_by_name_;
        std::map<HandlerName, HandlerId> handlers_by_name_;
    };
} // namespace lux::render::runtime_detail
