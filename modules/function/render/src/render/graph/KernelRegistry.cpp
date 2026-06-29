#include <lux/engine/render/graph/KernelDescriptor.hpp>

namespace lux::render
{
    KernelRegistry& KernelRegistry::instance() noexcept
    {
        static KernelRegistry s_instance;
        return s_instance;
    }

    KernelTypeId KernelRegistry::registerKernel(std::string_view name, KernelDescriptor desc)
    {
        // §3: Auto-allocate extension slot if requested.
        if (desc.ext_slot_name)
        {
            std::string slot_name(desc.ext_slot_name);
            if (ext_name_to_slot_.find(slot_name) == ext_name_to_slot_.end())
                ext_name_to_slot_.emplace(std::move(slot_name), next_ext_slot_++);
        }

        auto it = name_to_id_.find(std::string(name));
        if (it != name_to_id_.end())
        {
            // Already registered — replace descriptor, keep original ID.
            descriptors_.insert(it->second, std::move(desc));
            return it->second;
        }
        const KernelTypeId id = next_id_++;
        descriptors_.insert(id, std::move(desc));
        name_to_id_.emplace(std::string(name), id);
        return id;
    }

    KernelTypeId KernelRegistry::idOf(std::string_view name) const noexcept
    {
        auto it = name_to_id_.find(std::string(name));
        return it != name_to_id_.end() ? it->second : kInvalidKernelId;
    }

    const KernelDescriptor* KernelRegistry::find(KernelTypeId id) const noexcept
    {
        if (id == kInvalidKernelId || !descriptors_.contains(id))
            return nullptr;
        return &descriptors_.at(id);
    }

    FrameExtensionSlotId KernelRegistry::extSlotOf(std::string_view name) const noexcept
    {
        auto it = ext_name_to_slot_.find(std::string(name));
        return it != ext_name_to_slot_.end() ? it->second : kInvalidExtSlot;
    }

} // namespace lux::render
