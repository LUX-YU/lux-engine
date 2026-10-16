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

    void RenderServer::Impl::addCommand(
        CommandRegistration binding,
        CommandTransport transport,
        const StorageDescriptor* storage,
        std::shared_ptr<const void> pin
    ) noexcept
    {
        binding.bindNative(device.get());
        // Full registration comparison proved equivalence; retain the template's static contract.
        auto id = command_table.emplace(CommandEntry{{}, binding, transport, std::move(pin), storage});
        command_table.find(id)->id = id;
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
            if ((handler.descriptor()->completion == ECompletionContract::RESOURCE_GPU_READY) !=
                handler.gpuCompletion())
                return cxx::unexpected(RenderError{kProtocolContract});
            for (const auto& old : command_table)
                if (old.binding.descriptor()->name.name() == handler.descriptor()->name.name())
                    return cxx::unexpected(RenderError{kFeatureDuplicate});
            for (std::size_t previous = 0; previous < i; ++previous)
                if (handlers[previous].descriptor()->name.name() == handler.descriptor()->name.name())
                    return cxx::unexpected(RenderError{kFeatureDuplicate});
        }
        return {};
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
            s.addCommand(feature.handlers[i], transports[i], nullptr, feature.code_pin);
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

    RenderResult<void> RenderServer::Impl::registerStorage(
        StorageRegistration storage, std::span<const CommandTransport> transports
    ) noexcept
    {
        if (started.load() || !storage.description) return cxx::unexpected(RenderError{kRuntimeState});
        if (auto valid = validateStorage(*storage.description); !valid) return valid;
        const std::uint32_t capabilities =
            (device->device().textureDescriptors() ? 1u : 0u) | (device->device().textureCompressionBc() ? 2u : 0u);
        if ((storage.description->required_capabilities & ~capabilities) != 0)
            return cxx::unexpected(RenderError{kStorageCapability});
        if (auto valid = validateStorageFactory(*storage.description, storage.factory); !valid)
            return valid;
        const StorageCppType config_type{storage.configuration.type(),
            storage.configuration.objectSize(), storage.configuration.objectAlignment()};
        if (storage.configuration && config_type != storage.factory.config_type)
            return cxx::unexpected(RenderError{kStorageMismatch});
        for (const auto& existing : storage_types)
        {
            if (existing.description->type != storage.description->type) continue;
            if (auto match = compatibleStorage(*existing.description, *storage.description); !match) return match;
            return cxx::unexpected(RenderError{kFeatureDuplicate});
        }
        if (auto valid = validateHandlers(storage.handlers, transports); !valid) return valid;
        for (std::size_t i = 0; i < storage.handlers.size(); ++i)
            addCommand(storage.handlers[i], transports[i], storage.description, storage.factory.code_pin);
        storage_types.push_back(std::move(storage));
        return {};
    }

    RenderResult<void> RenderServer::registerStorageImpl(
        StorageRegistration storage, std::span<const CommandTransport> transports
    ) noexcept
    {
        return impl_->registerStorage(std::move(storage), transports);
    }
} // namespace lux::render
