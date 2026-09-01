#include <lux/engine/scene/SceneDescription.hpp>

#include <algorithm>

namespace lux::scene
{
    SceneRequirementBindingView::SceneRequirementBindingView(
        const SceneDescription& description,
        std::size_t binding_index
    ) noexcept
        : description_(&description), binding_index_(binding_index)
    {
    }

    SceneRequirementBindingView::operator bool() const noexcept
    {
        return description_ != nullptr && binding_index_ < description_->requirement_bindings_.size();
    }

    system::SystemInstanceId SceneRequirementBindingView::system() const noexcept
    {
        const auto& binding = description_->requirement_bindings_[binding_index_];
        return description_->systems_[binding.system_ordinal].id;
    }

    std::string_view SceneRequirementBindingView::requirement() const noexcept
    {
        return description_->requirement_bindings_[binding_index_].requirement;
    }

    std::string_view SceneRequirementBindingView::provider() const noexcept
    {
        return description_->requirement_bindings_[binding_index_].provider;
    }

    SceneSystemDependencyView::SceneSystemDependencyView(
        const SceneDescription& description,
        std::size_t dependency_index
    ) noexcept
        : description_(&description), dependency_index_(dependency_index)
    {
    }

    SceneSystemDependencyView::operator bool() const noexcept
    {
        return description_ != nullptr && dependency_index_ < description_->dependencies_.size();
    }

    system::SystemInstanceId SceneSystemDependencyView::before() const noexcept
    {
        return description_->systems_[description_->dependencies_[dependency_index_].before_system].id;
    }

    system::SystemInstanceId SceneSystemDependencyView::after() const noexcept
    {
        return description_->systems_[description_->dependencies_[dependency_index_].after_system].id;
    }

    SceneSystemView::SceneSystemView(const SceneDescription& description, std::size_t system_index) noexcept
        : description_(&description), system_index_(system_index)
    {
    }

    SceneSystemView::operator bool() const noexcept
    {
        return description_ != nullptr && system_index_ < description_->systems_.size();
    }

    system::SystemInstanceId SceneSystemView::instanceId() const noexcept
    {
        return description_->systems_[system_index_].id;
    }

    std::string_view SceneSystemView::instanceName() const noexcept
    {
        return description_->systems_[system_index_].instance_name;
    }

    const system::SystemTypeId& SceneSystemView::type() const noexcept
    {
        return description_->systems_[system_index_].type;
    }

    std::uint32_t SceneSystemView::version() const noexcept
    {
        return description_->systems_[system_index_].version;
    }

    std::string_view SceneSystemView::configurationSchemaName() const noexcept
    {
        return description_->systems_[system_index_].configuration_schema_name;
    }

    std::uint64_t SceneSystemView::configurationSchemaHash() const noexcept
    {
        return description_->systems_[system_index_].configuration_schema_hash;
    }

    std::uint32_t SceneSystemView::configurationSchemaVersion() const noexcept
    {
        return description_->systems_[system_index_].configuration_schema_version;
    }

    std::span<const std::byte> SceneSystemView::configurationPayload() const noexcept
    {
        const auto& system = description_->systems_[system_index_];
        return std::span<const std::byte>(description_->configuration_payload_).subspan(
            system.configuration_offset,
            system.configuration_size
        );
    }

    std::size_t SceneSystemView::requirementBindingCount() const noexcept
    {
        return static_cast<std::size_t>(std::count_if(
            description_->requirement_bindings_.begin(),
            description_->requirement_bindings_.end(),
            [this](const auto& binding) noexcept { return binding.system_ordinal == system_index_; }
        ));
    }

    SceneRequirementBindingView SceneSystemView::requirementBindingAt(std::size_t index) const noexcept
    {
        for (std::size_t ordinal{}; ordinal < description_->requirement_bindings_.size(); ++ordinal)
        {
            if (description_->requirement_bindings_[ordinal].system_ordinal != system_index_)
            {
                continue;
            }
            if (index == 0U)
            {
                return SceneRequirementBindingView(*description_, ordinal);
            }
            --index;
        }
        return {};
    }

    SceneRequirementBindingView SceneSystemView::findRequirementBinding(std::string_view requirement) const noexcept
    {
        for (std::size_t ordinal{}; ordinal < description_->requirement_bindings_.size(); ++ordinal)
        {
            const auto& binding = description_->requirement_bindings_[ordinal];
            if (binding.system_ordinal == system_index_ && binding.requirement == requirement)
            {
                return SceneRequirementBindingView(*description_, ordinal);
            }
        }
        return {};
    }

    asset::AssetId SceneDescription::world() const noexcept
    {
        return world_;
    }

    asset::AssetId SceneDescription::simulation() const noexcept
    {
        return simulation_;
    }

    std::size_t SceneDescription::systemCount() const noexcept
    {
        return systems_.size();
    }

    SceneSystemView SceneDescription::systemAt(std::size_t index) const noexcept
    {
        return index < systems_.size() ? SceneSystemView(*this, index) : SceneSystemView{};
    }

    SceneSystemView SceneDescription::findSystem(system::SystemInstanceId id) const noexcept
    {
        const auto found = system_ordinals_.find(id.value);
        return found != system_ordinals_.end() ? SceneSystemView(*this, found->second) : SceneSystemView{};
    }

    SceneSystemView SceneDescription::findSystem(std::string_view instance_name) const noexcept
    {
        const auto found = std::find_if(systems_.begin(), systems_.end(), [instance_name](const auto& system) noexcept {
            return system.instance_name == instance_name;
        });
        return found != systems_.end()
            ? SceneSystemView(*this, static_cast<std::size_t>(std::distance(systems_.begin(), found)))
            : SceneSystemView{};
    }

    std::size_t SceneDescription::dependencyCount() const noexcept
    {
        return dependencies_.size();
    }

    SceneSystemDependencyView SceneDescription::dependencyAt(std::size_t index) const noexcept
    {
        return index < dependencies_.size() ? SceneSystemDependencyView(*this, index) : SceneSystemDependencyView{};
    }
} // namespace lux::scene
