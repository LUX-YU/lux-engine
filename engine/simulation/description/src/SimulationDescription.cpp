#include <lux/engine/simulation/SimulationDescription.hpp>

#include <algorithm>
#include <limits>
#include <memory>

namespace lux::simulation
{
    namespace
    {
        void addRetainedBytes(std::size_t& total, std::size_t value) noexcept
        {
            if (value > std::numeric_limits<std::size_t>::max() - total)
            {
                total = std::numeric_limits<std::size_t>::max();
                return;
            }
            total += value;
        }

        void addRetainedArray(std::size_t& total, std::size_t count, std::size_t element_size) noexcept
        {
            if (count != 0U && element_size > std::numeric_limits<std::size_t>::max() / count)
            {
                total = std::numeric_limits<std::size_t>::max();
                return;
            }
            addRetainedBytes(total, count * element_size);
        }
    }

    SimulationDataView::SimulationDataView(const SimulationDescription& description, std::size_t data_index) noexcept
        : description_(&description), data_index_(data_index)
    {
    }

    SimulationDataView::operator bool() const noexcept
    {
        return description_ != nullptr && data_index_ < description_->data_.size();
    }

    const SimulationDataSchemaId& SimulationDataView::schema() const noexcept
    {
        return description_->schemas_[description_->data_[data_index_].schema_ordinal];
    }

    std::uint32_t SimulationDataView::version() const noexcept
    {
        return description_->data_[data_index_].version;
    }

    std::span<const std::byte> SimulationDataView::payload() const noexcept
    {
        const auto& record = description_->data_[data_index_];
        return std::span<const std::byte>(description_->payload_).subspan(record.payload_offset, record.payload_size);
    }

    SimulationSystemView::SimulationSystemView(
        const SimulationDescription& description,
        std::size_t system_index
    ) noexcept
        : description_(&description), system_index_(system_index)
    {
    }

    SimulationSystemView::operator bool() const noexcept
    {
        return description_ != nullptr && system_index_ < description_->systems_.size();
    }

    std::string_view SimulationSystemView::instanceName() const noexcept
    {
        return description_->systems_[system_index_].instance_name;
    }

