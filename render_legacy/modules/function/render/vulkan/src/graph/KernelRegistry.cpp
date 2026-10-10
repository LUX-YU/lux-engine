#include <limits>
#include <lux/engine/render/graph/KernelDescriptor.hpp>

namespace lux::render
{
    KernelRegistry& KernelRegistry::instance() noexcept
    {
        static KernelRegistry registry;
        return registry;
    }

    KernelRegistrationResult KernelRegistry::registerKernel(KernelRegistration registration) noexcept
    {
        const auto name = registration.canonical_name;
        const bool is_invalid_name = name.empty() || name.find_first_of(" \t\r\n") != std::string_view::npos ||
                                     name.find('\0') != std::string_view::npos;
        if (is_invalid_name)
        {
            return lux::cxx::unexpected(EKernelRegistrationError::INVALID_NAME);
        }
        const auto& descriptor = registration.descriptor;
        const std::string_view extension = descriptor.ext_slot_name ? descriptor.ext_slot_name : "";
        const std::lock_guard lock{mutex_};
        if (const auto found = name_to_id_.find(name); found != name_to_id_.end())
        {
            const auto id = found->second;
            const auto& existing = *find(id);
            const auto& old = existing.descriptor;
            const bool same_functions =
                old.emit == descriptor.emit && old.contribute_mesh == descriptor.contribute_mesh &&
                old.contribute_arena == descriptor.contribute_arena && old.replay == descriptor.replay &&
                old.resolve_patch == descriptor.resolve_patch;
            const bool same_extension = existing.extension_name == extension;
            if (!same_functions || !same_extension)
            {
                return lux::cxx::unexpected(EKernelRegistrationError::CONFLICT);
            }
            return id;
        }
        if (name_to_id_.size() == std::numeric_limits<KernelTypeId>::max())
        {
            return lux::cxx::unexpected(EKernelRegistrationError::CAPACITY);
        }
        FrameExtensionSlotId slot{};
        if (descriptor.ext_slot_name)
        {
            auto registered = FrameExtensionRegistry::instance().registerSlot(extension);
            if (!registered)
            {
                return lux::cxx::unexpected(
                    registered.error() == EFrameExtensionRegistrationError::CAPACITY
                        ? EKernelRegistrationError::EXTENSION_CAPACITY
                        : EKernelRegistrationError::INVALID_EXTENSION_NAME
                );
            }
            slot = *registered;
        }
        auto entry = std::make_unique<RegisteredKernel>();
        entry->code_lifetime = std::move(registration.code_lifetime);
        entry->extension_name = extension;
        entry->descriptor = descriptor;
        entry->descriptor.ext_slot_name = descriptor.ext_slot_name ? entry->extension_name.c_str() : nullptr;
        entry->extension_slot = slot;
        const auto id = static_cast<KernelTypeId>(name_to_id_.size() + 1);
        kernels_[id - 1] = std::move(entry);
        name_to_id_.emplace(name, id);
        published_count_.store(id, std::memory_order_release);
        return id;
    }

    lux::cxx::expected<void, EKernelRegistrationError> KernelRegistry::registerKernels(
        std::span<const KernelDeclaration> declarations,
        std::shared_ptr<const void> code_lifetime
    ) noexcept
    {
        for (const auto& declaration : declarations)
        {
            auto registered = registerKernel({declaration.canonical_name, declaration.descriptor, code_lifetime});
            if (!registered)
            {
                return lux::cxx::unexpected(registered.error());
            }
        }
        return {};
    }

    KernelTypeId KernelRegistry::idOf(std::string_view name) const noexcept
    {
        const std::lock_guard lock{mutex_};
        const auto found = name_to_id_.find(name);
        return found != name_to_id_.end() ? found->second : kInvalidKernelId;
    }

    const RegisteredKernel* KernelRegistry::find(KernelTypeId id) const noexcept
    {
        const auto count = published_count_.load(std::memory_order_acquire);
        return id != kInvalidKernelId && id <= count ? kernels_[id - 1].get() : nullptr;
    }
} // namespace lux::render
