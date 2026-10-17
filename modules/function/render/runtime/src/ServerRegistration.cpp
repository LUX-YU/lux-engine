#include "ServerImpl.hpp"

namespace lux::render
{
    RenderResult<void> RenderServer::Impl::listFeatures(RenderRequest& request) noexcept
    {
        FeatureList result;
        result.features.reserve(features.size());
        for (const auto& entry : features)
            result.features.push_back({entry.id, std::string(entry.definition.descriptor->type.name())});
        return request.reply(std::move(result));
    }

    RenderResult<void> RenderServer::Impl::listStorages(RenderRequest& request) noexcept
    {
        StorageList result;
        device->resources().forEachType([&](StorageTypeId id, const StorageDescriptor& type,
            std::uint32_t capabilities) noexcept {
            result.storages.push_back({id, std::string(type.type.name()), capabilities});
        });
        return request.reply(std::move(result));
    }

    CommandList RenderServer::Impl::commandDirectory() const noexcept
    {
        CommandList result;
        result.commands.reserve(command_table.size());
        for (const auto& route : command_table)
        {
            const auto& entry = *route.cold;
            const auto& descriptor = *entry.binding.descriptor();
            result.commands.push_back({entry.code_pin, std::string(descriptor.name.name()), entry.id,
                descriptor.request, descriptor.response, entry.transport.request, entry.transport.response,
                route.active.invoke != nullptr});
        }
        return result;
    }

    RenderResult<void> RenderServer::Impl::listCommands(RenderRequest& request) noexcept
    {
        return request.reply(commandDirectory());
    }

    RenderResult<void> RenderServer::Impl::listCommandBindings(RenderRequest& request) noexcept
    {
        const auto* entry = command_table.find(request.get<CommandId>());
        if (!entry) return cxx::unexpected(RenderError{kRuntimeIdentity});
        CommandBindings result{entry->cold->selected};
        result.handlers.reserve(entry->cold->supported.size());
        for (const auto& support : entry->cold->supported)
        {
            const auto& owner = **handlers.find(support.id);
            result.handlers.push_back({support.id, owner.name, owner.key});
        }
        return request.reply(std::move(result));
    }

    RenderResult<void> RenderServer::Impl::selectCommandBinding(RenderRequest& request) noexcept
    {
        const auto& input = request.get<CommandSelection>();
        auto* entry = command_table.find(input.command);
        if (!entry) return cxx::unexpected(RenderError{kRuntimeIdentity});
        if (input.handler.isNull())
        {
            entry->cold->selected = {};
            entry->active = {};
            entry->cold->active_code_pin = &entry->cold->code_pin;
            return request.reply();
        }
        const auto& supported = entry->cold->supported;
        const auto found = std::ranges::find(supported, input.handler, &SupportedHandler::id);
        if (found == supported.end()) return cxx::unexpected(RenderError{kRuntimeIdentity});
        entry->cold->selected = found->id;
        entry->active = found->call;
        entry->cold->active_code_pin = &(*handlers.find(found->id))->code_pin;
        return request.reply();
    }

    CommandId RenderServer::Impl::addCommand(
        CommandRegistration binding,
        CommandTransport transport,
        std::shared_ptr<const void> pin
    ) noexcept
    {
        binding.bindNative(device.get());
        // Full registration comparison proved equivalence; retain the template's static contract.
        auto metadata = std::make_unique<CommandMetadata>(
            CommandMetadata{{}, binding, transport, std::move(pin)}
        );
        auto* cold = metadata.get();
        const auto id = command_table.emplace(CommandEntry{binding.call(), std::move(metadata)});
        cold->id = id;
        cold->active_code_pin = &cold->code_pin;
        return id;
    }

