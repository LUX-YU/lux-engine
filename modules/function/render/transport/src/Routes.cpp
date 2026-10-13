#include <lux/engine/render/transport/State.hpp>

namespace lux::render::detail
{
    OwnedSchema::OwnedSchema(const DataSchemaDescriptor& source) noexcept : name(source.type.name())
    {
        names.reserve(source.fields.size());
        enums.reserve(source.fields.size());
        fields.reserve(source.fields.size());
        for (const auto& field : source.fields)
        {
            names.emplace_back(field.name);
            enums.emplace_back(field.enum_values.begin(), field.enum_values.end());
        }
        for (std::size_t i = 0; i < source.fields.size(); ++i)
        {
            auto field = source.fields[i];
            field.name = names[i];
            field.enum_values = enums[i];
            fields.push_back(field);
        }
        descriptor = source;
        descriptor.type = name.view();
        descriptor.fields = fields;
    }

    RouteEntry::RouteEntry(
        const OperationDescriptor& source, void* context_value, OperationHandler handler_value
    ) noexcept
        : name(source.type.name()), payload(source.payload), context(context_value), handler(handler_value)
    {
        if (source.reply) reply = std::make_unique<OwnedSchema>(*source.reply);
        descriptor = source;
        descriptor.type = name.view();
        descriptor.payload = payload.descriptor;
        if (reply) descriptor.reply = reply->descriptor;
    }

    RenderResult<void> compatibleOperations(const OperationDescriptor& left, const OperationDescriptor& right) noexcept
    {
        const bool is_mismatch = left.type != right.type || left.version != right.version ||
            left.completion != right.completion || left.reply.has_value() != right.reply.has_value();
        if (is_mismatch) return cxx::unexpected(RenderError{kDataSchemaMismatch});
        if (auto checked = compatibleDataSchemas(left.payload, right.payload); !checked) return checked;
        if (left.reply) return compatibleDataSchemas(*left.reply, *right.reply);
        return {};
    }

    RenderResult<Binding> bindOperation(
        const Routes& routes, const OperationDescriptor& descriptor, const OperationDescriptor& typed
    ) noexcept
    {
        if (auto checked = compatibleOperations(descriptor, typed); !checked)
        {
            return cxx::unexpected(checked.error());
        }
        for (const auto& entry : routes.entries)
        {
            if (entry->descriptor.type != descriptor.type) continue;
            auto checked = compatibleOperations(entry->descriptor, descriptor);
            if (!checked) return cxx::unexpected(checked.error());
            return Binding{routes.identity, entry->id};
        }
        return cxx::unexpected(RenderError{kTransportStale});
    }
}

namespace lux::render
{
    RenderRouteTable::RenderRouteTable(std::uint32_t capacity) noexcept
        : routes_(std::make_shared<detail::Routes>(capacity)) {}

    RenderRouteTable::~RenderRouteTable() = default;

    RenderRouteTable::RenderRouteTable(RenderRouteTable&&) noexcept = default;

    RenderRouteTable& RenderRouteTable::operator=(RenderRouteTable&&) noexcept = default;

    RenderResult<OperationId> RenderRouteTable::registerOperation(
        const OperationDescriptor& descriptor, void* context, OperationHandler handler
    ) noexcept
    {
        if (!routes_ || routes_->frozen) return cxx::unexpected(RenderError{kTransportContract});
        const bool is_invalid_identity = !descriptor.type.isValid() ||
            descriptor.version.major == 0 || handler == nullptr;
        const bool is_invalid_completion = descriptor.completion != ECompletionContract::STATE_COMMITTED &&
            descriptor.completion != ECompletionContract::RESOURCE_GPU_READY &&
            descriptor.completion != ECompletionContract::RESULT_AVAILABLE;
        if (is_invalid_identity || is_invalid_completion) return cxx::unexpected(RenderError{kTransportContract});
        if (auto checked = validateDataSchema(descriptor.payload); !checked) return cxx::unexpected(checked.error());
        if (descriptor.reply)
        {
            if (auto checked = validateDataSchema(*descriptor.reply); !checked) return cxx::unexpected(checked.error());
        }
        for (const auto& entry : routes_->entries)
        {
            if (entry->descriptor.type == descriptor.type) return cxx::unexpected(RenderError{kTransportContract});
        }
        if (routes_->entries.size() >= routes_->capacity) return cxx::unexpected(RenderError{kTransportCapacity});
        auto id = routes_->entries.emplace(std::make_unique<detail::RouteEntry>(descriptor, context, handler));
        (*routes_->entries.find(id))->id = id;
        return id;
    }

    RenderResult<void> RenderRouteTable::remove(OperationId id) noexcept
    {
        if (!routes_ || routes_->frozen) return cxx::unexpected(RenderError{kTransportContract});
        if (!routes_->entries.erase(id)) return cxx::unexpected(RenderError{kTransportStale});
        return {};
    }

    RenderResult<void> RenderRouteTable::clear() noexcept
    {
        if (!routes_ || routes_->frozen) return cxx::unexpected(RenderError{kTransportContract});
        routes_->entries.clear();
        return {};
    }

    RenderResult<detail::Binding> RenderRouteTable::bindErased(
        const OperationDescriptor& descriptor, const OperationDescriptor& typed
    ) const noexcept
    {
        if (!routes_) return cxx::unexpected(RenderError{kTransportStopped});
        return detail::bindOperation(*routes_, descriptor, typed);
    }

    std::span<const error::ErrorDescriptor> renderTransportErrorDescriptors() noexcept
    {
        static constexpr error::ErrorDescriptor entries[]{
            {"lux.render.transport.capacity", "Transport capacity exhausted", error::ERecovery::RETRYABLE},
            {"lux.render.transport.contract", "Invalid operation contract", error::ERecovery::PERMANENT},
            {"lux.render.transport.owner", "Foreign endpoint", error::ERecovery::PERMANENT},
            {"lux.render.transport.stale", "Stale or already settled request", error::ERecovery::PERMANENT},
            {"lux.render.transport.stopped", "Endpoint stopped", error::ERecovery::PERMANENT},
            {"lux.render.transport.cancelled", "Operation cancelled without completion", error::ERecovery::PERMANENT},
            {"lux.render.transport.busy", "Transport contention or recursive poll", error::ERecovery::RETRYABLE},
            {"lux.render.transport.thread", "Wrong SPSC thread", error::ERecovery::PERMANENT},
            {"lux.render.transport.budget", "Bulk byte budget exhausted", error::ERecovery::RETRYABLE}
        };
        return entries;
    }
}