    const SystemTypeId& SimulationSystemView::type() const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        return description_->system_types_[type].type;
    }

    std::uint32_t SimulationSystemView::version() const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        return description_->system_types_[type].version;
    }

    std::string_view SimulationSystemView::configurationSchemaName() const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        return description_->system_types_[type].configuration_schema_name;
    }

    std::uint64_t SimulationSystemView::configurationSchemaHash() const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        return description_->system_types_[type].configuration_schema_hash;
    }

    std::uint32_t SimulationSystemView::configurationSchemaVersion() const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        return description_->system_types_[type].configuration_schema_version;
    }

    std::span<const std::byte> SimulationSystemView::configurationPayload() const noexcept
    {
        const auto& system = description_->systems_[system_index_];
        return std::span<const std::byte>(description_->configuration_payload_)
            .subspan(system.configuration_offset, system.configuration_size);
    }

    std::size_t SimulationSystemView::capabilityCount() const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        return description_->system_types_[type].capabilities.size();
    }

    std::string_view SimulationSystemView::capabilityAt(std::size_t index) const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        const auto& capabilities = description_->system_types_[type].capabilities;
        return index < capabilities.size() ? std::string_view{capabilities[index]} : std::string_view{};
    }

    bool SimulationSystemView::hasCapability(std::string_view name) const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        const auto& capabilities = description_->system_types_[type].capabilities;
        return std::find(capabilities.begin(), capabilities.end(), name) != capabilities.end();
    }

    std::size_t SimulationSystemView::hookPointCount() const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        return description_->system_types_[type].hooks.size();
    }

    SimulationHookPointView SimulationSystemView::hookPointAt(std::size_t index) const noexcept
    {
        return index < hookPointCount() ? SimulationHookPointView(*description_, system_index_, index)
                                        : SimulationHookPointView{};
    }

    SimulationHookPointView SimulationSystemView::findHookPoint(std::string_view name) const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        const auto& hooks = description_->system_types_[type].hooks;
        const auto found =
            std::find_if(hooks.begin(), hooks.end(), [name](const auto& hook) noexcept { return hook.name == name; });
        return found != hooks.end() ? SimulationHookPointView(
                                          *description_,
                                          system_index_,
                                          static_cast<std::size_t>(std::distance(hooks.begin(), found))
        )
                                    : SimulationHookPointView{};
    }

    std::size_t SimulationSystemView::eventCount() const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        return description_->system_types_[type].events.size();
    }

    SimulationEventView SimulationSystemView::eventAt(std::size_t index) const noexcept
    {
        return index < eventCount() ? SimulationEventView(*description_, system_index_, index) : SimulationEventView{};
    }

    SimulationEventView SimulationSystemView::findEvent(std::string_view name) const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        const auto& events = description_->system_types_[type].events;
        const auto found = std::find_if(events.begin(), events.end(), [name](const auto& event) noexcept {
            return event.name == name;
        }
        );
        return found != events.end() ? SimulationEventView(
                                           *description_,
                                           system_index_,
                                           static_cast<std::size_t>(std::distance(events.begin(), found))
        )
                                     : SimulationEventView{};
    }

    SimulationHookPointView::SimulationHookPointView(
        const SimulationDescription& description,
        std::size_t system_index,
        std::size_t hook_index
    ) noexcept
        : description_(&description), system_index_(system_index), hook_index_(hook_index)
    {
    }

    SimulationHookPointView::operator bool() const noexcept
    {
        if (description_ == nullptr || system_index_ >= description_->systems_.size())
            return false;
        const auto type = description_->systems_[system_index_].type_ordinal;
        return hook_index_ < description_->system_types_[type].hooks.size();
    }

    SimulationSystemView SimulationHookPointView::system() const noexcept
    {
        return *this ? SimulationSystemView(*description_, system_index_) : SimulationSystemView{};
    }

    std::string_view SimulationHookPointView::name() const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        return description_->system_types_[type].hooks[hook_index_].name;
    }

    ESystemHookCardinality SimulationHookPointView::cardinality() const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        return description_->system_types_[type].hooks[hook_index_].cardinality;
    }

    std::size_t SimulationHookPointView::parameterCount() const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        return description_->system_types_[type].hooks[hook_index_].parameters.size();
    }

    lux::script::ScriptSemanticType SimulationHookPointView::parameterAt(std::size_t index) const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        const auto& parameters = description_->system_types_[type].hooks[hook_index_].parameters;
        if (index >= parameters.size())
            return {};
        const auto& parameter = parameters[index];
        return {parameter.type_id, parameter.canonical_name, parameter.pass};
    }

    std::size_t SimulationHookPointView::returnCount() const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        return description_->system_types_[type].hooks[hook_index_].returns.size();
    }

    lux::script::ScriptSemanticType SimulationHookPointView::returnAt(std::size_t index) const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        const auto& returns = description_->system_types_[type].hooks[hook_index_].returns;
        if (index >= returns.size())
            return {};
        const auto& result = returns[index];
        return {result.type_id, result.canonical_name, result.pass};
    }

    SimulationEventView::SimulationEventView(
        const SimulationDescription& description,
        std::size_t system_index,
        std::size_t event_index
    ) noexcept
        : description_(&description), system_index_(system_index), event_index_(event_index)
    {
    }

    SimulationEventView::operator bool() const noexcept
    {
        if (description_ == nullptr || system_index_ >= description_->systems_.size())
            return false;
        const auto type = description_->systems_[system_index_].type_ordinal;
        return event_index_ < description_->system_types_[type].events.size();
    }

    SimulationSystemView SimulationEventView::system() const noexcept
    {
        return *this ? SimulationSystemView(*description_, system_index_) : SimulationSystemView{};
    }

    std::string_view SimulationEventView::name() const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        return description_->system_types_[type].events[event_index_].name;
    }

    SimulationHookPointView SimulationEventView::dispatchHook() const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        const auto hook = description_->system_types_[type].events[event_index_].dispatch_hook_ordinal;
        return SimulationHookPointView(*description_, system_index_, hook);
    }

    ESystemEventTarget SimulationEventView::target() const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        return description_->system_types_[type].events[event_index_].target;
    }

    std::string_view SimulationEventView::payloadSchemaName() const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        return description_->system_types_[type].events[event_index_].payload_schema_name;
    }

    std::uint64_t SimulationEventView::payloadSchemaHash() const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        return description_->system_types_[type].events[event_index_].payload_schema_hash;
    }

    std::uint32_t SimulationEventView::payloadSchemaVersion() const noexcept
    {
        const auto type = description_->systems_[system_index_].type_ordinal;
        return description_->system_types_[type].events[event_index_].payload_schema_version;
    }

    SimulationDependencyView::SimulationDependencyView(
        const SimulationDescription& description,
        std::size_t dependency_index
    ) noexcept
        : description_(&description), dependency_index_(dependency_index)
    {
    }

    SimulationDependencyView::operator bool() const noexcept
    {
        return description_ != nullptr && dependency_index_ < description_->dependencies_.size();
    }

    SimulationSystemView SimulationDependencyView::before() const noexcept
    {
        const auto& dependency = description_->dependencies_[dependency_index_];
        return SimulationSystemView(*description_, dependency.before_system);
    }

    SimulationSystemView SimulationDependencyView::after() const noexcept
    {
        const auto& dependency = description_->dependencies_[dependency_index_];
        return SimulationSystemView(*description_, dependency.after_system);
    }

    SimulationGlobalScriptMountView::SimulationGlobalScriptMountView(
        const SimulationDescription& description,
        std::size_t mount_index
    ) noexcept
        : description_(&description), mount_index_(mount_index)
    {
    }

    SimulationGlobalScriptMountView::operator bool() const noexcept
    {
        return description_ != nullptr && mount_index_ < description_->global_script_mounts_.size();
    }

    ScriptMountId SimulationGlobalScriptMountView::id() const noexcept
    {
        return description_->global_script_mounts_[mount_index_].id;
    }

    const lux::asset::AssetId& SimulationGlobalScriptMountView::script() const noexcept
    {
        return description_->global_script_mounts_[mount_index_].script;
    }

    std::size_t SimulationGlobalScriptMountView::bindingCount() const noexcept
    {
        return description_->global_script_mounts_[mount_index_].bindings.size();
    }

    const ScriptBindingDescription* SimulationGlobalScriptMountView::bindingAt(std::size_t index) const noexcept
    {
        const auto& bindings = description_->global_script_mounts_[mount_index_].bindings;
        return index < bindings.size() ? std::addressof(bindings[index]) : nullptr;
    }

    bool SimulationDescription::empty() const noexcept
    {
        return data_.empty() && systems_.empty();
    }

    std::size_t SimulationDescription::dataCount() const noexcept
    {
        return data_.size();
    }
    std::size_t SimulationDescription::payloadBytes() const noexcept
    {
        return payload_.size();
    }
    std::size_t SimulationDescription::configurationPayloadBytes() const noexcept
    {
        return configuration_payload_.size();
    }

    std::size_t SimulationDescription::retainedBytes() const noexcept
    {
        std::size_t result{sizeof(SimulationDescription)};
        addRetainedArray(result, schemas_.capacity(), sizeof(SimulationDataSchemaId));
        addRetainedArray(result, data_.capacity(), sizeof(DataRecord));
        addRetainedArray(result, payload_.capacity(), sizeof(std::byte));
        addRetainedArray(result, system_types_.capacity(), sizeof(SystemTypeRecord));
        addRetainedArray(result, systems_.capacity(), sizeof(SystemRecord));
        addRetainedArray(result, configuration_payload_.capacity(), sizeof(std::byte));
        addRetainedArray(result, dependencies_.capacity(), sizeof(DependencyRecord));
        addRetainedArray(result, global_script_mounts_.capacity(), sizeof(ScriptMountDescription));
        for (const auto& schema : schemas_)
            addRetainedArray(result, schema.name.capacity(), sizeof(char));
        for (const auto& system : systems_)
            addRetainedArray(result, system.instance_name.capacity(), sizeof(char));
        for (const auto& type : system_types_)
        {
            addRetainedArray(result, type.type.name.capacity(), sizeof(char));
            addRetainedArray(result, type.configuration_schema_name.capacity(), sizeof(char));
            addRetainedArray(result, type.capabilities.capacity(), sizeof(std::string));
            addRetainedArray(result, type.hooks.capacity(), sizeof(HookRecord));
            addRetainedArray(result, type.events.capacity(), sizeof(EventRecord));
            for (const auto& capability : type.capabilities)
                addRetainedArray(result, capability.capacity(), sizeof(char));
            for (const auto& hook : type.hooks)
            {
                addRetainedArray(result, hook.name.capacity(), sizeof(char));
                addRetainedArray(result, hook.parameters.capacity(), sizeof(SemanticTypeRecord));
                addRetainedArray(result, hook.returns.capacity(), sizeof(SemanticTypeRecord));
                for (const auto& parameter : hook.parameters)
                {
                    addRetainedArray(result, parameter.canonical_name.capacity(), sizeof(char));
                }
                for (const auto& return_type : hook.returns)
                {
                    addRetainedArray(result, return_type.canonical_name.capacity(), sizeof(char));
                }
            }
            for (const auto& event : type.events)
            {
                addRetainedArray(result, event.name.capacity(), sizeof(char));
                addRetainedArray(result, event.payload_schema_name.capacity(), sizeof(char));
            }
        }
        for (const auto& mount : global_script_mounts_)
        {
            addRetainedArray(result, mount.bindings.capacity(), sizeof(ScriptBindingDescription));
            for (const auto& binding : mount.bindings)
            {
                std::visit(
                    [&](const auto& target) noexcept {
                        using Target = std::remove_cvref_t<decltype(target)>;
                        if constexpr (std::is_same_v<Target, SystemHookBindingTarget>)
                        {
                            addRetainedArray(result, target.system_type.name.capacity(), sizeof(char));
                            addRetainedArray(result, target.system_instance.capacity(), sizeof(char));
                            addRetainedArray(result, target.hook.capacity(), sizeof(char));
                        }
                        else if constexpr (std::is_same_v<Target, SystemEventBindingTarget>)
                        {
                            addRetainedArray(result, target.system_type.name.capacity(), sizeof(char));
                            addRetainedArray(result, target.system_instance.capacity(), sizeof(char));
                            addRetainedArray(result, target.event.capacity(), sizeof(char));
                        }
                    },
                    binding.target
                );
            }
        }
        return result;
    }

    std::span<const SimulationDataSchemaId> SimulationDescription::schemas() const noexcept
    {
        return schemas_;
    }

    SimulationDataView SimulationDescription::dataAt(std::size_t index) const noexcept
    {
        return index < data_.size() ? SimulationDataView(*this, index) : SimulationDataView{};
    }

    SimulationDataView SimulationDescription::findData(const SimulationDataSchemaId& schema) const noexcept
    {
        if (!schema.valid())
            return {};
        const auto iterator = std::lower_bound(schemas_.begin(), schemas_.end(), schema, SimulationDataSchemaIdLess{});
        if (iterator == schemas_.end() || *iterator != schema)
            return {};
        return SimulationDataView(*this, static_cast<std::size_t>(std::distance(schemas_.begin(), iterator)));
    }

    std::size_t SimulationDescription::systemCount() const noexcept
    {
        return systems_.size();
    }

    SimulationSystemView SimulationDescription::systemAt(std::size_t index) const noexcept
    {
        return index < systems_.size() ? SimulationSystemView(*this, index) : SimulationSystemView{};
    }

    SimulationSystemView SimulationDescription::findSystem(std::string_view instance_name) const noexcept
    {
        const auto found = std::lower_bound(
            systems_.begin(),
            systems_.end(),
            instance_name,
            [](const SystemRecord& system, std::string_view name) noexcept { return system.instance_name < name; }
        );
        return found != systems_.end() && found->instance_name == instance_name
                   ? SimulationSystemView(*this, static_cast<std::size_t>(std::distance(systems_.begin(), found)))
                   : SimulationSystemView{};
    }

    bool SimulationDescription::hasCapability(std::string_view name) const noexcept
    {
        for (std::size_t index{}; index < systems_.size(); ++index)
        {
            if (SimulationSystemView(*this, index).hasCapability(name))
                return true;
        }
        return false;
    }

    SimulationHookPointView
    SimulationDescription::findHookPoint(std::string_view system_instance, std::string_view hook_name) const noexcept
    {
        const auto system = findSystem(system_instance);
        return system ? system.findHookPoint(hook_name) : SimulationHookPointView{};
    }

    SimulationEventView
    SimulationDescription::findEvent(std::string_view system_instance, std::string_view event_name) const noexcept
    {
        const auto system = findSystem(system_instance);
        return system ? system.findEvent(event_name) : SimulationEventView{};
    }

    std::size_t SimulationDescription::dependencyCount() const noexcept
    {
        return dependencies_.size();
    }

    SimulationDependencyView SimulationDescription::dependencyAt(std::size_t index) const noexcept
    {
        return index < dependencies_.size() ? SimulationDependencyView(*this, index) : SimulationDependencyView{};
    }

    std::size_t SimulationDescription::globalScriptMountCount() const noexcept
    {
        return global_script_mounts_.size();
    }

    SimulationGlobalScriptMountView SimulationDescription::globalScriptMountAt(std::size_t index) const noexcept
    {
        return index < global_script_mounts_.size() ? SimulationGlobalScriptMountView(*this, index)
                                                    : SimulationGlobalScriptMountView{};
    }
}