    RenderResult<void> RenderServer::Impl::validateHandlers(
        std::span<const CommandRegistration> handlers, std::span<const CommandTransport> transports
    ) const noexcept
    {
        if (handlers.size() != transports.size()) return cxx::unexpected(RenderError{kFeatureContract});
        for (std::size_t i = 0; i < handlers.size(); ++i)
        {
            const auto& handler = handlers[i];
            if (!handler.valid()) return cxx::unexpected(RenderError{kFeatureContract});
            if (auto valid = validateCommand(*handler.descriptor()); !valid) return valid;
            if (auto valid = compatibleCommands(*handler.descriptor(), *transports[i].descriptor); !valid) return valid;
            for (const auto& old : command_table)
                if (old.cold->binding.descriptor()->name.name() == handler.descriptor()->name.name())
                    return cxx::unexpected(RenderError{kFeatureDuplicate});
            for (std::size_t previous = 0; previous < i; ++previous)
                if (handlers[previous].descriptor()->name.name() == handler.descriptor()->name.name())
                    return cxx::unexpected(RenderError{kFeatureDuplicate});
        }
        return {};
    }

    RenderResult<HandlerId> RenderServer::findHandler(CommandValueType type, std::string_view key) const noexcept
    {
        if (impl_->stop_requested.load()) return cxx::unexpected(RenderError{kRuntimeState});
        for (const auto& entry : impl_->handlers)
        {
            if (entry->type.type.name() != type.type.name() || entry->key != key) continue;
            if (entry->type != type) return cxx::unexpected(RenderError{kProtocolContract});
            return entry->id;
        }
        return HandlerId{};
    }

    RenderServer::HandlerEntry* RenderServer::handlerEntry(HandlerId id) noexcept
    {
        return impl_->handlers.find(id)->get();
    }

    vulkan::DeviceSession& RenderServer::nativeDevice() noexcept
    {
        return *impl_->device;
    }

    RenderResult<std::vector<CommandId>> RenderServer::prepareHandler(
        const HandlerEntry& owner, std::span<const CommandHandler> calls,
        std::span<const CommandTransport> transports
    ) const noexcept
    {
        const auto& state = *impl_;
        const bool is_invalid_installation = !owner.object || owner.name.empty() || calls.empty() ||
            calls.size() != transports.size() || state.stop_requested.load();
        if (is_invalid_installation) return cxx::unexpected(RenderError{kFeatureContract});
        const auto same_ops = [](const ipc::IpcTypeOps& left, const ipc::IpcTypeOps& right) noexcept {
            return left.token() == right.token() && left.size() == right.size() &&
                left.alignment() == right.alignment() && left.abi() == right.abi() &&
                left.usesInlineStorage() == right.usesInlineStorage();
        };
        std::vector<CommandId> commands(calls.size());
        for (std::size_t i = 0; i < calls.size(); ++i)
        {
            const auto& call = calls[i];
            if (!call.valid() || call.call().object != owner.object.get())
                return cxx::unexpected(RenderError{kFeatureContract});
            if (auto valid = validateCommand(*call.descriptor()); !valid)
                return cxx::unexpected(valid.error());
            if (auto valid = compatibleCommands(*call.descriptor(), *transports[i].descriptor); !valid)
                return cxx::unexpected(valid.error());
            for (std::size_t j = 0; j < i; ++j)
                if (calls[j].descriptor()->name.name() == call.descriptor()->name.name())
                    return cxx::unexpected(RenderError{kFeatureDuplicate});
            for (const auto& old : state.command_table)
            {
                if (old.cold->binding.descriptor()->name.name() != call.descriptor()->name.name()) continue;
                if (auto valid = compatibleCommands(*old.cold->binding.descriptor(), *call.descriptor()); !valid)
                    return cxx::unexpected(valid.error());
                const auto& transport = old.cold->transport;
                if (!same_ops(*transport.request, *transports[i].request) ||
                    !same_ops(*transport.response, *transports[i].response))
                    return cxx::unexpected(RenderError{kProtocolContract});
                commands[i] = old.cold->id;
                break;
            }
        }
        return commands;
    }

    HandlerId RenderServer::publishHandler(
        std::unique_ptr<HandlerEntry> owner, std::span<const CommandHandler> calls,
        std::span<const CommandTransport> transports, std::vector<CommandId> commands
    ) noexcept
    {
        auto& state = *impl_;
        // All recoverable qualification precedes publication. Ordinary heap exhaustion is fatal.
        auto* handler = owner.get();
        const auto id = state.handlers.emplace(std::move(owner));
        handler->id = id;
        for (std::size_t i = 0; i < calls.size(); ++i)
        {
            const bool first = commands[i].isNull();
            if (first) commands[i] = state.addCommand(calls[i], transports[i], handler->code_pin);
            auto& entry = *state.command_table.find(commands[i]);
            entry.cold->supported.push_back({id, calls[i].call()});
            if (first)
            {
                entry.cold->selected = id;
                entry.cold->active_code_pin = &handler->code_pin;
            }
            // Existing active remains unchanged, including a deliberately unbound command.
        }
        return id;
    }

