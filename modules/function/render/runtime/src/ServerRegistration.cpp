#include "ServerImpl.hpp"

namespace lux::render
{
    RenderResult<CommandId> RenderServer::findCommand(const CommandDescriptor& descriptor) const noexcept
    {
        for (const auto& entry : impl_->command_table)
        {
            const auto& stored = *entry.cold->binding.descriptor();
            if (stored.name.name() != descriptor.name.name()) continue;
            if (auto compatible = compatibleCommands(stored, descriptor); !compatible)
                return cxx::unexpected(compatible.error());
            return entry.cold->id;
        }
        return cxx::unexpected(RenderError{kRuntimeUnsupported});
    }

    namespace
    {
        bool sameFactory(const StorageFactory& left, const StorageFactory& right) noexcept
        {
            return left.native_type == right.native_type && left.config_type == right.config_type &&
                left.ops_type == right.ops_type && left.create == right.create && left.ops == right.ops &&
                left.binding_ops == right.binding_ops && left.compatible_config == right.compatible_config;
        }

        RenderResult<void> compatibleSupport(std::span<const CommandHandler> left, std::span<const CommandHandler> right) noexcept
        {
            for (std::size_t i = 0; i < left.size(); ++i)
                for (std::size_t j = 0; j < right.size(); ++j)
                {
                    if (left[i].descriptor()->name.name() != right[j].descriptor()->name.name()) continue;
                    if (auto valid = compatibleCommands(*left[i].descriptor(), *right[j].descriptor()); !valid)
                        return valid;

                }
            return {};
        }
    }

