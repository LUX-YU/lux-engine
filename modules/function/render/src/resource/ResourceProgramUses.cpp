#include <lux/engine/render/resource/ResourceProgramUses.hpp>

#include <lux/engine/render/device/CompiledPrograms.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <algorithm>

namespace lux::render
{
    ResourceProgramUses::~ResourceProgramUses() noexcept
    {
        const auto at = [&](std::size_t i) noexcept { return programs_[i]; };
        device_.programs().release(programs_.size(), at, {});
    }

    void ResourceProgramUses::remember(detail::NativeProgramId id) noexcept
    {
        if (std::ranges::find(programs_, id) != programs_.end()) return;
        device_.programs().retainLive(id);
        programs_.push_back(id);
    }

    void* ResourceProgramSource::borrow(
        const ResourceRegistry& registry, ResourceInstanceId resource, cxx::basic_type_info type
    ) noexcept
    {
        const auto storage = registry.borrow(resource, type);
        return storage ? *storage : nullptr;
    }

    ResourceProgramUses*
    ResourceProgramSource::find(const ResourceRegistry& registry, const detail::DeviceSession& device) const noexcept
    {
        if (!resolve_ || registry.deviceOf(resource_) != &device) return nullptr;
        auto* uses = resolve_(registry, *this);
        return uses && &uses->device_ == &device ? uses : nullptr;
    }

    void ResourceProgramSource::retainAdopted(
        const ResourceRegistry& registry, const detail::DeviceSession& device, detail::NativeProgramId id
    ) const noexcept
    {
        if (auto* uses = find(registry, device)) uses->remember(id);
    }

    bool ResourceProgramSource::available(
        const ResourceRegistry& registry, const detail::DeviceSession& device
    ) const noexcept
    {
        return find(registry, device) != nullptr;
    }
} // namespace lux::render