    RenderResult<HandlerId> RenderServer::installHandler(
        std::unique_ptr<HandlerEntry> owner, std::span<const CommandHandler> calls,
        std::span<const CommandTransport> transports
    ) noexcept
    {
        auto commands = prepareHandler(*owner, calls, transports);
        if (!commands) return cxx::unexpected(commands.error());
        return publishHandler(std::move(owner), calls, transports, std::move(*commands));
    }

    RenderResult<StorageTypeId> RenderServer::installStorage(
        StorageFactory factory, ConfigValue config, bool (*equal)(const void*, const void*) noexcept,
        HandlerEntry* handler, std::unique_ptr<HandlerEntry> owner, std::span<const CommandHandler> calls,
        std::span<const CommandTransport> transports
    ) noexcept
    {
        auto& state = *impl_;
        if (state.stop_requested.load()) return cxx::unexpected(RenderError{kRuntimeState});
        std::vector<CommandId> commands;
        if (handler)
        {
            auto prepared = prepareHandler(*handler, calls, transports);
            if (!prepared) return cxx::unexpected(prepared.error());
            commands = std::move(*prepared);
        }
        auto& resources = state.device->resources();
        auto id = resources.typeId(factory.description->type);
        if (id.isNull())
        {
            auto installed = resources.installFactory(std::move(factory), std::move(config));
            if (!installed) return cxx::unexpected(installed.error());
            id = *installed;
        }
        else
        {
            const auto& previous = *resources.factory(id);
            if (auto valid = compatibleStorage(*previous.description, *factory.description); !valid)
                return cxx::unexpected(valid.error());
            const bool is_wrong_implementation = previous.native_type != factory.native_type ||
                previous.config_type != factory.config_type || previous.ops_type != factory.ops_type ||
                previous.create != factory.create || previous.ops != factory.ops ||
                previous.binding_ops != factory.binding_ops || previous.collect != factory.collect ||
                previous.compatible_config != factory.compatible_config;
            if (is_wrong_implementation) return cxx::unexpected(RenderError{kStorageMismatch});
            const auto* original = resources.configuration(factory.description->type);
            if (!original || !equal(original, config.data())) return cxx::unexpected(RenderError{kStorageMismatch});
        }
        // Registry admission and all Command qualifications succeeded. Only infallible publication remains.
        if (owner) publishHandler(std::move(owner), calls, transports, std::move(commands));
        return id;
    }

    RenderResult<FeatureId> RenderServer::Impl::registerFeature(
        FeatureRegistration feature, std::span<const CommandTransport> transports
    ) noexcept
    {
        auto& s = *this;
        const bool invalid = s.started.load() || !feature.descriptor || !feature.graph ||
                             (feature.graph && !feature.graph->describe) ||
                             (feature.descriptor && feature.descriptor->type.name().empty());
        if (invalid) return cxx::unexpected(RenderError{kRuntimeState});
        for (const auto& current : s.features)
            if (current.definition.descriptor->type.name() == feature.descriptor->type.name())
                return cxx::unexpected(RenderError{kFeatureDuplicate});
        if (auto valid = s.validateHandlers(feature.handlers, transports); !valid)
            return cxx::unexpected(valid.error());
        for (std::size_t i = 0; i < feature.handlers.size(); ++i)
            s.addCommand(feature.handlers[i], transports[i], feature.code_pin);
        const auto id = s.features.emplace(FeatureEntry{{}, std::move(feature)});
        s.features.find(id)->id = id;
        return id;
    }

    RenderResult<FeatureId> RenderServer::registerFeatureImpl(
        FeatureRegistration feature, std::span<const CommandTransport> transports
    ) noexcept
    {
        return impl_->registerFeature(std::move(feature), transports);
    }

} // namespace lux::render