    RenderResult<void> RenderServer::Impl::listFeatures(RenderRequest& request) noexcept
    {
        FeatureList result;
        result.features.reserve(features.size());
        for (const auto& entry : features)
            result.features.push_back({
                entry.id, std::string(entry.descriptor.type.name()), std::string(entry.descriptor.config.token.name()),
                entry.descriptor.config.size, entry.descriptor.config.alignment
            });
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
            result.commands.push_back({std::string(descriptor.name.name()), entry.id,
                descriptor.request, descriptor.response,
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
        // Runtime-private controls have no installed Handler choice. In particular, disabling
        // ListCommands would remove the only discovery path for every future Client.
        if (entry->cold->supported.empty()) return cxx::unexpected(RenderError{kRuntimeUnsupported});
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
        CommandHandler binding,
        std::shared_ptr<const void> pin
    ) noexcept
    {
        // Full registration comparison proved equivalence; retain the template's static contract.
        auto metadata = std::make_unique<CommandMetadata>(
            CommandMetadata{{}, binding, std::move(pin)}
        );
        auto* cold = metadata.get();
        const auto id = command_table.emplace(CommandEntry{binding.call(), std::move(metadata)});
        cold->id = id;
        cold->active_code_pin = &cold->code_pin;
        return id;
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
        const HandlerEntry& owner, std::span<const CommandHandler> calls
    ) const noexcept
    {
        const auto& state = *impl_;
        const bool is_invalid_installation = !owner.object || owner.name.empty() || calls.empty() ||
            state.stop_requested.load();
        if (is_invalid_installation) return cxx::unexpected(RenderError{kFeatureContract});
        std::vector<CommandId> commands(calls.size());
        for (std::size_t i = 0; i < calls.size(); ++i)
        {
            const auto& call = calls[i];
            if (!call.valid() || call.call().object != owner.object.get())
                return cxx::unexpected(RenderError{kFeatureContract});
            if (auto valid = validateCommand(*call.descriptor()); !valid)
                return cxx::unexpected(valid.error());
            for (std::size_t j = 0; j < i; ++j)
                if (calls[j].descriptor()->name.name() == call.descriptor()->name.name())
                    return cxx::unexpected(RenderError{kFeatureDuplicate});
            for (const auto& old : state.command_table)
            {
                if (old.cold->binding.descriptor()->name.name() != call.descriptor()->name.name()) continue;
                if (auto valid = compatibleCommands(*old.cold->binding.descriptor(), *call.descriptor()); !valid)
                    return cxx::unexpected(valid.error());
                commands[i] = old.cold->id;
                break;
            }
        }
        return commands;
    }

    HandlerId RenderServer::publishHandler(
        std::unique_ptr<HandlerEntry> owner, std::span<const CommandHandler> calls, std::vector<CommandId> commands
    ) noexcept
    {
        auto& state = *impl_;
        // All recoverable qualification precedes publication. Ordinary heap exhaustion is fatal.
        auto* handler = owner.get();
        const auto id = state.handlers.emplace(std::move(owner));
        handler->id = id;
        for (std::size_t i = 0; i < calls.size(); ++i)
        {
            // Another qualified dependency may have published this previously absent command.
            if (commands[i].isNull())
                for (const auto& old : state.command_table)
                    if (old.cold->binding.descriptor()->name.name() == calls[i].descriptor()->name.name())
                    {
                        commands[i] = old.cold->id;
                        break;
                    }
            const bool first = commands[i].isNull();
            if (first) commands[i] = state.addCommand(calls[i], handler->code_pin);
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
        std::unique_ptr<HandlerEntry> owner, std::span<const CommandHandler> calls
    ) noexcept
    {
        auto commands = prepareHandler(*owner, calls);
        if (!commands) return cxx::unexpected(commands.error());
        return publishHandler(std::move(owner), calls, std::move(*commands));
    }

    RenderResult<StorageTypeId> RenderServer::prepareStorage(const StorageInstall& candidate, bool ensure) const noexcept
    {
        const auto& resources = impl_->device->resources();
        const auto& factory = candidate.factory;
        const auto id = resources.typeId(factory.description->type);
        if (id.isNull()) return id;
        const auto& previous = *resources.factory(id);
        if (auto valid = compatibleStorage(*previous.description, *factory.description); !valid)
            return cxx::unexpected(valid.error());
        if (!sameFactory(previous, factory)) return cxx::unexpected(RenderError{kStorageMismatch});
        // A Feature dependency honours an explicitly installed compatible configuration.
        if (!ensure)
        {
            const auto* original = resources.configuration(factory.description->type);
            if (!original || !candidate.equal(original, candidate.config.data()))
                return cxx::unexpected(RenderError{kStorageMismatch});
        }
        return id;
    }

    RenderResult<StorageTypeId> RenderServer::installStorage(StorageInstall candidate) noexcept
    {
        if (impl_->stop_requested.load()) return cxx::unexpected(RenderError{kRuntimeState});
        if (candidate.handler)
        {
            auto commands = prepareHandler(*candidate.handler, candidate.calls);
            if (!commands) return cxx::unexpected(commands.error());
            candidate.commands = std::move(*commands);
        }
        auto id = prepareStorage(candidate, false);
        if (!id) return cxx::unexpected(id.error());
        if (id->isNull())
        {
            id = impl_->device->resources().installFactory(std::move(candidate.factory), std::move(candidate.config));
            if (!id) return cxx::unexpected(id.error());
        }
        if (candidate.owner)
            publishHandler(std::move(candidate.owner), candidate.calls, std::move(candidate.commands));
        return *id;
    }

    RenderResult<FeatureId> RenderServer::installFeature(
        feature_detail::Implementation definition, std::shared_ptr<const void> code_pin,
        HandlerEntry* handler, std::unique_ptr<HandlerEntry> owner, std::span<const CommandHandler> calls, std::vector<StorageInstall> dependencies
    ) noexcept
    {
        auto& state = *impl_;
        const bool is_invalid_definition = !definition.descriptor || !definition.graph ||
            (definition.descriptor && definition.descriptor->type.name().empty()) ||
            (definition.graph && !definition.graph->describe);
        if (state.stop_requested.load()) return cxx::unexpected(RenderError{kRuntimeState});
        if (is_invalid_definition) return cxx::unexpected(RenderError{kFeatureContract});
        std::vector<CommandId> commands;
        if (handler)
        {
            auto prepared = prepareHandler(*handler, calls);
            if (!prepared) return cxx::unexpected(prepared.error());
            commands = std::move(*prepared);
        }
        FeatureId id;
        for (const auto& entry : state.features)
        {
            if (entry.descriptor.type.name() != definition.descriptor->type.name()) continue;
            const bool is_different_definition = entry.descriptor.config != definition.descriptor->config ||
                entry.graph.describe != definition.graph->describe || entry.graph.build != definition.graph->build ||
                entry.programs != definition.programs;
            if (is_different_definition) return cxx::unexpected(RenderError{kFeatureDuplicate});
            id = entry.id;
            break;
        }
        std::vector<StorageFactoryInstall> factories;
        factories.reserve(dependencies.size());
        for (std::size_t i = 0; i < dependencies.size(); ++i)
        {
            auto& candidate = dependencies[i];
            auto storage = prepareStorage(candidate, true);
            if (!storage) return cxx::unexpected(storage.error());
            bool duplicate = false;
            for (std::size_t j = 0; j < i; ++j)
            {
                const auto& previous = dependencies[j];
                if (previous.factory.description->type != candidate.factory.description->type) continue;
                if (auto valid = compatibleStorage(*previous.factory.description, *candidate.factory.description); !valid)
                    return cxx::unexpected(valid.error());
                if (!sameFactory(previous.factory, candidate.factory))
                    return cxx::unexpected(RenderError{kStorageMismatch});
                duplicate = true;
                break;
            }
            if (storage->isNull() && !duplicate)
                factories.push_back({candidate.factory, candidate.config});
            if (!candidate.handler) continue;
            auto prepared = prepareHandler(*candidate.handler, candidate.calls);
            if (!prepared) return cxx::unexpected(prepared.error());
            candidate.commands = std::move(*prepared);
            if (auto valid = compatibleSupport(calls, candidate.calls); !valid)
                return cxx::unexpected(valid.error());
            for (std::size_t j = 0; j < i; ++j)
                if (auto valid = compatibleSupport(dependencies[j].calls,
                    candidate.calls); !valid) return cxx::unexpected(valid.error());
        }
        // Registry performs its capability/config checks for every factory before any publication.
        if (auto installed = state.device->resources().installFactories(factories); !installed)
            return cxx::unexpected(installed.error());
        for (auto& candidate : dependencies)
            if (candidate.owner)
                publishHandler(std::move(candidate.owner), candidate.calls,
                    std::move(candidate.commands));
        // All recoverable checks are complete. Stable Feature entries survive later directory growth.
        if (id.isNull())
        {
            id = state.features.emplace(definition, std::move(code_pin));
            state.features.find(id)->id = id;
        }
        if (owner) publishHandler(std::move(owner), calls, std::move(commands));
        return id;
    }

} // namespace lux::render
